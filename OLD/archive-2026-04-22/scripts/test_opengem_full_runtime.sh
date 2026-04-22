#!/usr/bin/env bash
# scripts/test_opengem_full_runtime.sh — OPENGEM-007 runtime gate.
#
# Validates that shell_run_opengem_interactive() emits the granular
# runtime markers that classify a real desktop-visible launch vs. a
# preflight-only pass, and that the historical markers are still
# present (backward-compat). A runtime boot-log probe is opt-in.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHELL_SRC="$ROOT/stage2/src/shell.c"
MAKEFILE="$ROOT/Makefile"
BOOT_LOG="${CIUKIOS_OPENGEM_BOOT_LOG:-$ROOT/.ciukios-testlogs/stage2-boot.log}"

OK=0
FAIL=0

ok() { echo "[OK] $1"; OK=$((OK+1)); }
ko() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

check_contains() {
    local needle="$1" path="$2" label="$3"
    if grep -qF -- "$needle" "$path"; then ok "$label"
    else ko "$label (missing: $needle)"; fi
}

echo "=== OPENGEM-007 full-runtime gate ==="

# --- OPENGEM-007 granular runtime markers (new) ---
check_contains 'OpenGEM: runtime handoff begin' "$SHELL_SRC" \
    "shell.c: runtime handoff begin marker"
check_contains 'OpenGEM: desktop first frame presented' "$SHELL_SRC" \
    "shell.c: desktop first frame marker"
check_contains 'OpenGEM: interactive session active' "$SHELL_SRC" \
    "shell.c: interactive session active marker"
check_contains 'OpenGEM: runtime session ended' "$SHELL_SRC" \
    "shell.c: runtime session ended marker"

# --- Historical markers preserved (backward-compat) ---
check_contains 'OpenGEM: launcher window initialized' "$SHELL_SRC" \
    "shell.c: legacy launcher-initialized marker preserved"
check_contains 'OpenGEM: exit detected, returning to shell' "$SHELL_SRC" \
    "shell.c: legacy exit marker preserved"
check_contains '[ ui ] opengem overlay active' "$SHELL_SRC" \
    "shell.c: overlay-active marker preserved (OPENGEM-003)"
check_contains '[ ui ] opengem overlay dismissed, state restored' "$SHELL_SRC" \
    "shell.c: overlay-dismissed marker preserved (OPENGEM-003)"
check_contains 'stage2_mouse_opengem_session_enter()' "$SHELL_SRC" \
    "shell.c: mouse session enter still wired (OPENGEM-005)"
check_contains 'stage2_mouse_opengem_session_exit()' "$SHELL_SRC" \
    "shell.c: mouse session exit still wired (OPENGEM-005)"
check_contains '[ kbd ] opengem escape chord: alt+g+q detected' "$SHELL_SRC" \
    "shell.c: ALT+G+Q exit chord marker preserved (OPENGEM-005)"

# --- Ordering contract: new markers must appear between
#     stage2_mouse_opengem_session_enter() and shell_run() ---
if awk '
    /stage2_mouse_opengem_session_enter\(\)/ { seen_enter=1; next }
    seen_enter && /OpenGEM: runtime handoff begin/       { have_begin=1 }
    seen_enter && /OpenGEM: desktop first frame presented/ { have_first=1 }
    seen_enter && /OpenGEM: interactive session active/   { have_active=1 }
    seen_enter && have_begin && have_first && have_active && /shell_run\(boot_info, handoff, found_path\);/ {
        print "ORDER_OK"; exit
    }
' "$SHELL_SRC" | grep -q ORDER_OK; then
    ok "shell.c: runtime markers emit between session_enter and shell_run"
else
    ko "shell.c: expected ordering session_enter -> begin+first+active -> shell_run not found"
fi

# --- "session ended" must come after shell_run and before session_exit ---
if awk '
    /shell_run\(boot_info, handoff, found_path\);/ { seen_run=1; next }
    seen_run && /OpenGEM: runtime session ended/ { have_end=1 }
    seen_run && have_end && /stage2_mouse_opengem_session_exit\(\)/ {
        print "ORDER_OK"; exit
    }
' "$SHELL_SRC" | grep -q ORDER_OK; then
    ok "shell.c: runtime session-ended emits between shell_run and session_exit"
else
    ko "shell.c: expected ordering shell_run -> session ended -> session_exit not found"
fi

# --- Makefile target ---
if grep -qE '^test-opengem-full-runtime:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-full-runtime target declared"
else
    ko "Makefile: test-opengem-full-runtime target missing"
fi

# --- Runtime boot-log probe (opt-in) ---
if [ -f "$BOOT_LOG" ]; then
    echo "[info] boot log present: $BOOT_LOG"
    for m in \
        'OpenGEM: runtime handoff begin' \
        'OpenGEM: desktop first frame presented' \
        'OpenGEM: interactive session active' \
        'OpenGEM: runtime session ended'
    do
        if grep -qF -- "$m" "$BOOT_LOG"; then
            ok "boot-log runtime marker: '$m'"
        else
            ko "boot-log runtime marker missing: '$m'"
        fi
    done
else
    echo "[info] no boot log at $BOOT_LOG — runtime markers checked statically only"
fi

echo ""
echo "=== opengem-full-runtime summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM full-runtime gate"
    exit 1
fi
echo "[PASS] OpenGEM full-runtime gate"
