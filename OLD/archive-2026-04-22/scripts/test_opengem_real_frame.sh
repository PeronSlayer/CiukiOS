#!/usr/bin/env bash
# scripts/test_opengem_real_frame.sh — OPENGEM-008 real first-frame
# hook + session-duration gate.
#
# Validates that gfx_modes.c arms/disarms the real-blit marker, that
# shell.c brackets the shell_run() dispatch with the arm/disarm and
# emits the session duration line, and that the present path emits
# `OpenGEM: desktop frame blitted` exactly once on the non-cached
# branch. A runtime boot-log probe is opt-in.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GFX_H="$ROOT/stage2/include/gfx_modes.h"
GFX_C="$ROOT/stage2/src/gfx_modes.c"
SHELL_C="$ROOT/stage2/src/shell.c"
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

echo "=== OPENGEM-008 real first-frame + session-duration gate ==="

# --- gfx_modes.h: append-only ABI ---
check_contains 'OPENGEM-008' "$GFX_H" "gfx_modes.h: OPENGEM-008 marker"
check_contains 'gfx_mode_opengem_arm_first_frame' "$GFX_H" \
    "gfx_modes.h: arm_first_frame decl"
check_contains 'gfx_mode_opengem_disarm_first_frame' "$GFX_H" \
    "gfx_modes.h: disarm_first_frame decl"
check_contains 'gfx_mode_opengem_first_frame_armed' "$GFX_H" \
    "gfx_modes.h: first_frame_armed query decl"

# --- gfx_modes.c: implementation + marker emission ---
check_contains 'OPENGEM-008' "$GFX_C" "gfx_modes.c: OPENGEM-008 marker"
check_contains 'g_opengem_first_frame_armed' "$GFX_C" \
    "gfx_modes.c: arm state declared"
check_contains 'OpenGEM: desktop frame blitted' "$GFX_C" \
    "gfx_modes.c: real first-frame marker"
check_contains 'gfx_mode_opengem_arm_first_frame' "$GFX_C" \
    "gfx_modes.c: arm impl"
check_contains 'gfx_mode_opengem_disarm_first_frame' "$GFX_C" \
    "gfx_modes.c: disarm impl"

# The marker must fire on the real-blit branch (after successful
# gfx_mode13_present_plane), not on the cached-noop branch.
if awk '
    /gfx_mode13_present_plane\(\)/ { seen_real=1; next }
    seen_real && /OpenGEM: desktop frame blitted/ { print "OK"; exit }
' "$GFX_C" | grep -q OK; then
    ok "gfx_modes.c: marker emits on real-blit branch (post-upscale)"
else
    ko "gfx_modes.c: marker not on real-blit branch"
fi

# Must auto-disarm (single-shot).
if grep -qE 'g_opengem_first_frame_armed[[:space:]]*=[[:space:]]*0' "$GFX_C"; then
    ok "gfx_modes.c: marker auto-disarms (single-shot)"
else
    ko "gfx_modes.c: missing auto-disarm"
fi

# --- shell.c: wiring + duration line ---
check_contains 'OPENGEM-008' "$SHELL_C" "shell.c: OPENGEM-008 marker"
check_contains 'OPENGEM-009' "$SHELL_C" "shell.c: OPENGEM-009 marker"
check_contains 'gfx_mode_opengem_arm_first_frame()' "$SHELL_C" \
    "shell.c: arm call before shell_run"
check_contains 'gfx_mode_opengem_disarm_first_frame()' "$SHELL_C" \
    "shell.c: disarm call after shell_run"
check_contains 'OpenGEM: runtime session duration=' "$SHELL_C" \
    "shell.c: duration line prefix"
check_contains 'stage2_timer_ticks()' "$SHELL_C" \
    "shell.c: uses PIT ticks as duration source (OPENGEM-009)"
if grep -qF 'suffix = " ms\n"' "$SHELL_C"; then
    ok "shell.c: duration suffix is ms (OPENGEM-009)"
else
    ko "shell.c: duration suffix is not ms"
fi

# Ordering: arm must appear after session_enter and before shell_run.
if awk '
    /stage2_mouse_opengem_session_enter\(\)/ { seen_enter=1; next }
    seen_enter && /gfx_mode_opengem_arm_first_frame\(\)/ { have_arm=1 }
    seen_enter && have_arm && /shell_run\(boot_info, handoff, found_path\);/ {
        print "OK"; exit
    }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: arm ordered session_enter -> arm -> shell_run"
else
    ko "shell.c: arm ordering broken"
fi

# disarm + duration must come after shell_run and before session_exit.
if awk '
    /shell_run\(boot_info, handoff, found_path\);/ { seen_run=1; next }
    seen_run && /gfx_mode_opengem_disarm_first_frame\(\)/ { have_dis=1 }
    seen_run && /OpenGEM: runtime session duration=/ { have_dur=1 }
    seen_run && have_dis && have_dur && /stage2_mouse_opengem_session_exit\(\)/ {
        print "OK"; exit
    }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: disarm+duration ordered shell_run -> disarm+duration -> session_exit"
else
    ko "shell.c: disarm/duration ordering broken"
fi

# --- Makefile target ---
if grep -qE '^test-opengem-real-frame:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-real-frame target declared"
else
    ko "Makefile: test-opengem-real-frame target missing"
fi

# --- Runtime boot-log probe (opt-in) ---
if [ -f "$BOOT_LOG" ]; then
    echo "[info] boot log present: $BOOT_LOG"
    if grep -qF 'OpenGEM: desktop frame blitted' "$BOOT_LOG"; then
        ok "boot-log: 'OpenGEM: desktop frame blitted' observed"
    else
        echo "[info] 'OpenGEM: desktop frame blitted' not in log (expected if no OpenGEM session ran)"
    fi
    if grep -qE 'OpenGEM: runtime session duration=[0-9]+ ms' "$BOOT_LOG"; then
        ok "boot-log: session-duration line observed (ms, OPENGEM-009)"
    else
        echo "[info] session-duration (ms) not in log (expected if no OpenGEM session ran)"
    fi
else
    echo "[info] no boot log at $BOOT_LOG — runtime markers checked statically only"
fi

echo ""
echo "=== opengem-real-frame summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM real-frame gate"
    exit 1
fi
echo "[PASS] OpenGEM real-frame gate"
