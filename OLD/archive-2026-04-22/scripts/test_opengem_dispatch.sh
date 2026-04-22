#!/usr/bin/env bash
# scripts/test_opengem_dispatch.sh — OPENGEM-010 dispatch-target
# telemetry gate.
#
# Validates that shell_run_opengem_interactive() emits the
# dispatch-target marker immediately before calling shell_run(),
# that the marker exposes both path and kind tokens, that the
# probe list prefers the nested GEMVDI location over GEM.BAT, and
# that a runtime boot log (when provided) shows a real non-BAT
# dispatch when the OpenGEM payload is installed.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
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

echo "=== OPENGEM-010 dispatch-target telemetry gate ==="

# --- shell.c: marker emission + probe reorder ---
check_contains 'OPENGEM-010' "$SHELL_C" "shell.c: OPENGEM-010 marker"
check_contains '/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE' "$SHELL_C" \
    "shell.c: nested GEM.EXE in probe list"
check_contains 'OpenGEM: dispatch target=' "$SHELL_C" \
    "shell.c: dispatch target marker prefix"
check_contains 'kind=' "$SHELL_C" \
    "shell.c: dispatch target kind token"

# Probe reorder: nested GEM.EXE must precede GEM.BAT in the array.
if awk '
    /static const char \*paths\[\]/ { in_arr=1; next }
    in_arr && /GEMAPPS\/GEMSYS\/GEM.EXE/ { seen_exe=1; next }
    in_arr && /GEM.BAT/ {
        if (seen_exe) { print "OK" } else { print "BAD" }
        exit
    }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: probe list prefers nested GEM.EXE over GEM.BAT"
else
    ko "shell.c: probe list does not prefer nested GEM.EXE"
fi

# Dispatch marker must emit after arm and before shell_run().
if awk '
    /gfx_mode_opengem_arm_first_frame\(\)/ { seen_arm=1; next }
    seen_arm && /OpenGEM: dispatch target=/ { have_dispatch=1 }
    seen_arm && have_dispatch && /shell_run\(boot_info, handoff, found_path\);/ {
        print "OK"; exit
    }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: dispatch marker ordered arm -> dispatch -> shell_run"
else
    ko "shell.c: dispatch marker ordering broken"
fi

# --- Makefile target ---
if grep -qE '^test-opengem-dispatch:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-dispatch target declared"
else
    ko "Makefile: test-opengem-dispatch target missing"
fi

# --- Runtime boot-log probe (opt-in) ---
if [ -f "$BOOT_LOG" ]; then
    echo "[info] boot log present: $BOOT_LOG"
    if grep -qE 'OpenGEM: dispatch target=.* kind=(bat|exe|com|app)' "$BOOT_LOG"; then
        ok "boot-log: dispatch target+kind observed"
    else
        echo "[info] dispatch marker not in log (expected if no OpenGEM session ran)"
    fi
    # When the payload is installed, we expect the nested GEM.EXE
    # to be dispatched instead of GEM.BAT. This is advisory only.
    if grep -qE 'OpenGEM: dispatch target=/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM\.EXE kind=exe' "$BOOT_LOG"; then
        ok "boot-log: real GEM.EXE dispatched (payload installed)"
    else
        echo "[info] nested GEM.EXE not dispatched (payload absent or probe ordering bypassed)"
    fi
else
    echo "[info] no boot log at $BOOT_LOG — marker checked statically only"
fi

echo ""
echo "=== opengem-dispatch summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM dispatch gate"
    exit 1
fi
echo "[PASS] OpenGEM dispatch gate"
