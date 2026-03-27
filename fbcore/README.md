# fbcore

`fbcore` is the FinchBerryOS core identity kernel component.

It exposes basic system identity and boot-mode metadata through sysfs so early userspace components such as `pivot` can reliably detect the current system context.

The information is published under:

```text
/sys/kernel/finch
```

---

## Purpose

`fbcore` provides a small, stable identity interface for FinchBerryOS.

It is responsible for exposing:

- hardware target type
- system version
- system release
- official-build flag
- current operating mode

This allows early boot code to read a single canonical source of truth directly from the kernel.

---

## Behavior

`fbcore` uses three kinds of data sources:

- **bootloader-provided runtime data**
  - `hw_type`
- **kernel build-time configuration**
  - `CONFIG_FINCH_VERSION`
  - `CONFIG_FINCH_RELEASE`
  - `CONFIG_FINCH_OFFICIAL_BUILD`
- **internal recovery logic**
  - if `hw_type` is missing or invalid, the system is marked as recovery mode

### Allowed hardware types

Currently accepted `hw_type` values are:

- `sbc`
- `lwos`
- `egw`

If `hw_type` is not provided or does not match one of these values, `fbcore` switches to:

- `mode = recovery`
- `hw_type = unknown`

Otherwise the system reports:

- `mode = production`

---

## Boot parameter

`fbcore` expects the hardware type from the bootloader as a kernel parameter:

```text
fbcore.hw_type=<value>
```

Example:

```text
fbcore.hw_type=sbc
```

If the parameter is missing or invalid, the kernel logs a warning and `fbcore` enters recovery mode.

---

## Sysfs interface

`fbcore` creates this directory:

```text
/sys/kernel/finch
```

and exposes the following read-only files:

- `hw_type`
- `version`
- `release`
- `official`
- `mode`
- `info`

### File descriptions

#### `hw_type`

Returns the validated hardware type.

Examples:

```text
sbc
```

or in recovery mode:

```text
unknown
```

#### `version`

Returns the FinchBerryOS version from `CONFIG_FINCH_VERSION`.

#### `release`

Returns the FinchBerryOS release string from `CONFIG_FINCH_RELEASE`.

#### `official`

Returns whether this is an official build:

- `yes`
- `no`

This is derived from `CONFIG_FINCH_OFFICIAL_BUILD`.

#### `mode`

Returns the current operating mode:

- `production`
- `recovery`

#### `info`

`info` is a combined metadata file intended especially for early userspace consumers such as `pivot`.

Format contract:

- one `key=value` pair per line
- no whitespace around `=`
- stable and easy to parse
- consumers may split each line at the first `=`

Example:

```text
hw_type=sbc
version=1.0
release=FinchBerryOS 1
official=yes
mode=production
```

Recovery example:

```text
hw_type=unknown
version=1.0
release=FinchBerryOS 1
official=no
mode=recovery
```

---

## Initialization model

`fbcore` is initialized with:

```c
subsys_initcall(fbcore_init);
```

This means it is brought up very early during kernel initialization so that components like `pivot` can rely on `/sys/kernel/finch/info` being present as soon as userspace starts.

---

## No unload path

`fbcore` intentionally has no module exit / cleanup path.

Reason:

- the identity data must stay available and stable from boot until shutdown
- FinchBerryOS relies on this information as core system identity
- the component is designed as a fixed early kernel identity provider

---

## Build-time configuration

The code requires the following Kconfig values:

- `CONFIG_FINCH_VERSION`
- `CONFIG_FINCH_RELEASE`

Optional / feature-style flag:

- `CONFIG_FINCH_OFFICIAL_BUILD`

If required values are missing, compilation fails intentionally.

---

## Logging

`fbcore` logs through normal kernel logging:

- `pr_info(...)`
- `pr_warn(...)`

Typical cases:

- valid hardware type -> info log
- missing hardware type -> warning and recovery mode
- invalid hardware type -> warning and recovery mode

---

## Example usage

Read the full identity block:

```bash
cat /sys/kernel/finch/info
```

Read only the mode:

```bash
cat /sys/kernel/finch/mode
```

Read only the hardware target:

```bash
cat /sys/kernel/finch/hw_type
```

---

## Summary

`fbcore` is a small FinchBerryOS kernel identity component that:

- reads the hardware target from the bootloader
- exposes build metadata through sysfs
- determines whether the system is in production or recovery mode
- provides a stable `key=value` info file for early userspace
- initializes early enough for `pivot` and other early boot components to depend on it
