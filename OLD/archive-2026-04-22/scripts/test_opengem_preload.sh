#!/usr/bin/env bash
# scripts/test_opengem_preload.sh — OPENGEM-013 preload gate.
#
# Validates that the absolute-path preload probe is wired, issues
# a real fat_read_file() into the runtime payload buffer, emits
# the frozen marker set with the stable reason tokens, and is
# invoked between the OPENGEM-012 classify probe and shell_run().

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

echo "=== OPENGEM-013 preload gate ==="

check_contains 'OPENGEM-013' "$SHELL_C" "shell.c: OPENGEM-013 sentinel"
check_contains 'stage2_opengem_preload_absolute' "$SHELL_C" \
    "shell.c: preload function declared"
check_contains 'fat_read_file(' "$SHELL_C" \
    "shell.c: fat_read_file invoked"
check_contains 'SHELL_RUNTIME_COM_ENTRY_ADDR' "$SHELL_C" \
    "shell.c: staged into runtime buffer"

# Marker prefixes
check_contains 'OpenGEM: preload begin path=' "$SHELL_C" \
    "shell.c: begin marker prefix"
check_contains ' expect_size=0x' "$SHELL_C" \
    "shell.c: expect_size token"
check_contains 'OpenGEM: preload read bytes=0x' "$SHELL_C" \
    "shell.c: read marker prefix"
check_contains ' status=' "$SHELL_C" \
    "shell.c: status token"
check_contains 'OpenGEM: preload signature=' "$SHELL_C" \
    "shell.c: signature marker prefix"
check_contains ' match=' "$SHELL_C" \
    "shell.c: match token"
check_contains 'OpenGEM: preload verdict=' "$SHELL_C" \
    "shell.c: verdict marker prefix"
check_contains ' reason=' "$SHELL_C" \
    "shell.c: reason token"
check_contains 'OpenGEM: preload complete' "$SHELL_C" \
    "shell.c: complete marker"

# Status labels
check_contains '"ok"'        "$SHELL_C" "shell.c: status=ok"
check_contains '"too-large"' "$SHELL_C" "shell.c: status=too-large"
check_contains '"io-error"'  "$SHELL_C" "shell.c: status=io-error"
check_contains '"no-path"'   "$SHELL_C" "shell.c: status=no-path"

# Signature labels
check_contains '"MZ"'      "$SHELL_C" "shell.c: signature=MZ"
check_contains '"ZM"'      "$SHELL_C" "shell.c: signature=ZM"
check_contains '"text"'    "$SHELL_C" "shell.c: signature=text"
check_contains '"empty"'   "$SHELL_C" "shell.c: signature=empty"
check_contains '"unknown"' "$SHELL_C" "shell.c: signature=unknown"

# Verdict + reason tokens
check_contains 'dispatch-native'     "$SHELL_C" "shell.c: verdict=dispatch-native literal"
check_contains 'defer-to-shell-run'  "$SHELL_C" "shell.c: verdict=defer-to-shell-run literal"
check_contains 'preload-empty'       "$SHELL_C" "shell.c: reason preload-empty"
check_contains 'preload-too-large'   "$SHELL_C" "shell.c: reason preload-too-large"
check_contains 'preload-io-error'    "$SHELL_C" "shell.c: reason preload-io-error"
check_contains 'preload-no-path'     "$SHELL_C" "shell.c: reason preload-no-path"
check_contains 'signature-mismatch'  "$SHELL_C" "shell.c: reason signature-mismatch"
check_contains 'mz-16bit-pending'    "$SHELL_C" "shell.c: reason mz-16bit-pending"
check_contains 'bat-interp-ready'    "$SHELL_C" "shell.c: reason bat-interp-ready"
check_contains 'com-runtime-ready'   "$SHELL_C" "shell.c: reason com-runtime-ready"
check_contains 'unsupported-app'     "$SHELL_C" "shell.c: reason unsupported-app"
check_contains 'unsupported-unknown' "$SHELL_C" "shell.c: reason unsupported-unknown"

# Invocation order: classify -> preload -> shell_run
if awk '
    /stage2_opengem_classify_absolute\(found_path, found_size\)/ { a=1; next }
    a && /stage2_opengem_preload_absolute\(found_path, found_size,/ { b=1; next }
    b && /shell_run\(boot_info, handoff, found_path\);/ {
        print "OK"; exit
    }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: invocation order classify -> preload -> shell_run"
else
    ko "shell.c: invocation order broken"
fi

# Internal marker emission order inside preload
if awk '
    /OpenGEM: preload begin path=/       && !a { a=NR }
    /OpenGEM: preload read bytes=0x/     && !b { b=NR }
    /OpenGEM: preload signature=/        && !c { c=NR }
    /OpenGEM: preload verdict=/          && !d { d=NR }
    /OpenGEM: preload complete/          && !e { e=NR }
    END { if (a && b && c && d && e && a<b && b<c && c<d && d<e) print "OK" }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: preload emits begin<read<signature<verdict<complete"
else
    ko "shell.c: preload marker order broken"
fi

if grep -qE '^test-opengem-preload:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-preload target declared"
else
    ko "Makefile: test-opengem-preload target missing"
fi

# Runtime boot-log probe (opt-in)
if [ -f "$BOOT_LOG" ]; then
    echo "[info] boot log present: $BOOT_LOG"
    if grep -qE 'OpenGEM: preload begin path=.* expect_size=0x[0-9a-f]{8}' "$BOOT_LOG"; then
        ok "boot-log: preload begin well-formed"
    fi
    if grep -qE 'OpenGEM: preload read bytes=0x[0-9a-f]{8} status=(ok|too-large|io-error|no-path)' "$BOOT_LOG"; then
        ok "boot-log: preload read well-formed"
    fi
    if grep -qE 'OpenGEM: preload signature=(MZ|ZM|text|empty|unknown) match=[01]' "$BOOT_LOG"; then
        ok "boot-log: preload signature well-formed"
    fi
    if grep -qE 'OpenGEM: preload verdict=(dispatch-native|defer-to-shell-run) reason=[a-z0-9-]+' "$BOOT_LOG"; then
        ok "boot-log: preload verdict+reason well-formed"
    fi
else
    echo "[info] no boot log at $BOOT_LOG — marker checked statically only"
fi

echo ""
echo "=== opengem-preload summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM preload gate"
    exit 1
fi
echo "[PASS] OpenGEM preload gate"
