#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/dosrun-simple.log"
SERIAL_LOG="$LOG_DIR/dosrun-simple-serial.log"
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
echo [dosrun-e2e] begin
run CIUKSMK.COM
echo [dosrun-e2e] end
EOF

echo "[test-dosrun-simple] prebuilding artifacts..."
make -C "$PROJECT_DIR" clean all
make -C "$PROJECT_DIR/boot/uefi-loader" clean all

echo "[test-dosrun-simple] starting boot (timeout ${TIMEOUT_SECONDS}s)..."
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
        echo "[test-dosrun-simple] ---- qemu serial log ----"
        cat "$SERIAL_LOG"
    } >> "$LOG_FILE"
fi

if [[ $rc -eq 124 ]]; then
    echo "[test-dosrun-simple] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
    echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
    tail -n 120 "$LOG_FILE" >&2 || true
    exit 1
fi

required_patterns=(
    "[ ok ] stage2 mini shell ready (help/pwd/cd/dir/type/copy/ren/move/mkdir/rmdir/attrib/del/ascii/cls/ver/echo/set/ticks/mem/run/pmode/opengem/vmode/shutdown/reboot)"
    "[ test ] dosrun status path selftest: PASS"
    "[dosrun] launch path=CIUKSMK.COM type=COM"
    "[dosrun] result=ok code=0x2A"
)

forbidden_patterns=(
    "[ test ] dosrun status path selftest: FAIL"
    "[dosrun] result=error class=runtime"
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

echo "[PASS] dosrun simple-program test completed"
echo "[INFO] log: $LOG_FILE"
