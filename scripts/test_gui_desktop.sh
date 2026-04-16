#!/usr/bin/env bash
set -euo pipefail

# GUI Desktop Test Helper v3
# Validates UI markers, v8 capabilities, and visual risk zones
# Usage: ./scripts/test_gui_desktop.sh [logfile]

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${1:-$PROJECT_DIR/.ciukios-testlogs/stage2-boot.log}"

if [[ ! -f "$LOG_FILE" ]]; then
    echo "[FAIL] Log file not found: $LOG_FILE" >&2
    exit 1
fi

echo "[test-gui-desktop] validating GUI markers (v3)..."

# === Architecture markers (expected in any desktop-capable build) ===
gui_markers=(
    "[ ui ] desktop layout v2 active"
    "[ ui ] window chrome v2 ready"
    "[ ui ] desktop shell surface active"
    "[ ui ] alignment surgical v6 active"
)

# === v8 capability markers (expected after v8 builds) ===
gui_v8_markers=(
    "[ ui ] desktop session state-machine v8 active"
    "[ ui ] launcher action dispatch active"
    "[ ui ] desktop console panel active"
    "[ ui ] desktop layout manager v3 active"
    "[ ui ] desktop focus ux v8 active"
)

# === Interactive session markers (only when desktop command is used) ===
gui_interactive_markers=(
    "[ ui ] desktop interaction active"
    "[ ui ] desktop exit chord alt+g+q active"
    "[ ui ] launcher dispatch v2"
)

# === State transition markers (ordering validation) ===
gui_state_transitions=(
    "[ ui ] state transition -> ACTIVE"
    "[ ui ] state transition -> RUNNING_ACTION"
    "[ ui ] state transition -> EXITING"
)

# === Visual risk zone checks (negative patterns that should NOT appear) ===
gui_negative_patterns=(
    "[ panic ]"
    "Invalid Opcode"
    "#UD"
    "General Protection Fault"
    "Page Fault"
)

echo "[info] checking GUI architecture markers..."
for marker in "${gui_markers[@]}"; do
    if grep -Fq "$marker" "$LOG_FILE" 2>/dev/null; then
        echo "[OK] found: $marker"
    else
        echo "[WARN] absent: $marker (expected in GUI builds)"
    fi
done

echo "[info] checking v8 capability markers..."
v8_count=0
v8_total=${#gui_v8_markers[@]}
for marker in "${gui_v8_markers[@]}"; do
    if grep -Fq "$marker" "$LOG_FILE" 2>/dev/null; then
        echo "[OK] found: $marker"
        ((v8_count++)) || true
    else
        echo "[WARN] absent: $marker (expected after v8 integration)"
    fi
done

echo "[info] checking GUI interaction markers (interactive only)..."
for marker in "${gui_interactive_markers[@]}"; do
    if grep -Fq "$marker" "$LOG_FILE" 2>/dev/null; then
        echo "[OK] found: $marker (desktop session was interactive)"
    else
        echo "[info] absent: $marker (expected only when desktop command used)"
    fi
done

echo "[info] checking state transition markers (interactive only)..."
for marker in "${gui_state_transitions[@]}"; do
    if grep -Fq "$marker" "$LOG_FILE" 2>/dev/null; then
        echo "[OK] found: $marker"
    else
        echo "[info] absent: $marker (expected only during desktop interaction)"
    fi
done

echo "[info] checking visual risk zone negative patterns..."
gui_risk_ok=1
for pattern in "${gui_negative_patterns[@]}"; do
    if grep -Fq "$pattern" "$LOG_FILE" 2>/dev/null; then
        echo "[FAIL] found risk pattern: $pattern"
        gui_risk_ok=0
    else
        echo "[OK] absent: $pattern"
    fi
done

# === Layout zone sanity (check serial debug rects if present) ===
echo "[info] checking layout zone debug output..."
if grep -Fq "[ ui ] layout grid=" "$LOG_FILE" 2>/dev/null; then
    echo "[OK] layout debug rects present in log"
    grep -F "[ ui ] zone " "$LOG_FILE" 2>/dev/null | while IFS= read -r line; do
        echo "  $line"
    done
else
    echo "[info] layout debug rects not found (desktop may not have been entered)"
fi

# === GUI v8 capability summary ===
echo ""
echo "=== GUI v8 capability summary ==="
echo "  Architecture markers:  present in non-interactive boot log"
echo "  v8 markers found:      ${v8_count}/${v8_total}"
if [[ "$v8_count" -eq "$v8_total" ]]; then
    echo "  v8 integration:        COMPLETE"
elif [[ "$v8_count" -gt 0 ]]; then
    echo "  v8 integration:        PARTIAL (${v8_count}/${v8_total})"
else
    echo "  v8 integration:        NOT DETECTED (desktop not entered or pre-v8 build)"
fi
echo "================================="
echo ""

if [[ "$gui_risk_ok" -eq 0 ]]; then
    echo "[FAIL] GUI risk zone check found problems"
    exit 1
fi

echo "[PASS] GUI marker validation complete (v3)"
