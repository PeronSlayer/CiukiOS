#!/usr/bin/env bash
# OPENGEM-030 — MZ live-switch gate wire-up in shell.c dosrun path.
#
# Verifies: shell.c consults vm86_live_switch_is_armed() only for MZ
# inputs, default-disarmed path emits the observability marker and
# falls through to the stage2-native dosrun (no behavior change),
# armed path additionally invokes vm86_live_switch_execute() (which
# still runs OPENGEM-027 retq stubs).
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf()   { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }

S=stage2/src/shell.c

gf "$S" "OPENGEM-030" "phase sentinel"
gf "$S" "#include \"vm86.h\"" "vm86.h include"

# Gate is MZ-only.
gf "$S" "if (is_mz) {" "mz gate guard"
gf "$S" "vm86_live_switch_is_armed()" "armed check"

# Markers.
gf "$S" "OpenGEM: mz-live-gate armed=1 action=execute-stubs" "armed marker"
gf "$S" "OpenGEM: mz-live-gate armed=0 fallback=defer-to-shell-run" "disarmed marker"
gf "$S" "OpenGEM: mz-live-gate fallthrough=stage2-dosrun" "fallthrough marker"

# Armed path calls execute (currently stubs).
gf "$S" "vm86_live_switch_execute();" "execute call"

# The emit_launch_marker must still appear after the gate (fallthrough
# behavior preserved).
if awk '
    /OpenGEM: mz-live-gate/ { gate_seen = 1 }
    /shell_dosrun_emit_launch_marker/ { if (gate_seen) found = 1 }
    END { exit found ? 0 : 1 }
' "$S"; then
    pass
else
    fail "shell_dosrun_emit_launch_marker must remain after mz-live-gate"
fi

# vm86_live_switch_arm must NOT appear in shell.c (only test code arms).
if grep -nE -w 'vm86_live_switch_arm\b' "$S" >/dev/null; then
    fail "shell.c must not call vm86_live_switch_arm (test-only)"
else
    pass
fi

# No inline-asm lgdt/lidt/iret added to shell.c.
if grep -nE '__asm__[^"]*"[^"]*(lgdt|lidt|iretd|iretq|iret|ljmp)' "$S" >/dev/null; then
    fail "inline asm lgdt/lidt/iret present in shell.c"
else
    pass
fi

# vm86_switch.S still stub-only.
SW=stage2/src/vm86_switch.S
if grep -nE '^[ \t]*(lgdt|lidt|iret|iretd|iretq|ljmp|lretq)([ \t]|$)' "$SW" >/dev/null; then
    fail "vm86_switch.S gained forbidden live instruction"
else
    pass
fi

# Makefile target.
gf Makefile "test-vm86-gem-wire" "makefile target"

echo "[summary] $OK OK / $FAIL FAIL"
[ "$FAIL" = "0" ] && echo "[PASS] OPENGEM-030 vm86 gem-wire gate" || echo "[FAIL] OPENGEM-030 vm86 gem-wire gate"
exit "$FAIL"
