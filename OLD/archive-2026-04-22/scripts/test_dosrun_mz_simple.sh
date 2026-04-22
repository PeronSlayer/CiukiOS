#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/dosrun-mz.log"
SERIAL_LOG="$LOG_DIR/dosrun-mz-serial.log"
LOCK_FILE="$LOG_DIR/qemu-test.lock"
RUNTIME_DIR="$PROJECT_DIR/third_party/freedos/runtime"
FDAUTO_PATH="$RUNTIME_DIR/FDAUTO.BAT"
BACKUP_PATH="$LOG_DIR/FDAUTO.BAT.backup"

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
echo [dosrun-mz-e2e] begin
run CIUKMZ.EXE
echo [dosrun-mz-e2e] end
EOF

echo "[test-dosrun-mz] prebuilding artifacts..."
make -C "$PROJECT_DIR" clean all
make -C "$PROJECT_DIR/boot/uefi-loader" clean all

echo "[test-dosrun-mz] starting boot (timeout ${TIMEOUT_SECONDS}s)..."
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
        echo "[test-dosrun-mz] ---- qemu serial log ----"
        cat "$SERIAL_LOG"
    } >> "$LOG_FILE"
fi

if [[ $rc -eq 124 ]]; then
    echo "[test-dosrun-mz] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
    echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
    tail -n 120 "$LOG_FILE" >&2 || true
    exit 1
fi

required_patterns=(
    "[dosrun] launch path=CIUKMZ.EXE type=MZ"
    "[dosrun] result=ok code=0x2B"
    "[dosrun] argv tail len="
    "[dosrun] argv parse=PASS"
    "[compat] INT21h date/time ready (AH=2Ah/2Ch)"
    "[compat] INT21h ioctl baseline ready (AH=44h/AL=00h)"
)

forbidden_patterns=(
    "[dosrun] result=error class=bad_format"
    "[dosrun] result=error class=runtime"
    "[dosrun] result=error class=args_parse"
    "[dosrun] result=error class=unsupported_int21"
    "[dosrun] argv parse=FAIL"
    "[ panic ]"
    "#UD"
)

for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] missing required pattern: $pattern" >&2
        tail -n 200 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] found: $pattern"
done

for pattern in "${forbidden_patterns[@]}"; do
    if grep -Fq "$pattern" "$LOG_FILE"; then
        echo "[FAIL] forbidden pattern found: $pattern" >&2
        tail -n 200 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[OK] absent: $pattern"
done

echo "[PASS] dosrun MZ simple-program test completed"
echo "[INFO] log: $LOG_FILE"
