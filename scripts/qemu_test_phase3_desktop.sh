#!/usr/bin/env bash
set -euo pipefail

# OG-P3 acceptance gate: OpenGEM graphical desktop Phase 3 closure
# Tests that the full profile boots to GEM desktop without crashes

: "${CIUKIOS_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [[ "$(uname -s)" == "Darwin" ]]; then
	source "$(cd "$(dirname "${BASH_SOURCE[0]}")/macos" && pwd)/common.sh"
	ciuk_macos_prepare_tools
	ciuk_macos_check_required
	cd "$CIUKIOS_ROOT"
fi

IMG="${CIUKIOS_FULL_IMG:-build/full/ciukios-full.img}"
TIMEOUT=30
LOG_FILE="${CIUKIOS_P3_LOG:-build/full/qemu-phase3-desktop.log}"
DO_BUILD="${CIUKIOS_P3_BUILD:-1}"

# P3 must validate the real OpenGEM path, not the deterministic GEMVDI shim.
export CIUKIOS_OPENGEM_VALIDATION_VDI="${CIUKIOS_OPENGEM_VALIDATION_VDI:-0}"
# P3 gate needs Stage2 desktop autorun even if build_full defaults to shell-first.
export CIUKIOS_STAGE2_AUTORUN="${CIUKIOS_STAGE2_AUTORUN:-1}"

if [[ ! -f "$IMG" ]]; then
	echo "[qemu_test_phase3_desktop] ERROR: image not found: $IMG"
	exit 1
fi

if [[ "$DO_BUILD" == "1" ]]; then
	echo "[qemu_test_phase3_desktop] build step (real GEMVDI)"
	bash scripts/build_full.sh
fi

echo "[qemu_test_phase3_desktop] launching QEMU with full profile (timeout: ${TIMEOUT}s)"

mkdir -p "$(dirname "$LOG_FILE")"
rm -f "$LOG_FILE"

timeout "$TIMEOUT" qemu-system-i386 \
	-machine pc \
	-m 32M \
	-drive file="$IMG",format=raw,if=ide \
	-nographic \
	-chardev "file,id=ser0,path=$LOG_FILE" \
	-serial chardev:ser0 \
	-monitor none \
	-no-reboot \
	-no-shutdown \
	>/dev/null 2>&1 || true

if [[ -f "$LOG_FILE" ]]; then
	echo "[qemu_test_phase3_desktop] serial log: $LOG_FILE"
	tail -n 120 "$LOG_FILE" || true
else
	echo "[qemu_test_phase3_desktop] WARN: serial log missing"
fi

# Success criteria for real desktop mode:
# 1) Stage2 reaches OpenGEM launch and at least one desktop handoff marker
#    (GEM.EXE-first or GEMVDI-first).
# 2) No explicit launch failure marker is emitted.
# 3) Return marker is optional because real GEM can stay active in GUI mode.
if [[ -f "$LOG_FILE" ]] && grep -Eq "\[OPENGEM\] launch|\[\[OOPPEENNGGEEMM\]\][[:space:]]+llaauunncch" "$LOG_FILE"; then
	if grep -Eqi "\[OPENGEM\][[:space:]]+try[[:space:]]+GEMVDI|\[OPENGEM\][[:space:]]+try[[:space:]]+GEM\.EXE|\[\[OOPPEENNGGEEMM\]\][[:space:]]+ttrryy[[:space:]]+GGEEMMVVDDI{1,2}|\[\[OOPPEENNGGEEMM\]\].*ttrryy.*GGEEMM|try.*GEM.*EXE" "$LOG_FILE"; then
		if ! grep -Eq "\[OPENGEM\] launch failed AX=|\[\[OOPPEENNGGEEMM\]\][[:space:]]+llaauunncch[[:space:]]+ffaaiilleedd" "$LOG_FILE"; then
			echo "[qemu_test_phase3_desktop] PASS: OpenGEM handoff reached (desktop path active)"
			exit 0
		fi
	fi
fi

echo "[qemu_test_phase3_desktop] FAIL: GEM desktop did not reach completion"
exit 1
