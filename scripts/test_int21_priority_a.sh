#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="$PROJECT_DIR/.ciukios-testlogs"
LOG_FILE="$LOG_DIR/stage2-boot.log"
STAGE2_TEST="$PROJECT_DIR/scripts/test_stage2_boot.sh"

mkdir -p "$LOG_DIR"

if [[ ! -f "$LOG_FILE" ]]; then
    echo "[test-int21] stage2 log missing, running stage2 boot test first..."
    "$STAGE2_TEST"
else
    echo "[test-int21] reusing existing log: $LOG_FILE"
fi

required_patterns=(
    "[ test ] int21 priority-a selftest: PASS"
    "[ compat ] INT10h baseline path ready (stage2 video text/gfx)"
    "[ compat ] INT16h baseline path ready (irq1 + key buffer)"
    "[ compat ] INT1Ah baseline path ready (pit tick source)"
    "[ compat ] INT21h PSP/status path ready (AH=51h/62h/4Dh)"
    "[ compat ] INT21h io/handle baseline ready (AH=0Bh/0Ch/3Ch..42h)"
)

forbidden_patterns=(
    "[ test ] int21 priority-a selftest: FAIL"
    "Invalid Opcode"
    "#UD"
    "[ panic ]"
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

echo "[PASS] INT21 priority-A compatibility test completed"
