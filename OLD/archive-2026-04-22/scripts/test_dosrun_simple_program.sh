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

run_dosrun_attempt() {
    local label="$1"
    local headless="$2"
    local attempt_log="$LOG_DIR/dosrun-simple-${label}.attempt.log"
    local attempt_serial="$LOG_DIR/dosrun-simple-${label}.serial.log"
    local rc

    rm -f "$attempt_log" "$attempt_serial"

    echo "[test-dosrun-simple] starting boot (${label}, timeout ${TIMEOUT_SECONDS}s)..."
    set +e
    CIUKIOS_INCLUDE_FREEDOS=1 \
    CIUKIOS_INCLUDE_OPENGEM=0 \
    CIUKIOS_SKIP_BUILD=1 \
    CIUKIOS_QEMU_HEADLESS="$headless" \
    CIUKIOS_QEMU_SERIAL_FILE="$attempt_serial" \
    timeout "${TIMEOUT_SECONDS}s" "$RUN_SCRIPT" > "$attempt_log" 2>&1
    rc=$?

    {
        echo
        echo "[test-dosrun-simple] ---- attempt ${label} log ----"
        cat "$attempt_log"
        if [[ -f "$attempt_serial" ]]; then
            echo
            echo "[test-dosrun-simple] ---- attempt ${label} qemu serial log ----"
            cat "$attempt_serial"
        fi
    } >> "$LOG_FILE"

    DOSRUN_ATTEMPT_LABEL="$label"
    DOSRUN_ATTEMPT_LOG="$attempt_log"
    DOSRUN_ATTEMPT_SERIAL="$attempt_serial"
    return "$rc"
}

cat > "$FDAUTO_PATH" <<'EOF'
echo [dosrun-e2e] begin
run CIUKSMK.COM
echo [dosrun-e2e] end
EOF

echo "[test-dosrun-simple] prebuilding artifacts..."
make -C "$PROJECT_DIR" clean
make -C "$PROJECT_DIR" all
make -C "$PROJECT_DIR/boot/uefi-loader" clean
make -C "$PROJECT_DIR/boot/uefi-loader" all

rm -f "$LOG_FILE"
set +e
run_dosrun_attempt headless 1
rc=$?
set -e
ACTIVE_LOG="$DOSRUN_ATTEMPT_LOG"
ACTIVE_SERIAL="$DOSRUN_ATTEMPT_SERIAL"
ACTIVE_LABEL="$DOSRUN_ATTEMPT_LABEL"
EVIDENCE_LOG="$LOG_FILE"

if ! grep -Fq "[ ok ] stage2 mini shell ready" "$EVIDENCE_LOG"; then
    if grep -Fq "[CiukiOS] Starting QEMU..." "$EVIDENCE_LOG"; then
        echo "[test-dosrun-simple] headless capture produced no shell markers; retrying with graphical QEMU fallback" | tee -a "$LOG_FILE"
        set +e
        run_dosrun_attempt gui-fallback 0
        rc=$?
        set -e
        ACTIVE_LOG="$DOSRUN_ATTEMPT_LOG"
        ACTIVE_SERIAL="$DOSRUN_ATTEMPT_SERIAL"
        ACTIVE_LABEL="$DOSRUN_ATTEMPT_LABEL"
    fi
fi

if [[ $rc -eq 124 ]]; then
    echo "[test-dosrun-simple] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
    echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
    tail -n 120 "$EVIDENCE_LOG" >&2 || true
    exit 1
fi

required_patterns=(
    "[ ok ] stage2 mini shell ready (help/pwd/cd/dir/type/copy/ren/move/mkdir/rmdir/attrib/del/ascii/cls/ver/echo/set/ticks/mem/run/pmode/opengem/vmode/shutdown/reboot)"
    "[ test ] dosrun status path selftest: PASS"
    "[dosrun] launch path=CIUKSMK.COM type=COM"
    "[dosrun] result=ok code=0x2A"
    "[dosrun] argv tail len="
    "[dosrun] argv parse=PASS"
    "[compat] INT21h date/time ready (AH=2Ah/2Ch)"
    "[compat] INT21h ioctl baseline ready (AH=44h/AL=00h)"
)

forbidden_patterns=(
    "[ test ] dosrun status path selftest: FAIL"
    "[dosrun] result=error class=runtime"
    "[dosrun] result=error class=args_parse"
    "[dosrun] result=error class=unsupported_int21"
    "[dosrun] argv parse=FAIL"
    "[ panic ]"
    "#UD"
)

for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "$pattern" "$EVIDENCE_LOG"; then
        echo "[FAIL] missing required pattern: $pattern" >&2
        tail -n 200 "$EVIDENCE_LOG" >&2 || true
        exit 1
    fi
    echo "[OK] found: $pattern"
done

for pattern in "${forbidden_patterns[@]}"; do
    if grep -Fq "$pattern" "$EVIDENCE_LOG"; then
        echo "[FAIL] forbidden pattern found: $pattern" >&2
        tail -n 200 "$EVIDENCE_LOG" >&2 || true
        exit 1
    fi
    echo "[OK] absent: $pattern"
done

echo "[PASS] dosrun simple-program test completed"
echo "[INFO] log: $LOG_FILE"
