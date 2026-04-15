# Phase 1 - Foundation (Historical)

Note: this document describes the first baseline goal already reached.
The active roadmap for the DOS 6.2 compatibility path is:
- `docs/roadmap-dos62-compat.md`
- `docs/phase-0-kickoff.md`

## Current State
- Freestanding ELF kernel
- x86_64 entry point
- COM1 serial debug
- Internal boot protocol defined

## Next Goal
- Custom UEFI bootloader
- ELF kernel loading
- `boot_info` handoff to kernel
- QEMU boot via serial

## Rules
- No dependency on external kernels
- Bootloader written inside this project
- Simple, documented ABI between loader and kernel
