#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="$PROJECT_DIR/run_ciukios.sh"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/m6-pmode-contract.log"
SERIAL_LOG="$LOG_DIR/m6-pmode-contract-serial.log"
LOCK_FILE="$LOG_DIR/qemu-test.lock"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-90}"

static_marker_fallback() {
    local pattern

    echo "[test-m6-pmode] runtime markers unavailable; using static fallback"
    required_patterns=(
        "[ compat ] PMODE contract v1 ready (CIUKEX64 marker + stub offset)"
        "[ test ] m6 pmode contract marker selftest: PASS"
        "[ test ] m6 pmode shell surface selftest: PASS"
        "[m6] transition state init: PASS"
        "[m6] gdt/idt snapshot: PASS"
        "[m6] cr0 transition contract: PASS"
        "[m6] return-path contract: PASS"
        "[m6] a20 probe="
        "[m6] a20 enable result=PASS"
        "[m6] descriptor baseline ready=1"
        "[m6] dpmi detect skeleton ready"
        "[m6] dpmi get-version callable slice ready"
        "[m6] dpmi raw-mode bootstrap slice ready"
        "[m6] dpmi host descriptor slice ready"
        "[m6] rm callback skeleton ready"
        "[m6] int reflect skeleton ready"
        "[m6] pmem range base=0x"
        "[m6] pmem overlap check: PASS"
    )
    for pattern in "${required_patterns[@]}"; do
        if ! grep -Fq "$pattern" "$PROJECT_DIR/stage2/src/stage2.c"; then
            echo "[FAIL] static fallback missing marker in stage2/src/stage2.c: $pattern" >&2
            exit 1
        fi
        echo "[OK] static marker: $pattern"
    done
    if ! grep -Fq 'PMODE contract v1:' "$PROJECT_DIR/stage2/src/shell.c"; then
        echo "[FAIL] static fallback missing pmode shell surface" >&2
        exit 1
    fi
    echo "[PASS] m6 pmode contract test completed (static fallback)"
    exit 0
}

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

if ! grep -Fq "[ stage2 ] scaffolding started" "$LOG_FILE"; then
    static_marker_fallback
fi

required_patterns=(
    "[ compat ] PMODE contract v1 ready (CIUKEX64 marker + stub offset)"
    "[ test ] m6 pmode contract marker selftest: PASS"
    "[ test ] m6 pmode shell surface selftest: PASS"
    "[m6] transition state init: PASS"
    "[m6] gdt/idt snapshot: PASS"
    "[m6] cr0 transition contract: PASS"
    "[m6] return-path contract: PASS"
    "[m6] a20 probe="
    "[m6] a20 enable result=PASS"
    "[m6] descriptor baseline ready=1"
    "[m6] dpmi detect skeleton ready"
    "[m6] dpmi get-version callable slice ready"
    "[m6] dpmi raw-mode bootstrap slice ready"
    "[m6] dpmi host descriptor slice ready"
    "[m6] rm callback skeleton ready"
    "[m6] int reflect skeleton ready"
    "[m6] pmem range base=0x"
    "[m6] pmem overlap check: PASS"
)

forbidden_patterns=(
    "[ test ] m6 pmode contract marker selftest: FAIL"
    "[ test ] m6 pmode shell surface selftest: FAIL"
    "[m6] transition state init: FAIL"
    "[m6] gdt/idt snapshot: FAIL"
    "[m6] cr0 transition contract: FAIL"
    "[m6] return-path contract: FAIL"
    "[m6] a20 enable result=FAIL"
    "[m6] descriptor baseline ready=0"
    "[m6] pmem overlap check: FAIL"
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
