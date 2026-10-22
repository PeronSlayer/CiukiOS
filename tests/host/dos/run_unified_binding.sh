#!/usr/bin/env bash
# f3-56 unified CiukiDOS DFV2 binding: pinned fork build (twice) and pristine
# patch-0001 build with the pinned Open Watcom 1.9/NASM tools, the sanitized
# native matrix, and real-CPU (libx86emu) boots of the fork, CIUKIDF.SYS and
# guest COM programs. No QEMU, systemd scope or OS image. See README.md.
set -euo pipefail
cd "$(dirname "$0")/../../.."
if [[ ! -f /usr/include/x86emu.h ]]; then
    echo 'H56: unavailable (libx86emu development headers missing)'
    exit 1
fi
# ASan/UBSan with leak detection left at its default (on).
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    python3 -B tests/host/dos/unified_binding.py </dev/null
