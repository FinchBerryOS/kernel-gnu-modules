// SPDX-License-Identifier: GPL-2.0
/*
 * fbppid.c
 *
 * Kernelmodul: Broker-only current-PPID query device.
 *
 * Ziel:
 *   - genau ein Device: /dev/fbppid
 *   - genau ein autorisierter Nutzer (Broker): fbportscore
 *   - PID 1 oder CAP_SYS_ADMIN registriert, welche PID der Broker ist
 *   - Broker kann aktuelle Parent-PID einer Ziel-PID abfragen
 *
 * Nicht enthalten:
 *   - Spawn-Events (→ fbspawn)
 *   - Vererbung
 *   - Portlogik
 *   - Queueing
 *   - ursprüngliche Spawn-Parent-Info
 *
 * Semantik:
 *   - Es wird die AKTUELLE Parent-PID (TGID-Ebene) zurückgegeben.
 *   - Wenn der Prozess bereits reparented wurde, ist das Ergebnis
 *     die aktuelle und nicht die ursprüngliche Parent-Beziehung.
 *   - PID 1 gibt ppid=0 zurück (init hat keinen echten Parent).
 *
 * ABI-Vertrag (Userspace):
 *   - __pad-Felder in ioctl-Structs MÜSSEN 0 sein (Zukunftssicherheit).
 *   - flags-Feld in query_args MUSS 0 sein.
 *   - ppid=0 bedeutet: kein Parent.  Das wird für PID 1 explizit
 *     erzwungen und ist eine fbppid-ABI-Regel, kein Kernel-Verhalten.
 *   - REGISTER_BROKER darf nur über einen Admin-fd aufgerufen werden
 *     (geöffnet von PID 1 oder CAP_SYS_ADMIN).
 *   - QUERY_PPID darf nur über einen Broker-fd aufgerufen werden
 *     (geöffnet vom registrierten Broker).
 *   - Admin-fds zählen NICHT gegen das Single-Reader-Limit.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/capability.h>
#include <linux/rcupdate.h>
#include <linux/cred.h>
#include <linux/errno.h>

/* ── ioctl-Definitionen ────────────────────────────────────────────────── */

#define FBPPID_IOC_MAGIC 'P'

struct fbppid_register_broker_args {
	__s32 pid;
	__u32 __pad;   /* Muss 0 sein – reserviert für spätere Erweiterung. */
};

struct fbppid_query_args {
	__s32 pid;     /* input:  Ziel-TGID */
	__s32 ppid;    /* output: aktuelle Parent-TGID */
	__u32 flags;   /* input:  muss 0 sein */
	__u32 __pad;   /* muss 0 sein */
};

#define FBPPID_IOC_REGISTER_BROKER \
	_IOW(FBPPID_IOC_MAGIC, 1, struct fbppid_register_broker_args)

#define FBPPID_IOC_QUERY_PPID \
	_IOWR(FBPPID_IOC_MAGIC, 2, struct fbppid_query_args)

/* ── fd-Rollen ─────────────────────────────────────────────────────────── */

/*
 * Jeder offene fd bekommt genau eine Rolle, gespeichert in
 * file->private_data.  Die Rolle bestimmt, welche ioctls erlaubt sind.
 *
 *   ADMIN  — PID 1 / CAP_SYS_ADMIN.  Darf nur REGISTER_BROKER.
 *            Zählt NICHT gegen das Broker-fd-Limit.
 *            Kann beliebig oft geöffnet/geschlossen werden.
 *
 *   BROKER — der registrierte Broker-Prozess.  Darf nur QUERY_PPID.
 *            Unterliegt dem Single-Reader-Limit (fbppid_open_owner).
 *
 * Die Rollen-Prüfung im ioctl ist eine NOTWENDIGE, aber NICHT HINREICHENDE
 * Bedingung.  Jeder ioctl-Handler prüft zusätzlich die Identität des
 * Aufrufers zum ioctl-Zeitpunkt (capable() bzw. is_broker_current()),
 * um fd-passing via SCM_RIGHTS / fork-Vererbung abzusichern.
 *
 * Sentinel-Werte statt 1/2 für bessere Crash-Diagnostik.
 */
