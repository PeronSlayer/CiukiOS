#!/usr/bin/env bash
# Build the full image with the DOS-window VM session profile (C:\VM: Jemm386,
# CVSESSION, VMSTART, DPMIRUN) and run it in QEMU with the AC'97 card the
# session's Sound Blaster 16 / OPL3 model plays on.
#
#   scripts/build_run_full_vm_window.sh [qemu_run_full.sh options]
#
# In the guest: F4, "run \VM\VMSTART.COM", EXIT; then F3 (Run):
#   run \VM\DPMIRUN.COM \APPS\DOOM\DOOMCORE.EXE
#   run \VM\DPMIRUN.COM \APPS\DOOMVAN\PCDMCORE.EXE
# Set CIUKIOS_SKIP_BUILD=1 to reuse build/full/ciukios-full.img.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
if [[ "${CIUKIOS_SKIP_BUILD:-0}" != "1" ]]; then
	CIUKIOS_VM_WINDOW=1 bash scripts/build_full.sh
fi
mdir -i "${CIUKIOS_FULL_IMG:-build/full/ciukios-full.img}" ::VM/VMSTART.COM >/dev/null 2>&1 \
	|| { echo "[vm-window] ERROR: the image has no C:\\VM profile; build with CIUKIOS_VM_WINDOW=1" >&2; exit 1; }
export QEMU_AUDIO_DEVICES="${QEMU_AUDIO_DEVICES:-ac97}"
exec bash scripts/qemu_run_full.sh --no-build "$@"
