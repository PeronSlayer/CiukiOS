#!/usr/bin/env bash
set -euo pipefail

# OpenGEM Integration Smoke Test v1
# Validates OpenGEM integration markers and command surface
# Usage: ./scripts/test_opengem_integration.sh [logfile]
#
# Returns: PASS (with optional SKIP semantics if payload absent)

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${1:-$PROJECT_DIR/.ciukios-testlogs/stage2-boot.log}"
OPENGEM_RUNTIME="$PROJECT_DIR/third_party/freedos/runtime/OPENGEM"

echo "[test-opengem] OpenGEM integration smoke test v1"
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
    echo "[PASS] OpenGEM smoke test complete (SKIP - no boot log)"
    exit 0
fi

# === Check 2: OpenGEM payload presence ===
OPENGEM_PRESENT=0
if [[ -d "$OPENGEM_RUNTIME" ]]; then
    # Check for any runnable entry
    for cand in GEM.BAT GEM.EXE DESKTOP.APP OPENGEM.BAT OPENGEM.EXE; do
        if find "$OPENGEM_RUNTIME" -maxdepth 3 -iname "$cand" -type f 2>/dev/null | grep -q .; then
            echo "[info] OpenGEM payload: PRESENT at $OPENGEM_RUNTIME (entry: $cand)"
            OPENGEM_PRESENT=1
            break
        fi
    done
    if [[ "$OPENGEM_PRESENT" -eq 0 ]]; then
        echo "[info] OpenGEM payload: PRESENT but no runnable entry found"
    fi
else
    echo "[info] OpenGEM payload: ABSENT (skip semantics active)"
fi

# === Check 3: Shell command surface ===
echo ""
echo "[info] checking shell command surface..."
check_marker "opengem" "INFO" || true

# === Check 4: Boot integrity (no panics) ===
echo ""
echo "[info] checking boot integrity..."
check_absent "[ panic ]"
check_absent "Invalid Opcode"
check_absent "#UD"
check_absent "General Protection Fault"

# === Check 5: OpenGEM-specific markers (only if payload present) ===
echo ""
if [[ "$OPENGEM_PRESENT" -eq 1 ]]; then
    echo "[info] checking OpenGEM launch markers (payload present)..."
    check_marker "[ app ] opengem launch requested" "WARN" || true
    check_marker "[ app ] opengem preflight started" "WARN" || true
    check_marker "[ app ] opengem preflight complete" "WARN" || true
else
    echo "[info] OpenGEM launch markers: SKIP (payload absent, not expected in log)"
    check_marker "[ app ] opengem launch requested" "INFO" || true
    check_marker "[ app ] opengem preflight started" "INFO" || true
fi

# === Summary ===
echo ""
echo "=== OpenGEM integration summary ==="
echo "  Payload:         $( [[ $OPENGEM_PRESENT -eq 1 ]] && echo PRESENT || echo ABSENT )"
echo "  Boot integrity:  $( [[ $FAIL_COUNT -eq 0 ]] && echo OK || echo ISSUES )"
echo "==================================="
echo ""

if [[ "$FAIL_COUNT" -gt 0 ]]; then
    echo "[FAIL] OpenGEM smoke test found $FAIL_COUNT issue(s)"
    exit 1
fi

if [[ "$OPENGEM_PRESENT" -eq 0 ]]; then
    echo "[PASS] OpenGEM smoke test complete (SKIP - payload absent)"
else
    echo "[PASS] OpenGEM smoke test complete"
fi
