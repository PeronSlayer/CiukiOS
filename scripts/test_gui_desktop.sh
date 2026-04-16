#!/usr/bin/env bash
set -euo pipefail

# GUI Desktop Test Helper v2
# Validates UI markers and visual risk zones for desktop GUI system
# Usage: ./scripts/test_gui_desktop.sh [logfile]

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${1:-$PROJECT_DIR/.ciukios-testlogs/stage2-boot.log}"

if [[ ! -f "$LOG_FILE" ]]; then
    echo "[FAIL] Log file not found: $LOG_FILE" >&2
    exit 1
fi

echo "[test-gui-desktop] validating GUI markers (v2)..."

# === Architecture markers (expected in any desktop-capable build) ===
gui_markers=(
    "[ ui ] desktop layout v2 active"
    "[ ui ] window chrome v2 ready"
    "[ ui ] desktop shell surface active"
    "[ ui ] alignment surgical v6 active"
)

# === Alignment/clipping markers (expected after v6+ builds) ===
gui_alignment_markers=(
    "[ ui ] alignment surgical v6 active"
)

# === Interactive session markers (only when desktop command is used) ===
gui_interactive_markers=(
    "[ ui ] desktop interaction active"
    "[ ui ] desktop exit chord alt+g+q active"
    "[ ui ] launcher dispatch v2"
)

# === Visual risk zone checks (negative patterns that should NOT appear) ===
gui_negative_patterns=(
    "[ panic ]"
    "Invalid Opcode"
    "#UD"
)

echo "[info] checking GUI architecture markers..."
for marker in "${gui_markers[@]}"; do
    if grep -Fq "$marker" "$LOG_FILE" 2>/dev/null; then
        echo "[OK] found: $marker"
    else
        echo "[WARN] absent: $marker (expected in GUI builds)"
    fi
done

echo "[info] checking alignment/clipping markers..."
for marker in "${gui_alignment_markers[@]}"; do
    if grep -Fq "$marker" "$LOG_FILE" 2>/dev/null; then
        echo "[OK] found: $marker"
    else
        echo "[WARN] absent: $marker (expected after alignment v6)"
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

if [[ "$gui_risk_ok" -eq 0 ]]; then
    echo "[FAIL] GUI risk zone check found problems"
    exit 1
fi

echo "[PASS] GUI marker validation complete (v2)"
