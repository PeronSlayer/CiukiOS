#!/usr/bin/env bash
# f3-57 unified CiukiDOS local IFS: the pinned fork build (twice) and the
# pristine patch-0001 kernel with the pinned Open Watcom 1.9/NASM tools, then
# real-CPU (libx86emu) boots of the fork with the production DFV2 adapter and
# live IFS backend over the production VFS/FAT. No QEMU, systemd scope or OS
# image. See README.md.
set -euo pipefail
cd "$(dirname "$0")/../../.."
if [[ ! -f /usr/include/x86emu.h ]]; then
    echo 'H57: unavailable (libx86emu development headers missing)'
    exit 1
fi
# ASan/UBSan with leak detection left at its default (on).
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    python3 -B tests/host/dos/unified_file.py </dev/null
