#!/usr/bin/env bash
# Staged boot-to-DOOM failure-taxonomy harness.
#
# This is a deterministic packaging-level harness that reuses the existing
# DOOM target packaging baseline and classifies the result into the staged
# failure taxonomy expected by the DOOM milestone:
#
#   1. binary_found   - DOOM.EXE is present in the packaged image
#   2. wad_found      - DOOM1.WAD (or DOOM.WAD alias) is present in the image
#   3. extender_init  - DOS extender readiness markers are active in stage2
#   4. video_init     - VGA mode 13h baseline markers are active in stage2
#   5. menu_reached   - deferred: requires real DOOM runtime (not yet wired)
#
# The harness classifies earlier stages as PASS and the `menu_reached` stage
# as DEFERRED until the real runtime path is available. This keeps the gate
# deterministic today while giving the DOOM milestone a stable progression
# contract.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/doom-boot-harness.log"
TMP_DIR="$LOG_DIR/doom-boot-harness-fixtures"
IMAGE="$PROJECT_DIR/build/ciukios.img"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"

mkdir -p "$LOG_DIR"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"
rm -f "$LOG_FILE"

emit_stage() {
	local stage="$1" status="$2" detail="${3:-}"
	echo "[stage] ${stage} ${status}${detail:+ - }${detail}"
}

stage_fail() {
	emit_stage "$1" "FAIL" "${2:-}"
	exit 1
}

printf 'fake-doom-exe\n' > "$TMP_DIR/DOOM.EXE"
printf 'fake-shareware-wad\n' > "$TMP_DIR/DOOM.WAD"
printf 'mouse_sensitivity 5\n' > "$TMP_DIR/DEFAULT.CFG"

echo "[test-doom-boot-harness] prebuilding artifacts..."
make -C "$PROJECT_DIR" clean all
make -C "$PROJECT_DIR/boot/uefi-loader" clean all

echo "[test-doom-boot-harness] packaging image (skip-run mode)..."
CIUKIOS_INCLUDE_FREEDOS=0 \
CIUKIOS_INCLUDE_OPENGEM=0 \
CIUKIOS_INCLUDE_DOOM=1 \
CIUKIOS_DOOM_EXE_PATH="$TMP_DIR/DOOM.EXE" \
CIUKIOS_DOOM_WAD_PATH="$TMP_DIR/DOOM.WAD" \
CIUKIOS_DOOM_CFG_PATH="$TMP_DIR/DEFAULT.CFG" \
CIUKIOS_QEMU_SKIP_RUN=1 \
CIUKIOS_SKIP_BUILD=1 \
"$RUN_SCRIPT" > "$LOG_FILE" 2>&1 || {
	stage_fail "binary_found" "packaging step failed (see $LOG_FILE)"
}

# Stage 1: binary_found
if ! grep -Fq '[CiukiOS] DOOM.EXE copied to image' "$LOG_FILE"; then
	stage_fail "binary_found" "no DOOM.EXE copy marker in packaging log"
fi
emit_stage "binary_found" "PASS"

# Stage 2: wad_found
if ! grep -Fq '[CiukiOS] DOOM1.WAD copied to image' "$LOG_FILE"; then
	stage_fail "wad_found" "no DOOM1.WAD copy marker in packaging log"
fi
if ! grep -Fq '[CiukiOS] DOOM.WAD alias mapped to DOOM1.WAD' "$LOG_FILE"; then
	stage_fail "wad_found" "DOOM.WAD alias handling missing from packaging log"
fi
emit_stage "wad_found" "PASS"

# Stage 3: extender_init (static marker validation)
required_ext=(
	'm6_dpmi_detect_skeleton_ready'
	'dpmi get-version callable slice ready'
	'dpmi raw-mode bootstrap slice ready'
	'dpmi allocate-ldt slice ready'
)
for pat in "${required_ext[@]}"; do
	if ! grep -Fq "$pat" "$PROJECT_DIR/stage2/src/stage2.c"; then
		stage_fail "extender_init" "missing marker source: $pat"
	fi
done
emit_stage "extender_init" "PASS"

# Stage 4: video_init (VGA13h baseline)
required_video=(
	'm6_vga13_baseline_ready'
	'[compat] vga13 baseline ready (320x200x8 checkpoint v1)'
)
for pat in "${required_video[@]}"; do
	if ! grep -Fq "$pat" "$PROJECT_DIR/stage2/src/stage2.c"; then
		stage_fail "video_init" "missing marker source: $pat"
	fi
done
emit_stage "video_init" "PASS"

# Stage 5: menu_reached - deferred until real DOOM runtime path exists.
emit_stage "menu_reached" "DEFERRED" "requires real DOOM runtime (post-M6 extender closure)"

echo "[PASS] doom boot-to-game staged harness (failure taxonomy wired)"
echo "[INFO] log: $LOG_FILE"
