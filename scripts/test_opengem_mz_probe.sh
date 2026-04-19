#!/usr/bin/env bash
# scripts/test_opengem_mz_probe.sh — OPENGEM-015 MZ deep-probe gate.
#
# Validates the MZ header parser: all 12 header fields exposed, the
# viability ladder, and that the probe is gated on classify="mz".

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

echo "=== OPENGEM-015 MZ-probe gate ==="

check_contains 'OPENGEM-015' "$SHELL_C" "shell.c: OPENGEM-015 sentinel"
check_contains 'stage2_opengem_mz_probe' "$SHELL_C" \
    "shell.c: mz probe helper declared"

# Marker prefixes (all 10 marker lines)
for m in \
    'OpenGEM: mz-probe begin path=' \
    'OpenGEM: mz-probe signature=' \
    'OpenGEM: mz-probe header e_cblp=0x' \
    'OpenGEM: mz-probe alloc e_minalloc=0x' \
    'OpenGEM: mz-probe stack e_ss=0x' \
    'OpenGEM: mz-probe entry e_cs=0x' \
    'OpenGEM: mz-probe reloc e_lfarlc=0x' \
    'OpenGEM: mz-probe layout load_bytes=0x' \
    'OpenGEM: mz-probe viability=' \
    'OpenGEM: mz-probe complete' ; do
    check_contains "$m" "$SHELL_C" "shell.c: marker '$m'"
done

# All 12 header fields surfaced
for f in e_cblp e_cp e_crlc e_cparhdr e_minalloc e_maxalloc e_ss e_sp e_cs e_ip e_lfarlc e_ovno; do
    check_contains " ${f}=0x" "$SHELL_C" "shell.c: field ${f}"
done

# Viability labels
check_contains 'runnable-real-mode' "$SHELL_C" "shell.c: viability=runnable-real-mode"
check_contains 'requires-extender'  "$SHELL_C" "shell.c: viability=requires-extender"
check_contains '"malformed"'        "$SHELL_C" "shell.c: viability=malformed"
check_contains 'skipped-non-mz'     "$SHELL_C" "shell.c: viability=skipped-non-mz"

# Reason tokens (7 stable)
for r in mz-v8086-candidate mz-load-exceeds-real-mode mz-max-alloc-64k \
         mz-header-too-small mz-header-malformed mz-non-mz-skipped mz-no-buffer ; do
    check_contains "$r" "$SHELL_C" "shell.c: reason $r"
done

# Signature status labels
check_contains '"too-small"' "$SHELL_C" "shell.c: status=too-small"
check_contains '"not-mz"'    "$SHELL_C" "shell.c: status=not-mz"

# Gated on classify=="mz"
if perl -0777 -ne 'exit(/classify_label\[0\]\s*==\s*.m.\s*&&\s*classify_label\[1\]\s*==\s*.z.[^}]*stage2_opengem_mz_probe\(/s ? 0 : 1)' "$SHELL_C"; then
    ok "shell.c: mz probe gated on classify_label==mz"
else
    ko "shell.c: mz probe not gated on classify_label"
fi

# Invocation after preload, before dispatch_native
if awk '
    /stage2_opengem_preload_absolute\(found_path, found_size,/ && !a { a=NR }
    /stage2_opengem_mz_probe\(found_path,/                     && !b { b=NR }
    /stage2_opengem_dispatch_native\(boot_info, handoff,/      && !c { c=NR }
    END { if (a && b && c && a<b && b<c) print "OK" }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: order preload -> mz_probe -> dispatch_native"
else
    ko "shell.c: mz probe misplaced"
fi

# Internal marker order (serial_write lines only)
if awk '
    /serial_write\("OpenGEM: mz-probe begin path=/     && !a { a=NR }
    /serial_write\("OpenGEM: mz-probe signature=/      && !b { b=NR }
    /"OpenGEM: mz-probe header e_cblp=0x/              && !c { c=NR }
    /serial_write\("OpenGEM: mz-probe alloc e_minalloc=0x/ && !d { d=NR }
    /serial_write\("OpenGEM: mz-probe stack e_ss=0x/   && !e { e=NR }
    /serial_write\("OpenGEM: mz-probe entry e_cs=0x/   && !f { f=NR }
    /serial_write\("OpenGEM: mz-probe reloc e_lfarlc=0x/ && !g { g=NR }
    /serial_write\("OpenGEM: mz-probe layout load_bytes=0x/ && !h { h=NR }
    /serial_write\("OpenGEM: mz-probe viability=/      && !i { i=NR }
    /serial_write\("OpenGEM: mz-probe complete/        && !j { j=NR }
    END {
        if (a && b && c && d && e && f && g && h && i && j &&
            a<b && b<c && c<d && d<e && e<f && f<g && g<h && h<i && i<j)
            print "OK"
    }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: mz-probe full marker ordering"
else
    ko "shell.c: mz-probe marker order broken"
fi

if grep -qE '^test-opengem-mz-probe:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-mz-probe target declared"
else
    ko "Makefile: test-opengem-mz-probe target missing"
fi

# Runtime probe (opt-in)
if [ -f "$BOOT_LOG" ]; then
    echo "[info] boot log present: $BOOT_LOG"
    if grep -qE 'OpenGEM: mz-probe begin path=.* size=0x[0-9a-f]{8}' "$BOOT_LOG"; then
        ok "boot-log: mz-probe begin well-formed"
    fi
    if grep -qE 'OpenGEM: mz-probe signature=(MZ|ZM|none) status=(ok|too-small|not-mz)' "$BOOT_LOG"; then
        ok "boot-log: mz-probe signature well-formed"
    fi
    if grep -qE 'OpenGEM: mz-probe viability=(runnable-real-mode|requires-extender|malformed|skipped-non-mz) reason=[a-z0-9-]+' "$BOOT_LOG"; then
        ok "boot-log: mz-probe viability well-formed"
    fi
else
    echo "[info] no boot log at $BOOT_LOG — marker checked statically only"
fi

echo ""
echo "=== opengem-mz-probe summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM MZ probe gate"
    exit 1
fi
echo "[PASS] OpenGEM MZ probe gate"
