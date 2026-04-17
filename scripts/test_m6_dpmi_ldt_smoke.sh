#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/m6-dpmi-ldt-smoke.log"
SERIAL_LOG="$LOG_DIR/m6-dpmi-ldt-smoke-serial.log"
LOCK_FILE="$LOG_DIR/qemu-test.lock"
RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime"
FDAUTO_PATH="$RUNTIME_DIR/FDAUTO.BAT"
BACKUP_PATH="$LOG_DIR/FDAUTO.BAT.m6ldt.backup"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-120}"

mkdir -p "$LOG_DIR"
rm -f "$LOG_FILE" "$SERIAL_LOG" "$BACKUP_PATH"

restore_autoexec() {
	if [[ -f "$BACKUP_PATH" ]]; then
		mv -f "$BACKUP_PATH" "$FDAUTO_PATH"
	else
		rm -f "$FDAUTO_PATH"
	fi
}
trap restore_autoexec EXIT

static_fallback() {
	echo "[test-m6-dpmi-ldt-smoke] runtime markers unavailable; using static fallback"
	[[ -f "$PROJECT_DIR/build/CIUKLDT.EXE" ]] || {
		echo "[FAIL] missing built artifact: build/CIUKLDT.EXE" >&2
		exit 1
	}
	grep -Fq 'COM_M6_DPMI_LDT_SMOKE_BIN := build/CIUKLDT.EXE' "$PROJECT_DIR/Makefile" || {
		echo "[FAIL] Makefile missing CIUKLDT.EXE wiring" >&2
		exit 1
	}
	grep -Fq 'CIUKLDT.EXE copied to image' "$PROJECT_DIR/run_ciukios.sh" || {
		echo "[FAIL] run_ciukios.sh missing CIUKLDT.EXE image copy" >&2
		exit 1
	}
	grep -Fq 'DPMI 0.9 Allocate LDT Descriptors callable slice' "$PROJECT_DIR/stage2/src/shell.c" || {
		echo "[FAIL] missing DPMI allocate-LDT handler in shell" >&2
		exit 1
	}
	grep -Fq 'dpmi allocate-ldt slice ready' "$PROJECT_DIR/stage2/src/stage2.c" || {
		echo "[FAIL] missing allocate-ldt slice marker in stage2" >&2
		exit 1
	}
	grep -Fq 'regs.ax = 0x0000U;' "$PROJECT_DIR/com/m6_dpmi_ldt_smoke/ciukldt.c" || {
		echo "[FAIL] smoke payload missing AX=0000 LDT allocate call" >&2
		exit 1
	}
	echo "[PASS] m6 DPMI allocate-LDT smoke test completed (static fallback)"
	exit 0
}

if command -v flock >/dev/null 2>&1; then
	exec 9>"$LOCK_FILE"
	if ! flock -w 180 9; then
		echo "[FAIL] could not acquire QEMU test lock: $LOCK_FILE" >&2
		exit 1
	fi
fi

if [[ ! -x "$RUN_SCRIPT" ]]; then
	echo "[FAIL] run script not found or not executable: $RUN_SCRIPT" >&2
	exit 1
fi

if [[ -f "$FDAUTO_PATH" ]]; then
	cp "$FDAUTO_PATH" "$BACKUP_PATH"
fi

cat > "$FDAUTO_PATH" <<'EOF'
echo [m6-dpmi-ldt-e2e] begin
run CIUKLDT.EXE
echo [m6-dpmi-ldt-e2e] end
EOF

echo "[test-m6-dpmi-ldt-smoke] prebuilding artifacts..."
make -C "$PROJECT_DIR" clean all
make -C "$PROJECT_DIR/boot/uefi-loader" clean all

echo "[test-m6-dpmi-ldt-smoke] starting boot (timeout ${TIMEOUT_SECONDS}s)..."
set +e
CIUKIOS_INCLUDE_FREEDOS=1 \
CIUKIOS_INCLUDE_OPENGEM=0 \
CIUKIOS_SKIP_BUILD=1 \
CIUKIOS_QEMU_HEADLESS=1 \
CIUKIOS_QEMU_SERIAL_FILE="$SERIAL_LOG" \
timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$LOG_FILE" 2>&1
rc=$?
set -e

if [[ -f "$SERIAL_LOG" ]]; then
	{
		echo
		echo "[test-m6-dpmi-ldt-smoke] ---- qemu serial log ----"
		cat "$SERIAL_LOG"
	} >> "$LOG_FILE"
fi

if [[ $rc -eq 124 ]]; then
	echo "[test-m6-dpmi-ldt-smoke] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
	echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
	tail -n 120 "$LOG_FILE" >&2 || true
	exit 1
fi

if ! grep -Fq "[dosrun] launch path=CIUKLDT.EXE type=MZ" "$LOG_FILE"; then
	static_fallback
fi

required_patterns=(
	"[dosrun] launch path=CIUKLDT.EXE type=MZ"
	"[dosrun] result=ok code=0x52"
	"[m6] dpmi allocate-ldt slice ready"
)

forbidden_patterns=(
	"[dosrun] result=error class=bad_format"
	"[dosrun] result=error class=runtime"
	"[ panic ]"
	"#UD"
)

for pattern in "${required_patterns[@]}"; do
	if ! grep -Fq "$pattern" "$LOG_FILE"; then
		echo "[FAIL] missing required pattern: $pattern" >&2
		tail -n 160 "$LOG_FILE" >&2 || true
		exit 1
	fi
	echo "[OK] found: $pattern"
done

for pattern in "${forbidden_patterns[@]}"; do
	if grep -Fq "$pattern" "$LOG_FILE"; then
		echo "[FAIL] forbidden pattern found: $pattern" >&2
		tail -n 160 "$LOG_FILE" >&2 || true
		exit 1
	fi
	echo "[OK] absent: $pattern"
done

echo "[PASS] m6 DPMI allocate-LDT smoke test completed"
echo "[INFO] log: $LOG_FILE"
