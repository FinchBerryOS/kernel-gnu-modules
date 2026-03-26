# fbppid

Small kernel module for broker-only current PPID queries.

## Device
- `/dev/fbppid`

## Features
- PID 1 / CAP_SYS_ADMIN can register the broker
- registered broker can query current PPID of a target TGID

## Build
```bash
make