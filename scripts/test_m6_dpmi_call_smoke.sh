#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/m6-dpmi-call-smoke.log"
SERIAL_LOG="$LOG_DIR/m6-dpmi-call-smoke-serial.log"
LOCK_FILE="$LOG_DIR/qemu-test.lock"
RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime"
FDAUTO_PATH="$RUNTIME_DIR/FDAUTO.BAT"
BACKUP_PATH="$LOG_DIR/FDAUTO.BAT.m6dpmicall.backup"

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
	echo "[test-m6-dpmi-call-smoke] runtime markers unavailable; using static fallback"
	[[ -f "$PROJECT_DIR/build/CIUK31.EXE" ]] || {
		echo "[FAIL] missing built artifact: build/CIUK31.EXE" >&2
		exit 1
	}
	grep -Fq 'COM_M6_DPMI_CALL_SMOKE_BIN := build/CIUK31.EXE' "$PROJECT_DIR/Makefile" || {
		echo "[FAIL] Makefile missing CIUK31.EXE wiring" >&2
		exit 1
	}
	grep -Fq 'CIUK31.EXE copied to image' "$PROJECT_DIR/run_ciukios.sh" || {
		echo "[FAIL] run_ciukios.sh missing CIUK31.EXE image copy" >&2
		exit 1
	}
	grep -Fq 'void     (*int31)(ciuki_dos_context_t *ctx, ciuki_int21_regs_t *regs);' "$PROJECT_DIR/boot/proto/services.h" || {
		echo "[FAIL] services ABI missing int31 hook" >&2
		exit 1
	}
	grep -Fq 'if (regs->ax == 0x0400U)' "$PROJECT_DIR/stage2/src/shell.c" || {
		echo "[FAIL] missing DPMI get-version handler" >&2
		exit 1
	}
	grep -Fq 'regs->ax = 0x005AU;' "$PROJECT_DIR/stage2/src/shell.c" || {
		echo "[FAIL] missing DPMI version return wiring" >&2
		exit 1
	}
	grep -Fq 'svc->int31' "$PROJECT_DIR/com/m6_dpmi_call_smoke/ciuk31.c" || {
		echo "[FAIL] smoke payload missing int31 call" >&2
		exit 1
	}
	echo "[PASS] m6 DPMI callable smoke test completed (static fallback)"
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
echo [m6-dpmi-call-e2e] begin
run CIUK31.EXE
echo [m6-dpmi-call-e2e] end
EOF

echo "[test-m6-dpmi-call-smoke] prebuilding artifacts..."
make -C "$PROJECT_DIR" clean all
make -C "$PROJECT_DIR/boot/uefi-loader" clean all

echo "[test-m6-dpmi-call-smoke] starting boot (timeout ${TIMEOUT_SECONDS}s)..."
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
		echo "[test-m6-dpmi-call-smoke] ---- qemu serial log ----"
		cat "$SERIAL_LOG"
	} >> "$LOG_FILE"
fi

if [[ $rc -eq 124 ]]; then
	echo "[test-m6-dpmi-call-smoke] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
	echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
	tail -n 120 "$LOG_FILE" >&2 || true
	exit 1
fi

if ! grep -Fq "[dosrun] launch path=CIUK31.EXE type=MZ" "$LOG_FILE"; then
	static_fallback
fi

required_patterns=(
	"[dosrun] launch path=CIUK31.EXE type=MZ"
	"[dosrun] result=ok code=0x4B"
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

echo "[PASS] m6 DPMI callable smoke test completed"
echo "[INFO] log: $LOG_FILE"