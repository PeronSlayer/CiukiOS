#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/doom-target-packaging.log"
IMAGE="$PROJECT_DIR/build/ciukios.img"
TMP_DIR="$LOG_DIR/doom-packaging-fixtures"

mkdir -p "$LOG_DIR"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"
rm -f "$LOG_FILE"

static_fallback() {
	echo "[test-doom-target-packaging] image inspection unavailable; using static fallback"
	grep -Fq 'CIUKIOS_INCLUDE_DOOM' "$PROJECT_DIR/run_ciukios.sh" || {
		echo "[FAIL] run_ciukios.sh missing DOOM packaging toggle" >&2
		exit 1
	}
	grep -Fq 'CIUKIOS_QEMU_SKIP_RUN' "$PROJECT_DIR/run_ciukios.sh" || {
		echo "[FAIL] run_ciukios.sh missing image-only packaging mode" >&2
		exit 1
	}
	grep -Fq 'DOOM.BAT generated' "$PROJECT_DIR/run_ciukios.sh" || {
		echo "[FAIL] run_ciukios.sh missing DOOM launch batch generation" >&2
		exit 1
	}
	grep -Fq 'DOOM.WAD alias mapped to DOOM1.WAD' "$PROJECT_DIR/run_ciukios.sh" || {
		echo "[FAIL] run_ciukios.sh missing DOOM.WAD alias handling" >&2
		exit 1
	}
	echo "[PASS] doom target packaging test completed (static fallback)"
	return 0
}

printf 'fake-doom-exe\n' > "$TMP_DIR/DOOM.EXE"
printf 'fake-shareware-wad\n' > "$TMP_DIR/DOOM.WAD"
printf 'mouse_sensitivity 5\n' > "$TMP_DIR/DEFAULT.CFG"

echo "[test-doom-target-packaging] prebuilding artifacts..."
make -C "$PROJECT_DIR" clean all
make -C "$PROJECT_DIR/boot/uefi-loader" clean all

CIUKIOS_INCLUDE_FREEDOS=0 \
CIUKIOS_INCLUDE_OPENGEM=0 \
CIUKIOS_INCLUDE_DOOM=1 \
CIUKIOS_DOOM_EXE_PATH="$TMP_DIR/DOOM.EXE" \
CIUKIOS_DOOM_WAD_PATH="$TMP_DIR/DOOM.WAD" \
CIUKIOS_DOOM_CFG_PATH="$TMP_DIR/DEFAULT.CFG" \
CIUKIOS_QEMU_SKIP_RUN=1 \
CIUKIOS_SKIP_BUILD=1 \
"$RUN_SCRIPT" > "$LOG_FILE" 2>&1

required_log_patterns=(
	"[CiukiOS] DOOM.EXE copied to image"
	"[CiukiOS] DOOM.WAD alias mapped to DOOM1.WAD"
	"[CiukiOS] DOOM1.WAD copied to image"
	"[CiukiOS] DEFAULT.CFG copied to image"
	"[CiukiOS] DOOM.BAT generated in image"
	"[CiukiOS] QEMU launch skipped (CIUKIOS_QEMU_SKIP_RUN=1)"
)

for pattern in "${required_log_patterns[@]}"; do
	if ! grep -Fq "$pattern" "$LOG_FILE"; then
		echo "[FAIL] missing packaging log pattern: $pattern" >&2
		tail -n 120 "$LOG_FILE" >&2 || true
		exit 1
	fi
	done

if ! command -v mdir >/dev/null 2>&1; then
	static_fallback
	exit 0
fi

for pattern in DOOM.EXE DOOM1.WAD DEFAULT.CFG DOOM.BAT; do
	rm -f "$TMP_DIR/$pattern.out"
	mcopy -o -i "$IMAGE" "::EFI/CIUKIOS/$pattern" "$TMP_DIR/$pattern.out" >/dev/null 2>&1 || {
		echo "[FAIL] packaged image missing /EFI/CIUKIOS/$pattern" >&2
		mdir -i "$IMAGE" ::EFI/CIUKIOS >&2 || true
		exit 1
	}
done

grep -Fq 'run DOOM.EXE' "$TMP_DIR/DOOM.BAT.out" || {
	echo "[FAIL] DOOM.BAT missing launch command" >&2
	cat "$TMP_DIR/DOOM.BAT.out" >&2
	exit 1
}

echo "[PASS] doom target packaging test completed"
echo "[INFO] log: $LOG_FILE"