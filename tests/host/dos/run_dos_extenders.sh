#!/usr/bin/env bash
# DOS extenders in the DOS Box (DOSX): a DOS/4GW 1.97 client and the Warcraft
# II demo under DOS/4GW Professional 1.97 on the production VM code with
# libx86emu (selector 40h, nested saved flags images). No QEMU, systemd scope
# or OS image. See dos_extenders.py.
set -euo pipefail
cd "$(dirname "$0")/../../.."
if [[ ! -f /usr/include/x86emu.h ]]; then
    echo 'DOSX: unavailable (libx86emu development headers missing)'
    exit 1
fi
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    python3 -B tests/host/dos/dos_extenders.py </dev/null
