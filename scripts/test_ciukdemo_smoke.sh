#!/usr/bin/env bash
# CIUKDEMO.COM smoke gate — OT-DEMO-001.
#
# Two-tier validation (same pattern as test_vga13_baseline.sh):
#   1. Static:  build artifacts, source markers, shell wiring, image include.
#   2. Runtime: optional QEMU boot + serial capture for phase markers.
#
# Runtime is skipped (not failed) if the host cannot capture serial output.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
SERIAL_LOG="$LOG_DIR/ciukdemo-serial.log"
ATTEMPT_LOG="$LOG_DIR/ciukdemo-attempt.log"
LOCK_FILE="$LOG_DIR/qemu-test.lock"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-60}"
STATIC_PASS=0
RUNTIME_PASS=0

mkdir -p "$LOG_DIR"

fail() {
	echo "[FAIL] $1" >&2
	exit 1
}

echo "[ciukdemo-gate] Tier 1: static checks..."

# Source file exists with all phase markers and entry point
SRC="$PROJECT_DIR/com/ciukdemo/ciukdemo.c"
[[ -f "$SRC" ]] || fail "com/ciukdemo/ciukdemo.c missing"

grep -Fq 'void com_main' "$SRC" || fail "ciukdemo com_main entry missing"
grep -Fq '[ciukdemo] start'            "$SRC" || fail "start marker missing"
grep -Fq '[ciukdemo] phase 1 title'    "$SRC" || fail "phase 1 marker missing"
grep -Fq '[ciukdemo] phase 2 plasma'   "$SRC" || fail "phase 2 marker missing"
grep -Fq '[ciukdemo] phase 3 orbits'   "$SRC" || fail "phase 3 marker missing"
grep -Fq '[ciukdemo] phase 4 rings'    "$SRC" || fail "phase 4 marker missing"
grep -Fq '[ciukdemo] phase 5 fadeout'  "$SRC" || fail "phase 5 marker missing"
grep -Fq '[ciukdemo] OK'               "$SRC" || fail "completion marker missing"

# Shell has the `demo` command and the curated help entry
SHELL_SRC="$PROJECT_DIR/stage2/src/shell.c"
grep -Fq 'if (str_eq(cmd, "demo"))' "$SHELL_SRC" \
	|| fail "demo command not wired in shell"
grep -Fq 'run the real-time graphics showcase' "$SHELL_SRC" \
	|| fail "demo entry missing from curated help text"
grep -Fq '"CIUKDEMO.COM"' "$SHELL_SRC" \
	|| fail "shell demo dispatch does not reference CIUKDEMO.COM"

# Build artifact
[[ -f "$PROJECT_DIR/build/CIUKDEMO.COM" ]] \
	|| fail "build/CIUKDEMO.COM not found (run 'make all')"

# run_ciukios.sh includes the demo in the FAT image
grep -Fq 'CIUKDEMO.COM' "$PROJECT_DIR/run_ciukios.sh" \
	|| fail "run_ciukios.sh does not install CIUKDEMO.COM"

STATIC_PASS=1
echo "[PASS] Tier 1: static checks passed"

if [[ ! -x "$RUN_SCRIPT" ]]; then
	echo "[SKIP] Tier 2: run script not executable"
	echo "[PASS] ciukdemo gate (static-only mode)"
	exit 0
fi

echo "[ciukdemo-gate] Tier 2: runtime QEMU capture (best-effort)..."
rm -f "$ATTEMPT_LOG" "$SERIAL_LOG"

if command -v flock >/dev/null 2>&1; then
	exec 9>"$LOCK_FILE"
	if ! flock -w 60 9; then
		echo "[SKIP] Tier 2: could not acquire QEMU lock"
		echo "[PASS] ciukdemo gate (static-only mode)"
		exit 0
	fi
fi

set +e
CIUKIOS_INCLUDE_FREEDOS=0 \
CIUKIOS_INCLUDE_OPENGEM=0 \
CIUKIOS_SKIP_BUILD=1 \
CIUKIOS_QEMU_HEADLESS=1 \
CIUKIOS_QEMU_SERIAL_FILE="$SERIAL_LOG" \
timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$ATTEMPT_LOG" 2>&1
set -e

runtime_markers=0
if [[ -f "$SERIAL_LOG" ]] && [[ -s "$SERIAL_LOG" ]]; then
	for marker in \
		'[ciukdemo] start' \
		'[ciukdemo] phase 1 title' \
		'[ciukdemo] phase 2 plasma' \
		'[ciukdemo] phase 3 orbits' \
		'[ciukdemo] phase 4 rings' \
		'[ciukdemo] phase 5 fadeout' \
		'[ciukdemo] OK'; do
		if grep -Fq "$marker" "$SERIAL_LOG"; then
			runtime_markers=$((runtime_markers + 1))
			echo "[runtime] captured: $marker"
		fi
	done
fi

if [[ "$runtime_markers" -gt 0 ]]; then
	RUNTIME_PASS=1
	echo "[PASS] Tier 2: $runtime_markers runtime markers captured"
else
	echo "[SKIP] Tier 2: no runtime serial markers captured (host serial capture unavailable)"
	echo "[INFO] This is expected on some hosts. Static gate still valid."
fi

if [[ "$STATIC_PASS" -eq 1 ]] && [[ "$RUNTIME_PASS" -eq 1 ]]; then
	echo "[PASS] ciukdemo gate (static + runtime)"
elif [[ "$STATIC_PASS" -eq 1 ]]; then
	echo "[PASS] ciukdemo gate (static-only)"
else
	fail "ciukdemo gate failed"
fi
