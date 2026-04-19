#!/usr/bin/env bash
# scripts/test_opengem_extender.sh — OPENGEM-011 extender readiness
# probe gate.
#
# Validates that the DOS extender readiness probe is wired, invokes
# the existing INT 2Fh AX=1687h DPMI installation-check handler,
# emits the frozen marker set, and is called from
# shell_run_opengem_interactive() between the OPENGEM-010 dispatch
# marker and shell_run().

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

echo "=== OPENGEM-011 extender readiness probe gate ==="

check_contains 'OPENGEM-011' "$SHELL_C" "shell.c: OPENGEM-011 sentinel"
check_contains 'stage2_opengem_probe_extender' "$SHELL_C" \
    "shell.c: probe function declared"
check_contains 'regs.ax = 0x1687U' "$SHELL_C" \
    "shell.c: probe issues INT 2Fh AX=1687h"
check_contains 'shell_com_int2f((ciuki_dos_context_t *)0, &regs)' "$SHELL_C" \
    "shell.c: probe invokes INT 2Fh handler"

check_contains 'OpenGEM: extender probe begin' "$SHELL_C" \
    "shell.c: begin marker"
check_contains 'OpenGEM: extender dpmi installed=' "$SHELL_C" \
    "shell.c: installed-flag marker prefix"
check_contains ' flags=0x' "$SHELL_C" \
    "shell.c: flags token"
check_contains 'OpenGEM: extender mode=dpmi-stub' "$SHELL_C" \
    "shell.c: mode=dpmi-stub branch"
check_contains 'OpenGEM: extender mode=none' "$SHELL_C" \
    "shell.c: mode=none branch"
check_contains 'OpenGEM: extender probe complete' "$SHELL_C" \
    "shell.c: complete marker"

# Ordering: dispatch -> probe -> shell_run
if awk '
    /OpenGEM: dispatch target=/ { seen_dispatch=1; next }
    seen_dispatch && /stage2_opengem_probe_extender\(\)/ { seen_probe=1; next }
    seen_probe && /shell_run\(boot_info, handoff, found_path\);/ {
        print "OK"; exit
    }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: ordering dispatch -> probe -> shell_run"
else
    ko "shell.c: probe invocation ordering broken"
fi

# Marker emission order inside the probe: begin -> installed -> mode -> complete
if awk '
    /OpenGEM: extender probe begin/    && !a { a=NR }
    /OpenGEM: extender dpmi installed=/ && !b { b=NR }
    /OpenGEM: extender mode=/          && !c { c=NR }
    /OpenGEM: extender probe complete/  && !d { d=NR }
    END { if (a && b && c && d && a<b && b<c && c<d) print "OK" }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: probe emits begin<installed<mode<complete"
else
    ko "shell.c: probe marker emission order broken"
fi

if grep -qE '^test-opengem-extender:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-extender target declared"
else
    ko "Makefile: test-opengem-extender target missing"
fi

# Runtime boot-log probe (opt-in)
if [ -f "$BOOT_LOG" ]; then
    echo "[info] boot log present: $BOOT_LOG"
    if grep -qE 'OpenGEM: extender probe begin' "$BOOT_LOG" && \
       grep -qE 'OpenGEM: extender probe complete' "$BOOT_LOG"; then
        ok "boot-log: probe begin+complete observed"
    else
        echo "[info] probe markers absent (expected if no OpenGEM session ran)"
    fi
    if grep -qE 'OpenGEM: extender mode=(dpmi-stub|none)' "$BOOT_LOG"; then
        ok "boot-log: extender mode published"
    fi
    if grep -qE 'OpenGEM: extender dpmi installed=[01] flags=0x[0-9a-f]{4}' "$BOOT_LOG"; then
        ok "boot-log: installed+flags word well-formed"
    fi
else
    echo "[info] no boot log at $BOOT_LOG — marker checked statically only"
fi

echo ""
echo "=== opengem-extender summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM extender readiness gate"
    exit 1
fi
echo "[PASS] OpenGEM extender readiness gate"
