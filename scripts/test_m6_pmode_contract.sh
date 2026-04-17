#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/m6-pmode-contract.log"
SERIAL_LOG="$LOG_DIR/m6-pmode-contract-serial.log"
LOCK_FILE="$LOG_DIR/qemu-test.lock"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-90}"

mkdir -p "$LOG_DIR"
rm -f "$LOG_FILE" "$SERIAL_LOG"

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

echo "[test-m6-pmode] starting boot (timeout ${TIMEOUT_SECONDS}s)..."
set +e
CIUKIOS_INCLUDE_FREEDOS=0 \
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
        echo "[test-m6-pmode] ---- qemu serial log ----"
        cat "$SERIAL_LOG"
    } >> "$LOG_FILE"
fi

if [[ $rc -eq 124 ]]; then
    echo "[test-m6-pmode] timeout reached (expected for QEMU halt loop)"
elif [[ $rc -ne 0 ]]; then
    echo "[FAIL] run_ciukios.sh exited with error (exit=$rc)" >&2
    tail -n 120 "$LOG_FILE" >&2 || true
    exit 1
fi

required_patterns=(
    "[ compat ] PMODE contract v1 ready (CIUKEX64 marker + stub offset)"
    "[ test ] m6 pmode contract marker selftest: PASS"
    "[ test ] m6 pmode shell surface selftest: PASS"
)

forbidden_patterns=(
    "[ test ] m6 pmode contract marker selftest: FAIL"
    "[ test ] m6 pmode shell surface selftest: FAIL"
    "[ panic ]"
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

echo "[PASS] m6 pmode contract test completed"
echo "[INFO] log: $LOG_FILE"
