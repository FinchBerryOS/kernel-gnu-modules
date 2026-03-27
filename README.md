# Finch Kernel Components

This repository contains low-level FinchBerryOS kernel-side components that provide core system identity and brokered parent-process inspection.

Currently included:

- `fbcore`
- `fbppid`

---

## Overview

These components are intended for early boot and core system operation.

### `fbcore`

`fbcore` exposes FinchBerryOS identity and boot-mode metadata through sysfs under:

```text
/sys/kernel/finch
```

It provides a stable kernel-to-userspace interface for early components such as `pivot`, including:

- hardware target type
- system version
- release string
- official-build flag
- current mode (`production` or `recovery`)

### `fbppid`

`fbppid` provides a restricted kernel interface for parent-PID inspection through:

```text
/dev/fbppid
```

It is designed around a broker model:

- PID 1 or a process with `CAP_SYS_ADMIN` registers the broker
- only the registered broker may query the current PPID of a target TGID

This is used by higher-level FinchBerryOS userspace components to safely resolve process parent relationships without exposing unrestricted access.

---

## Repository structure

Typical contents:

- `fbcore/`
  - kernel identity component
- `fbppid/`
  - broker-only PPID query interface
- userspace companion crates may exist in related repositories, such as Rust wrappers for `/dev/fbppid`

---

## Component summaries

## fbcore

Main responsibilities:

- read hardware type from boot parameters
- expose system metadata through sysfs
- determine whether the system is in production or recovery mode
- provide a stable `key=value` info block for early userspace

Published path:

```text
/sys/kernel/finch
```

Important outputs:

- `hw_type`
- `version`
- `release`
- `official`
- `mode`
- `info`

`fbcore` is intended to initialize early enough that boot-time userspace can depend on it.

## fbppid

Main responsibilities:

- expose `/dev/fbppid`
- allow trusted broker registration
- allow broker-only current PPID queries

Main model:

1. PID 1 starts the broker process
2. PID 1 registers that broker with the kernel interface
3. the broker performs PPID queries for target processes

This keeps the PPID query interface intentionally narrow and controlled.

---

## Build

Each component may have its own build flow.

For example, `fbppid` provides a standard kernel-module style build entrypoint:

```bash
make
```

Refer to the component-local README files for detailed build and integration instructions.

---

## Intended use

This repository is meant for FinchBerryOS platform and boot integration work, especially:

- early boot environment setup
- system identity exposure
- recovery-mode signaling
- brokered kernel/userspace process inspection

These components are not intended as general-purpose Linux modules; they are part of the FinchBerryOS platform stack.

---

## See also

For component-specific details, read the local READMEs:

- `fbcore/README.md`
- `fbppid/README.md`

---

## Summary

This repository contains core FinchBerryOS kernel components for:

- exposing stable system identity to early userspace
- signaling production vs recovery mode
- providing a broker-restricted PPID query mechanism
- supporting the FinchBerryOS boot and process-control stack