#define FBPPID_ROLE_ADMIN   ((void *)0x46425001UL)  /* "FBP\x01" */
#define FBPPID_ROLE_BROKER  ((void *)0x46425002UL)  /* "FBP\x02" */

/* ── Broker-State ──────────────────────────────────────────────────────── */

static DEFINE_MUTEX(fbppid_broker_lock);

/*
 * Broker-PID wird RCU-geschützt gelesen und unter Mutex geschrieben.
 *
 * Schema:
 *   - Schreiber: fbppid_broker_lock + rcu_assign_pointer
 *   - Leser:     rcu_read_lock + rcu_dereference
 *   - Freigabe:  put_pid via call_rcu-Callback (verzögert)
 *
 * Damit ist der Hot-Path (QUERY_PPID → is_broker_current) lockfrei.
 */
static struct pid __rcu *fbppid_broker_pid_ref;

/* Owner-PID des aktuell offenen Broker-fd. */
static pid_t fbppid_open_owner = -1;

/* ── RCU-Callback für verzögerte pid-Freigabe ─────────────────────────── */

struct fbppid_rcu_pid {
	struct rcu_head rcu;
	struct pid     *pid_ref;
};

static void fbppid_rcu_put_pid(struct rcu_head *head)
{
	struct fbppid_rcu_pid *ctx =
		container_of(head, struct fbppid_rcu_pid, rcu);
	put_pid(ctx->pid_ref);
	kfree(ctx);
}

/*
 * Gibt eine alte Broker-PID-Referenz verzögert frei.
 * Muss NACH dem rcu_assign_pointer aufgerufen werden.
 * Wenn kzalloc fehlschlägt, warten wir synchron (sicher, aber langsamer).
 */
static void fbppid_defer_put_pid(struct pid *old)
{
	struct fbppid_rcu_pid *ctx;

	if (!old)
		return;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (ctx) {
		ctx->pid_ref = old;
		call_rcu(&ctx->rcu, fbppid_rcu_put_pid);
	} else {
		synchronize_rcu();
		put_pid(old);
	}
}

/* ── Broker-Prüfung (lockfrei, RCU-geschützt) ─────────────────────────── */

static bool fbppid_is_broker_current(void)
{
	struct pid *broker;
	bool match;

	rcu_read_lock();
	broker = rcu_dereference(fbppid_broker_pid_ref);
	match  = broker && task_tgid(current) == broker;
	rcu_read_unlock();

	return match;
}

/* ── Hilfsfunktionen ───────────────────────────────────────────────────── */

static int fbppid_validate_target_process(pid_t tgid,
					  struct task_struct **out_task)
{
	struct pid *pid_ref;
	struct task_struct *task;

	if (tgid <= 0)
		return -EINVAL;

	pid_ref = find_get_pid(tgid);
	if (!pid_ref)
		return -ESRCH;

	/*
	 * PIDTYPE_TGID: Liefert nur den Thread-Group-Leader.
	 * Ein Nebenthread mit zufällig gleicher numerischer PID wird
	 * nicht fälschlich als Prozess akzeptiert.
	 */
	task = get_pid_task(pid_ref, PIDTYPE_TGID);
	put_pid(pid_ref);

	if (!task)
		return -ESRCH;

	*out_task = task;
	return 0;
}

static pid_t fbppid_get_current_ppid_tgid(struct task_struct *task)
{
	pid_t ppid;

	/*
	 * ABI-Regel: ppid=0 bedeutet „kein Parent".
	 *
	 * Für PID 1 (init) erzwingen wir das explizit, statt uns darauf
	 * zu verlassen, dass real_parent auf swapper (TGID 0) zeigt.
	 * Das ist heute so, aber es ist ein Kernel-Implementierungsdetail,
	 * keine garantierte Schnittstelle.  Wir wollen die Semantik selbst
	 * kontrollieren.
	 */
	if (task_tgid_nr(task) == 1)
		return 0;

	/*
	 * real_parent unter RCU lesen.
	 * real_parent ist für alle echten Prozesse immer gesetzt.
	 * Die NULL-Prüfung ist rein defensiv.
	 */
	ppid = 0;
	rcu_read_lock();
	if (likely(task->real_parent))
		ppid = task_tgid_nr(task->real_parent);
	rcu_read_unlock();

	return ppid;
}

