#!/usr/bin/env bash
# scripts/test_opengem_input.sh — OPENGEM-005 host-side static gate.
#
# Validates the guarded INT 33h / cursor bridge for OpenGEM sessions,
# the ALT+G+Q escape-chord telemetry marker, and the integration with
# shell_run_opengem_interactive(). A runtime boot-log probe is opt-in.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MOUSE_HDR="$ROOT/stage2/include/mouse.h"
MOUSE_SRC="$ROOT/stage2/src/mouse.c"
SHELL_SRC="$ROOT/stage2/src/shell.c"
MAKEFILE="$ROOT/Makefile"
BOOT_LOG="$ROOT/.ciukios-testlogs/stage2-boot.log"

OK=0
FAIL=0

ok() { echo "[OK] $1"; OK=$((OK+1)); }
ko() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

check_file() {
    [ -f "$1" ] && ok "file: ${1##*/}" || ko "file missing: $1"
}

check_contains() {
    local needle="$1" path="$2" label="$3"
    if grep -qF -- "$needle" "$path"; then ok "$label"
    else ko "$label (missing: $needle — file: $path)"; fi
}

echo "=== OPENGEM-005 input/mouse smoke gate ==="

check_file "$MOUSE_HDR"
check_file "$MOUSE_SRC"
check_file "$SHELL_SRC"
check_file "$MAKEFILE"

# --- mouse.h: int33_hooks_t append-only surface ---
check_contains 'OPENGEM-005' "$MOUSE_HDR" "mouse.h: OPENGEM-005 marker"
check_contains 'typedef struct int33_hooks' "$MOUSE_HDR" "mouse.h: int33_hooks struct"
check_contains 'STAGE2_INT33_HOOKS_VERSION 1' "$MOUSE_HDR" "mouse.h: hooks version constant"
check_contains 'on_session_enter' "$MOUSE_HDR" "mouse.h: on_session_enter field"
check_contains 'on_session_exit' "$MOUSE_HDR" "mouse.h: on_session_exit field"
check_contains 'on_mouse_event' "$MOUSE_HDR" "mouse.h: on_mouse_event field"
check_contains 'stage2_mouse_set_opengem_hooks' "$MOUSE_HDR" "mouse.h: set_opengem_hooks decl"
check_contains 'stage2_mouse_opengem_session_enter' "$MOUSE_HDR" "mouse.h: session_enter decl"
check_contains 'stage2_mouse_opengem_session_exit' "$MOUSE_HDR" "mouse.h: session_exit decl"
check_contains 'stage2_mouse_opengem_cursor_quiesced' "$MOUSE_HDR" "mouse.h: cursor_quiesced decl"

# --- mouse.c: implementation + markers ---
check_contains 'OPENGEM-005' "$MOUSE_SRC" "mouse.c: OPENGEM-005 marker"
check_contains 'stage2_mouse_set_opengem_hooks' "$MOUSE_SRC" "mouse.c: set_opengem_hooks impl"
check_contains 'stage2_mouse_opengem_session_enter' "$MOUSE_SRC" "mouse.c: session_enter impl"
check_contains 'stage2_mouse_opengem_session_exit' "$MOUSE_SRC" "mouse.c: session_exit impl"
check_contains 'stage2_mouse_opengem_cursor_quiesced' "$MOUSE_SRC" "mouse.c: cursor_quiesced impl"
check_contains '[ mouse ] opengem session: cursor disabled' "$MOUSE_SRC" \
    "mouse.c: marker cursor disabled"
check_contains '[ mouse ] opengem session: cursor restored' "$MOUSE_SRC" \
    "mouse.c: marker cursor restored"
check_contains '[ mouse ] opengem hook installed' "$MOUSE_SRC" \
    "mouse.c: marker hook installed"

# --- shell.c: integration ---
check_contains 'stage2_mouse_opengem_session_enter()' "$SHELL_SRC" \
    "shell.c: calls session_enter before shell_run"
check_contains 'stage2_mouse_opengem_session_exit()' "$SHELL_SRC" \
    "shell.c: calls session_exit after shell_run"
check_contains 'stage2_mouse_opengem_cursor_quiesced()' "$SHELL_SRC" \
    "shell.c: mode-13 cursor checks quiesced flag"
check_contains '[ kbd ] opengem escape chord: alt+g+q detected' "$SHELL_SRC" \
    "shell.c: emits ALT+G+Q escape chord marker"

# --- Makefile target ---
if grep -qE '^test-opengem-input:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-input target declared"
else
    ko "Makefile: test-opengem-input target missing"
fi

# --- Opt-in boot-log probe ---
if [ -f "$BOOT_LOG" ]; then
    for m in \
        '[ mouse ] opengem session: cursor disabled' \
        '[ mouse ] opengem session: cursor restored'
    do
        if grep -qF -- "$m" "$BOOT_LOG"; then
            ok "boot-log: '$m'"
        else
            ko "boot-log: missing '$m'"
        fi
    done
else
    echo "[info] no boot log at $BOOT_LOG — skipping runtime marker probe"
fi

echo ""
echo "=== opengem-input smoke summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM input smoke gate"
    exit 1
fi
echo "[PASS] OpenGEM input smoke gate"
