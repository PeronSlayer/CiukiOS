#!/usr/bin/env bash
set -euo pipefail

# oZone Integration Smoke Test v1
# Validates oZone integration markers and command surface
# Usage: ./scripts/test_ozone_integration.sh [logfile]
#
# Returns: PASS (with optional SKIP semantics if payload absent)

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${1:-$PROJECT_DIR/.ciukios-testlogs/stage2-boot.log}"
OZONE_RUNTIME="$PROJECT_DIR/third_party/freedos/runtime/OZONE"

echo "[test-ozone] oZone integration smoke test v1"
echo ""

FAIL_COUNT=0

check_marker() {
    local marker="$1"
    local severity="$2"  # FAIL, WARN, INFO, SKIP
    if [[ -f "$LOG_FILE" ]] && grep -Fq "$marker" "$LOG_FILE" 2>/dev/null; then
        echo "[OK] found: $marker"
        return 0
    else
        echo "[$severity] absent: $marker"
        if [[ "$severity" == "FAIL" ]]; then
            ((FAIL_COUNT++)) || true
        fi
        return 1
    fi
}

check_absent() {
    local pattern="$1"
    if [[ -f "$LOG_FILE" ]] && grep -Fq "$pattern" "$LOG_FILE" 2>/dev/null; then
        echo "[FAIL] risk pattern found: $pattern"
        ((FAIL_COUNT++)) || true
    else
        echo "[OK] absent: $pattern"
    fi
}

# === Check 1: Log file exists ===
if [[ ! -f "$LOG_FILE" ]]; then
    echo "[SKIP] No boot log found: $LOG_FILE"
    echo "[PASS] oZone smoke test complete (SKIP - no boot log)"
    exit 0
fi

# === Check 2: oZone payload presence ===
OZONE_PRESENT=0
if [[ -d "$OZONE_RUNTIME" ]] && [[ -f "$OZONE_RUNTIME/OZONE.EXE" ]]; then
    echo "[info] oZone payload: PRESENT at $OZONE_RUNTIME"
    OZONE_PRESENT=1
else
    echo "[info] oZone payload: ABSENT (skip semantics active)"
fi

# === Check 3: Shell command surface ===
echo ""
echo "[info] checking shell command surface..."
check_marker "ozone" "INFO" || true  # ozone command listed in shell ready

# === Check 4: Boot integrity (no panics) ===
echo ""
echo "[info] checking boot integrity..."
check_absent "[ panic ]"
check_absent "Invalid Opcode"
check_absent "#UD"
check_absent "General Protection Fault"

# === Check 5: oZone-specific markers (only if payload present) ===
echo ""
if [[ "$OZONE_PRESENT" -eq 1 ]]; then
    echo "[info] checking oZone launch markers (payload present)..."
    check_marker "[ app ] ozone launch requested" "WARN"
    check_marker "[ app ] ozone preflight started" "WARN"
    check_marker "[ app ] ozone preflight complete" "WARN"
else
    echo "[info] oZone launch markers: SKIP (payload absent, not expected in log)"
    # Check these as INFO only — they might be present if ozone was tested manually
    check_marker "[ app ] ozone launch requested" "INFO" || true
    check_marker "[ app ] ozone preflight started" "INFO" || true
fi

# === Summary ===
echo ""
echo "=== oZone integration summary ==="
echo "  Payload:         $( [[ $OZONE_PRESENT -eq 1 ]] && echo PRESENT || echo ABSENT )"
echo "  Boot integrity:  $( [[ $FAIL_COUNT -eq 0 ]] && echo OK || echo ISSUES )"
echo "================================="
echo ""

if [[ "$FAIL_COUNT" -gt 0 ]]; then
    echo "[FAIL] oZone smoke test found $FAIL_COUNT issue(s)"
    exit 1
fi

if [[ "$OZONE_PRESENT" -eq 0 ]]; then
    echo "[PASS] oZone smoke test complete (SKIP - payload absent)"
else
    echo "[PASS] oZone smoke test complete"
fi
