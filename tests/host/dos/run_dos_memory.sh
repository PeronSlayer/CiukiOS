#!/usr/bin/env bash
# DOS Box memory (DOSMEM): what a real-mode program started at the DOS Box
# prompt sees (XMS through INT 2Fh, free conventional memory, FreeCOM swap,
# HMA/UMB/EMS) on the production VM code with libx86emu, and Wolfenstein 3-D
# 1.4's memory check. No QEMU, systemd scope or OS image. See dos_memory.py.
set -euo pipefail
cd "$(dirname "$0")/../../.."
if [[ ! -f /usr/include/x86emu.h ]]; then
    echo 'DOSMEM: unavailable (libx86emu development headers missing)'
    exit 1
fi
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    python3 -B tests/host/dos/dos_memory.py </dev/null
