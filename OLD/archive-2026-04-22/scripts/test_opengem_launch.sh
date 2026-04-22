#!/usr/bin/env bash
# scripts/test_opengem_launch.sh — OPENGEM-003 host-side static gate.
#
# Asserts that the OpenGEM launcher invocation path saves and restores
# the desktop (dock) state across the helper call, emits the Phase 3
# overlay markers, and renders the OpenGEM entry with a facsimile
# glyph. A runtime boot-log probe is opt-in.
#
# Gate contract is static-only so it runs cleanly on macOS hosts
# where `make test-stage2` is blocked.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHELL_SRC="$ROOT/stage2/src/shell.c"
UI_SRC="$ROOT/stage2/src/ui.c"
UI_HDR="$ROOT/stage2/include/ui.h"
MAKEFILE="$ROOT/Makefile"
BOOT_LOG="$ROOT/.ciukios-testlogs/stage2-boot.log"

OK=0
FAIL=0

ok()  { echo "[OK] $1"; OK=$((OK+1)); }
ko()  { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

check_file() {
    if [ -f "$1" ]; then ok "file: $(basename "$1")"
    else ko "file missing: $1"; fi
}

check_contains() {
    local needle="$1" path="$2" label="$3"
    if grep -qF -- "$needle" "$path"; then ok "$label"
    else ko "$label (missing: $needle — file: $path)"; fi
}

check_regex() {
    local regex="$1" path="$2" label="$3"
    if grep -qE -- "$regex" "$path"; then ok "$label"
    else ko "$label (regex not matched: $regex — file: $path)"; fi
}

echo "=== OPENGEM-003 launch/state smoke gate ==="

check_file "$SHELL_SRC"
check_file "$UI_SRC"
check_file "$UI_HDR"
check_file "$MAKEFILE"

# --- UI accessors (focus save/restore) --------------------------------
check_contains 'OPENGEM-003' "$UI_HDR" "ui.h: OPENGEM-003 marker present"
check_contains 'ui_get_launcher_focus' "$UI_HDR" "ui.h: declares ui_get_launcher_focus"
check_contains 'ui_set_launcher_focus' "$UI_HDR" "ui.h: declares ui_set_launcher_focus"
check_contains 'ui_launcher_item_count' "$UI_HDR" "ui.h: declares ui_launcher_item_count"

check_contains 'ui_get_launcher_focus' "$UI_SRC" "ui.c: defines ui_get_launcher_focus"
check_contains 'ui_set_launcher_focus' "$UI_SRC" "ui.c: defines ui_set_launcher_focus"
check_contains 'ui_launcher_item_count' "$UI_SRC" "ui.c: defines ui_launcher_item_count"
check_contains 'ui_launcher_display_for' "$UI_SRC" "ui.c: defines ui_launcher_display_for"
check_contains '"[G] OPENGEM"' "$UI_SRC" "ui.c: text-mode glyph facsimile '[G] OPENGEM'"
# Canonical action key must stay intact for dispatch/help/gate matching.
check_contains '"OPENGEM"' "$UI_SRC" "ui.c: canonical OPENGEM action key preserved"

# --- shell.c: OPENGEM-003 state save/restore + overlay markers --------
check_contains 'OPENGEM-003' "$SHELL_SRC" "shell.c: OPENGEM-003 marker present"
check_contains 'desktop_snapshot' "$SHELL_SRC" "shell.c: desktop_snapshot struct on stack"
check_contains 'ui_get_launcher_focus()' "$SHELL_SRC" "shell.c: captures launcher focus"
check_contains 'ui_set_launcher_focus(desktop_snapshot.launcher_focus)' "$SHELL_SRC" \
    "shell.c: restores launcher focus from snapshot"

# Serial markers (frozen vocabulary)
check_contains '[ ui ] opengem dock state saved: sel=' "$SHELL_SRC" \
    "shell.c: marker '[ ui ] opengem dock state saved: sel='"
check_contains '[ ui ] opengem overlay active' "$SHELL_SRC" \
    "shell.c: marker '[ ui ] opengem overlay active'"
check_contains '[ ui ] opengem overlay dismissed, state restored' "$SHELL_SRC" \
    "shell.c: marker '[ ui ] opengem overlay dismissed, state restored'"

# Modal fallback line when preflight fails.
check_contains 'OPENGEM: n/a - payload not installed' "$SHELL_SRC" \
    "shell.c: modal-style fallback line printed to text console"
check_contains 'OpenGEM running - press ALT+G+Q' "$SHELL_SRC" \
    "shell.c: overlay banner printed to text console"

# --- Makefile target --------------------------------------------------
if grep -qE '^test-opengem-launch:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-launch target declared"
else
    ko "Makefile: test-opengem-launch target missing"
fi

# --- Opt-in boot log probe -------------------------------------------
if [ -f "$BOOT_LOG" ]; then
    for m in \
        '[ ui ] opengem dock state saved: sel=' \
        '[ ui ] opengem overlay active' \
        '[ ui ] opengem overlay dismissed, state restored'
    do
        if grep -qF -- "$m" "$BOOT_LOG"; then
            ok "boot-log: contains '$m'"
        else
            ko "boot-log: missing '$m'"
        fi
    done
else
    echo "[info] no boot log at $BOOT_LOG — skipping runtime marker probe"
fi

echo ""
echo "=== opengem-launch smoke summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM launch smoke gate"
    exit 1
fi
echo "[PASS] OpenGEM launch smoke gate"
