#!/usr/bin/env bash
# scripts/test_opengem_absolute_dispatch.sh — OPENGEM-012 classify gate.
#
# Validates that the absolute-dispatch classification probe is
# wired, emits the frozen marker set with the stable reason
# tokens, and is invoked between the OPENGEM-011 extender probe
# and shell_run().

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

echo "=== OPENGEM-012 absolute-dispatch classify gate ==="

check_contains 'OPENGEM-012' "$SHELL_C" "shell.c: OPENGEM-012 sentinel"
check_contains 'stage2_opengem_classify_absolute' "$SHELL_C" \
    "shell.c: classify function declared"
check_contains 'shell_write_u32_hex' "$SHELL_C" \
    "shell.c: u32 hex helper present"

# Marker prefixes
check_contains 'OpenGEM: absolute dispatch begin path=' "$SHELL_C" \
    "shell.c: begin marker prefix"
check_contains ' size=0x' "$SHELL_C" \
    "shell.c: size=0x token"
check_contains 'OpenGEM: absolute dispatch classify=' "$SHELL_C" \
    "shell.c: classify marker prefix"
check_contains ' by=path' "$SHELL_C" \
    "shell.c: by=path qualifier"
check_contains 'OpenGEM: absolute dispatch capable=' "$SHELL_C" \
    "shell.c: capable marker prefix"
check_contains 'OpenGEM: absolute dispatch complete' "$SHELL_C" \
    "shell.c: complete marker"

# Classify labels
check_contains '"mz"' "$SHELL_C"      "shell.c: classify=mz label"
check_contains '"bat"' "$SHELL_C"     "shell.c: classify=bat label"
check_contains '"com"' "$SHELL_C"     "shell.c: classify=com label"
check_contains '"app"' "$SHELL_C"     "shell.c: classify=app label"
check_contains '"unknown"' "$SHELL_C" "shell.c: classify=unknown label"

# Reason tokens (stable contract)
check_contains '16bit-mz-extender-pending' "$SHELL_C" \
    "shell.c: reason 16bit-mz-extender-pending"
check_contains 'bat-interp-available' "$SHELL_C" \
    "shell.c: reason bat-interp-available"
check_contains 'com-runtime-available' "$SHELL_C" \
    "shell.c: reason com-runtime-available"
check_contains 'no-loader-for-app' "$SHELL_C" \
    "shell.c: reason no-loader-for-app"
check_contains 'unknown-extension' "$SHELL_C" \
    "shell.c: reason unknown-extension"
check_contains 'no-path' "$SHELL_C" \
    "shell.c: reason no-path"

# Invocation order: extender probe -> classify -> shell_run
if awk '
    /stage2_opengem_probe_extender\(\)/ { a=1; next }
    a && /stage2_opengem_classify_absolute\(found_path, found_size\)/ { b=1; next }
    b && /shell_run\(boot_info, handoff, found_path\);/ {
        print "OK"; exit
    }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: invocation order extender -> classify -> shell_run"
else
    ko "shell.c: invocation order broken"
fi

# Internal marker emission order within classify function
if awk '
    /OpenGEM: absolute dispatch begin path=/    && !a { a=NR }
    /OpenGEM: absolute dispatch classify=/      && !b { b=NR }
    /OpenGEM: absolute dispatch capable=/       && !c { c=NR }
    /OpenGEM: absolute dispatch complete/       && !d { d=NR }
    END { if (a && b && c && d && a<b && b<c && c<d) print "OK" }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: classify emits begin<classify<capable<complete"
else
    ko "shell.c: classify marker order broken"
fi

# found_size captured at find match
if grep -qF 'found_size = probe.size;' "$SHELL_C"; then
    ok "shell.c: found_size captured from probe"
else
    ko "shell.c: found_size not captured"
fi

if grep -qE '^test-opengem-absolute-dispatch:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-absolute-dispatch target declared"
else
    ko "Makefile: test-opengem-absolute-dispatch target missing"
fi

# Runtime boot-log probe (opt-in)
if [ -f "$BOOT_LOG" ]; then
    echo "[info] boot log present: $BOOT_LOG"
    if grep -qE 'OpenGEM: absolute dispatch begin path=.* size=0x[0-9a-f]{8}' "$BOOT_LOG"; then
        ok "boot-log: begin marker well-formed"
    fi
    if grep -qE 'OpenGEM: absolute dispatch classify=(mz|bat|com|app|unknown) by=path' "$BOOT_LOG"; then
        ok "boot-log: classify label valid"
    fi
    if grep -qE 'OpenGEM: absolute dispatch capable=[01] reason=[a-z0-9-]+' "$BOOT_LOG"; then
        ok "boot-log: capable+reason well-formed"
    fi
else
    echo "[info] no boot log at $BOOT_LOG — marker checked statically only"
fi

echo ""
echo "=== opengem-absolute-dispatch summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM absolute-dispatch classify gate"
    exit 1
fi
echo "[PASS] OpenGEM absolute-dispatch classify gate"
