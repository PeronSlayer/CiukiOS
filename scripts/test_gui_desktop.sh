#!/usr/bin/env bash
set -euo pipefail

# GUI Desktop Test Helper
# Validates UI markers for desktop GUI system
# Usage: ./scripts/test_gui_desktop.sh [logfile]

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${1:-$PROJECT_DIR/.ciukios-testlogs/stage2-boot.log}"

if [[ ! -f "$LOG_FILE" ]]; then
    echo "[FAIL] Log file not found: $LOG_FILE" >&2
    exit 1
fi

echo "[test-gui-desktop] validating GUI markers..."

# GUI markers from D1-D4
# D1: Layout system v2
# D2: Window chrome v2
# D3: Interaction loop (interactive only)
# D4: Launcher dispatch (interactive only)

gui_markers=(
    "[ ui ] desktop layout v2 active"
    "[ ui ] window chrome v2 ready"
    "[ ui ] desktop shell surface active"
    "[ ui ] alignment surgical v6 active"
)

gui_interactive_markers=(
    "[ ui ] desktop interaction active"
    "[ ui ] launcher dispatch v2"
)

echo "[info] checking GUI architecture markers (non-interactive)..."
for marker in "${gui_markers[@]}"; do
    if grep -Fq "$marker" "$LOG_FILE" 2>/dev/null; then
        echo "[OK] found: $marker"
    else
        echo "[WARN] absent: $marker (expected in GUI builds)"
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

echo "[PASS] GUI marker validation complete"
