#!/usr/bin/env bash
# scripts/test_opengem_native_dispatch.sh — OPENGEM-014 native dispatch gate.
#
# Validates that the absolute-path preload verdict is consumed by a
# native dispatcher that bypasses shell_run() for BAT and COM targets.

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

echo "=== OPENGEM-014 native-dispatch gate ==="

check_contains 'OPENGEM-014' "$SHELL_C" "shell.c: OPENGEM-014 sentinel"
check_contains 'stage2_opengem_dispatch_native' "$SHELL_C" \
    "shell.c: native dispatcher declared"

# Preload signature exposes out-params
check_contains 'out_verdict' "$SHELL_C" "shell.c: preload exposes out_verdict"
check_contains 'out_reason'  "$SHELL_C" "shell.c: preload exposes out_reason"
check_contains 'out_read_bytes' "$SHELL_C" "shell.c: preload exposes out_read_bytes"

# Dispatcher markers (frozen)
check_contains 'OpenGEM: native-dispatch begin path=' "$SHELL_C" \
    "shell.c: native-dispatch begin marker"
check_contains ' kind=' "$SHELL_C" \
    "shell.c: native-dispatch kind token"
check_contains 'OpenGEM: native-dispatch bat=invoked'  "$SHELL_C" \
    "shell.c: bat=invoked marker"
check_contains 'OpenGEM: native-dispatch com=invoked'  "$SHELL_C" \
    "shell.c: com=invoked marker"
check_contains 'OpenGEM: native-dispatch com=failed'   "$SHELL_C" \
    "shell.c: com=failed marker"
check_contains 'OpenGEM: native-dispatch complete errorlevel=' "$SHELL_C" \
    "shell.c: native-dispatch complete marker"

# Actual dispatch calls (real execution change)
check_contains 'shell_run_batch_file(boot_info, handoff, path)' "$SHELL_C" \
    "shell.c: invokes shell_run_batch_file on absolute path"
check_contains 'shell_run_staged_image(boot_info, handoff, basename, read_bytes,' "$SHELL_C" \
    "shell.c: invokes shell_run_staged_image on staged buffer"

# Preload verdict now emits dispatch-native for bat/com (static
# inspection of the bat/com branches in stage2_opengem_preload_absolute).
if perl -0777 -ne 'exit(/classify\[2\]\s*==\s*.t.\s*\)\s*\{[^}]*verdict\s*=\s*"dispatch-native"/s ? 0 : 1)' "$SHELL_C"; then
    ok "shell.c: preload emits dispatch-native for bat"
else
    ko "shell.c: bat verdict not promoted to dispatch-native"
fi

if perl -0777 -ne 'exit(/classify\[2\]\s*==\s*.m.\s*\)\s*\{[^}]*verdict\s*=\s*"dispatch-native"/s ? 0 : 1)' "$SHELL_C"; then
    ok "shell.c: preload emits dispatch-native for com"
else
    ko "shell.c: com verdict not promoted to dispatch-native"
fi

# MZ branch still defers
if perl -0777 -ne 'exit(/classify\[1\]\s*==\s*.z.\s*\)\s*\{[^}]*"mz-16bit-pending"[^}]*"defer-to-shell-run"|classify\[1\]\s*==\s*.z.\s*\)\s*\{[^}]*"defer-to-shell-run"[^}]*"mz-16bit-pending"/s ? 0 : 1)' "$SHELL_C"; then
    ok "shell.c: MZ still defers to shell_run (pending extender)"
else
    ko "shell.c: MZ branch missing defer-to-shell-run + mz-16bit-pending"
fi

# Call-site branching: dispatch_native() return gates shell_run()
if awk '
    /stage2_opengem_dispatch_native\(boot_info, handoff,/ { a=1; next }
    a && /shell_run\(boot_info, handoff, found_path\);/ { ok=1; exit }
    END { if (ok) print "OK" }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: shell_run() reached via else branch of native dispatcher"
else
    ko "shell.c: shell_run() no longer reachable from opengem interactive"
fi

# Dispatcher invocation order inside shell_run_opengem_interactive
if awk '
    /stage2_opengem_preload_absolute\(found_path, found_size,/ && !a { a=NR }
    /stage2_opengem_dispatch_native\(boot_info, handoff,/      && !b { b=NR }
    END { if (a && b && a<b) print "OK" }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: order preload -> dispatch_native"
else
    ko "shell.c: dispatch_native not placed after preload"
fi

# Internal marker order inside dispatcher (only count serial_write lines)
if awk '
    /serial_write\("OpenGEM: native-dispatch begin path=/       && !a { a=NR }
    /serial_write\("OpenGEM: native-dispatch (bat|com)=/        && !b { b=NR }
    /"OpenGEM: native-dispatch complete errorlevel=/            && !c { c=NR; }
    END { if (a && b && c && a<b && b<c) print "OK" }
' "$SHELL_C" | grep -q OK; then
    ok "shell.c: dispatcher emits begin<invoked/failed<complete"
else
    ko "shell.c: dispatcher marker order broken"
fi

if grep -qE '^test-opengem-native-dispatch:' "$MAKEFILE"; then
    ok "Makefile: test-opengem-native-dispatch target declared"
else
    ko "Makefile: test-opengem-native-dispatch target missing"
fi

# Runtime boot-log probe (opt-in)
if [ -f "$BOOT_LOG" ]; then
    echo "[info] boot log present: $BOOT_LOG"
    if grep -qE 'OpenGEM: preload verdict=dispatch-native reason=(bat-interp-ready|com-runtime-ready)' "$BOOT_LOG"; then
        ok "boot-log: preload promoted to dispatch-native"
    fi
    if grep -qE 'OpenGEM: native-dispatch begin path=.* kind=(bat|com) reason=[a-z0-9-]+' "$BOOT_LOG"; then
        ok "boot-log: native-dispatch begin well-formed"
    fi
    if grep -qE 'OpenGEM: native-dispatch (bat|com)=(invoked|failed)' "$BOOT_LOG"; then
        ok "boot-log: native-dispatch invocation marker present"
    fi
    if grep -qE 'OpenGEM: native-dispatch complete errorlevel=[0-9]+' "$BOOT_LOG"; then
        ok "boot-log: native-dispatch complete well-formed"
    fi
else
    echo "[info] no boot log at $BOOT_LOG — marker checked statically only"
fi

echo ""
echo "=== opengem-native-dispatch summary: $OK OK / $FAIL FAIL ==="
if [ "$FAIL" -gt 0 ]; then
    echo "[FAIL] OpenGEM native dispatch gate"
    exit 1
fi
echo "[PASS] OpenGEM native dispatch gate"