/* ── ioctl-Handler ─────────────────────────────────────────────────────── */

static long fbppid_ioctl_register_broker(struct file *file, unsigned long arg)
{
	struct fbppid_register_broker_args ua;
	struct pid *new_broker = NULL;
	struct pid *old_broker = NULL;
	struct task_struct *task = NULL;

	/* Rollen-Check: nur Admin-fds. */
	if (file->private_data != FBPPID_ROLE_ADMIN)
		return -EPERM;

	/*
	 * Identitäts-Check zum ioctl-Zeitpunkt.
	 *
	 * Die Rolle wurde beim open() vergeben.  Wenn der fd aber per
	 * SCM_RIGHTS oder fork() an einen unprivilegierten Prozess
	 * weitergereicht wurde, hat dieser zwar einen Admin-fd, aber
	 * keine Admin-Rechte.  Deshalb prüfen wir hier nochmal.
	 */
	if (task_tgid_nr(current) != 1 && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (copy_from_user(&ua, (void __user *)arg, sizeof(ua)))
		return -EFAULT;

	if (ua.__pad != 0)
		return -EINVAL;

	if (ua.pid <= 0)
		return -EINVAL;

	new_broker = find_get_pid((pid_t)ua.pid);
	if (!new_broker)
		return -ESRCH;

	/* PIDTYPE_TGID: nur Thread-Group-Leader akzeptieren. */
	task = get_pid_task(new_broker, PIDTYPE_TGID);
	if (!task) {
		put_pid(new_broker);
		return -ESRCH;
	}
	put_task_struct(task);

	mutex_lock(&fbppid_broker_lock);

	old_broker = rcu_dereference_protected(fbppid_broker_pid_ref,
		lockdep_is_held(&fbppid_broker_lock));

	rcu_assign_pointer(fbppid_broker_pid_ref, new_broker);
	fbppid_open_owner = -1;

	mutex_unlock(&fbppid_broker_lock);

	fbppid_defer_put_pid(old_broker);

	pr_info("fbppid: broker registered, pid=%d\n", ua.pid);

	return 0;
}

static long fbppid_ioctl_query_ppid(struct file *file, unsigned long arg)
{
	struct fbppid_query_args ua;
	struct task_struct *task = NULL;
	pid_t ppid;
	int ret;

	/* Rollen-Check: nur Broker-fds. */
	if (file->private_data != FBPPID_ROLE_BROKER)
		return -EPERM;

	/*
	 * Identitäts-Check zum ioctl-Zeitpunkt.
	 *
	 * Schützt gegen fd-passing: Wenn der Broker-fd per SCM_RIGHTS
	 * an einen anderen Prozess weitergereicht wurde, ist
	 * task_tgid(current) != broker → EPERM.
	 */
	if (!fbppid_is_broker_current())
		return -EPERM;

	if (copy_from_user(&ua, (void __user *)arg, sizeof(ua)))
		return -EFAULT;

	if (ua.flags != 0 || ua.__pad != 0)
		return -EINVAL;

	ret = fbppid_validate_target_process((pid_t)ua.pid, &task);
	if (ret)
		return ret;

	ppid = fbppid_get_current_ppid_tgid(task);
	put_task_struct(task);

	if (put_user(ppid, &((struct fbppid_query_args __user *)arg)->ppid))
		return -EFAULT;

	return 0;
}

/* ── file operations ───────────────────────────────────────────────────── */

static int fbppid_open(struct inode *inode, struct file *file)
{
	bool is_admin  = (task_tgid_nr(current) == 1 || capable(CAP_SYS_ADMIN));
	bool is_broker = fbppid_is_broker_current();
	int ret = 0;

	if ((file->f_flags & O_ACCMODE) != O_RDONLY)
		return -EINVAL;

	/*
	 * Zugangsregeln:
	 *
	 *   Admin (PID 1 / CAP_SYS_ADMIN):
	 *     → bekommt ROLE_ADMIN
	 *     → darf REGISTER_BROKER aufrufen
	 *     → zählt NICHT als Broker-fd (kein Limit)
	 *     → kann KEIN QUERY_PPID
	 *
	 *   Registrierter Broker:
	 *     → bekommt ROLE_BROKER
	 *     → darf QUERY_PPID aufrufen
	 *     → Single-Reader-Limit (ein fd gleichzeitig)
	 *     → kann KEIN REGISTER_BROKER
	 *
	 *   Alle anderen → EPERM
	 *
	 *   Wenn ein Prozess sowohl Admin als auch Broker ist (z.B. PID 1
	 *   registriert sich selbst), gewinnt Admin — das ist sinnvoller,
	 *   weil der Broker-fd-Pfad ein separater open()-Aufruf sein sollte.
	 */
	if (is_admin) {
		file->private_data = FBPPID_ROLE_ADMIN;
		return 0;
	}

	if (!is_broker)
		return -EPERM;

	mutex_lock(&fbppid_broker_lock);

	/* Re-Check unter Lock: Broker könnte gerade gewechselt haben. */
	if (!fbppid_is_broker_current()) {
		ret = -EPERM;
		goto out;
	}

	if (fbppid_open_owner > 0) {
		ret = -EBUSY;
		goto out;
	}

	fbppid_open_owner  = task_tgid_nr(current);
	file->private_data = FBPPID_ROLE_BROKER;

out:
	mutex_unlock(&fbppid_broker_lock);
	return ret;
}

static int fbppid_release(struct inode *inode, struct file *file)
{
	/* Admin-fds haben keinen Einfluss auf den Broker-State. */
	if (file->private_data != FBPPID_ROLE_BROKER)
		return 0;

	mutex_lock(&fbppid_broker_lock);

	if (fbppid_open_owner == task_tgid_nr(current))
		fbppid_open_owner = -1;

	mutex_unlock(&fbppid_broker_lock);
	return 0;
}

static long fbppid_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case FBPPID_IOC_REGISTER_BROKER:
		return fbppid_ioctl_register_broker(file, arg);

	case FBPPID_IOC_QUERY_PPID:
		return fbppid_ioctl_query_ppid(file, arg);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations fbppid_fops = {
	.owner          = THIS_MODULE,
	.open           = fbppid_open,
	.release        = fbppid_release,
	.read           = NULL,
	.write          = NULL,
	.unlocked_ioctl = fbppid_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.llseek         = no_llseek,
};

static struct miscdevice fbppid_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "fbppid",
	.fops  = &fbppid_fops,
	.mode  = 0400,
};

/* ── init / exit ───────────────────────────────────────────────────────── */

static int __init fbppid_init(void)
{
	int ret;

	ret = misc_register(&fbppid_miscdev);
	if (ret)
		return ret;

	pr_info("fbppid: loaded, /dev/fbppid ready\n");
	return 0;
}

static void __exit fbppid_exit(void)
{
	struct pid *old_broker;

	misc_deregister(&fbppid_miscdev);

	mutex_lock(&fbppid_broker_lock);
	old_broker = rcu_dereference_protected(fbppid_broker_pid_ref,
		lockdep_is_held(&fbppid_broker_lock));
	RCU_INIT_POINTER(fbppid_broker_pid_ref, NULL);
	fbppid_open_owner = -1;
	mutex_unlock(&fbppid_broker_lock);

	synchronize_rcu();
	if (old_broker)
		put_pid(old_broker);

	pr_info("fbppid: unloaded\n");
}

module_init(fbppid_init);
module_exit(fbppid_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("FinchBerryOS Contributors");
MODULE_DESCRIPTION("Broker-only current-PPID query device for fbportscore");