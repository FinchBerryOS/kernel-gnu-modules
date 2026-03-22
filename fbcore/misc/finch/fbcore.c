#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>

/**
 * FinchBerry OS Core Identity (fbcore)
 * ------------------------------------
 * Stellt System-Metadaten via /sys/kernel/finch bereit.
 *
 * Logik:
 * - hw_type kommt vom Bootloader (fbcore.hw_type=...)
 * - release/version kommen fest aus dem Kernel-Build (Kconfig)
 * - Fehlt hw_type oder ist ungültig -> mode = recovery
 *
 * Dieses Modul hat absichtlich keinen module_exit / Cleanup-Pfad.
 * Als subsys_initcall ist es fest im Kernel verankert und darf
 * zur Laufzeit nicht entladen werden — die Identity-Daten müssen
 * vom Boot bis zum Shutdown konsistent verfügbar bleiben.
 */

/* --- Kconfig Guards --- */
#ifndef CONFIG_FINCH_VERSION
#error "CONFIG_FINCH_VERSION is not defined — check your Kconfig/defconfig."
#endif

#ifndef CONFIG_FINCH_RELEASE
#error "CONFIG_FINCH_RELEASE is not defined — check your Kconfig/defconfig."
#endif

/* --- Erlaubte hw_type-Werte --- */
static const char * const valid_hw_types[] = {
	"sbc",
	"lwos",
	"egw",
};

/* --- Parameter vom Bootloader --- */
static char *hw_type;
module_param(hw_type, charp, 0444);
MODULE_PARM_DESC(hw_type, "Hardware target type (e.g., sbc, lwos, egw)");

/* --- Interne Status-Flags --- */
static bool is_recovery;

/* --- Hilfefunktionen --- */
static bool is_valid_hw_type(const char *type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(valid_hw_types); i++) {
		if (strcmp(type, valid_hw_types[i]) == 0)
			return true;
	}
	return false;
}

static const char *get_finch_mode(void)
{
	return is_recovery ? "recovery" : "production";
}

/* --- Sysfs Callback-Funktionen (Read-Only) --- */
static ssize_t hw_type_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", is_recovery ? "unknown" : hw_type);
}

static ssize_t version_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CONFIG_FINCH_VERSION);
}

static ssize_t release_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", CONFIG_FINCH_RELEASE);
}

static ssize_t official_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  IS_ENABLED(CONFIG_FINCH_OFFICIAL_BUILD) ? "yes" : "no");
}

static ssize_t mode_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", get_finch_mode());
}

/**
 * info_show — Sammeldatei für Pivot.
 *
 * Format-Vertrag (stabil, von Pivot geparst):
 *   Jede Zeile ist key=value, kein Whitespace um '='.
 *   Pivot darf pro Zeile am ersten '=' splitten.
 */
static ssize_t info_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"hw_type=%s\n"
		"version=%s\n"
		"release=%s\n"
		"official=%s\n"
		"mode=%s\n",
		is_recovery ? "unknown" : hw_type,
		CONFIG_FINCH_VERSION,
		CONFIG_FINCH_RELEASE,
		IS_ENABLED(CONFIG_FINCH_OFFICIAL_BUILD) ? "yes" : "no",
		get_finch_mode());
}

/* --- Attribute Definitionen --- */
static struct kobj_attribute hw_type_attr  = __ATTR_RO(hw_type);
static struct kobj_attribute version_attr  = __ATTR_RO(version);
static struct kobj_attribute release_attr  = __ATTR_RO(release);
static struct kobj_attribute official_attr = __ATTR_RO(official);
static struct kobj_attribute mode_attr     = __ATTR_RO(mode);
static struct kobj_attribute info_attr     = __ATTR_RO(info);

static struct attribute *finch_attrs[] = {
	&hw_type_attr.attr,
	&version_attr.attr,
	&release_attr.attr,
	&official_attr.attr,
	&mode_attr.attr,
	&info_attr.attr,
	NULL,
};

static const struct attribute_group finch_group = {
	.attrs = finch_attrs,
};

static struct kobject *finch_kobj;

/* --- Modul Initialisierung --- */
static int __init fbcore_init(void)
{
	int retval;

	/* Schritt 1: Validierung des Boot-Parameters */
	if (hw_type == NULL || strlen(hw_type) == 0) {
		is_recovery = true;
		pr_warn("fbcore: No hw_type provided. Falling back to RECOVERY mode.\n");
	} else if (!is_valid_hw_type(hw_type)) {
		is_recovery = true;
		pr_warn("fbcore: Unknown hw_type '%s'. Falling back to RECOVERY mode.\n",
			hw_type);
	} else {
		pr_info("fbcore: FinchBerry OS (%s) on %s initialized.\n",
			CONFIG_FINCH_RELEASE, hw_type);
	}

	/* Schritt 2: Verzeichnis /sys/kernel/finch erstellen */
	finch_kobj = kobject_create_and_add("finch", kernel_kobj);
	if (!finch_kobj)
		return -ENOMEM;

	/* Schritt 3: Attribut-Gruppe (Dateien) erstellen */
	retval = sysfs_create_group(finch_kobj, &finch_group);
	if (retval) {
		kobject_put(finch_kobj);
		return retval;
	}

	return 0;
}

/* Als subsys_initcall, damit Pivot die Infos sofort beim Start vorfindet */
subsys_initcall(fbcore_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("FinchBerryOS Team");
MODULE_DESCRIPTION("Core identity and hardware detection for FinchBerryOS");