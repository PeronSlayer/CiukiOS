#!/usr/bin/env bash
# OPENGEM-029 — v8086 armed-but-gated live-switch execute path.
#
# Static gate. Verifies the arm/disarm/execute API, the default-disarmed
# invariant, the magic-and-plan-gating contract, and that the only C
# call site of the vm86_switch.S trampolines is inside the armed path
# in vm86.c (no boot-path invocation).
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf()   { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }

H=stage2/include/vm86.h
C=stage2/src/vm86.c

gf "$C" "OPENGEM-029" "impl sentinel"
gf "$H" "OPENGEM-029" "header sentinel"

# Magic + status enum.
gf "$H" "#define VM86_LIVE_ARM_MAGIC 0x12860029u" "arm magic"
gf "$H" "typedef enum vm86_live_exec_status {" "status enum"
gf "$H" "VM86_LIVE_EXEC_BLOCKED_NOT_ARMED" "status not armed"
gf "$H" "VM86_LIVE_EXEC_BLOCKED_NO_PLAN" "status no plan"
gf "$H" "VM86_LIVE_EXEC_BLOCKED_BAD_PLAN" "status bad plan"
gf "$H" "VM86_LIVE_EXEC_INVOKED_STUBS" "status invoked stubs"

# API prototypes.
gf "$H" "int  vm86_live_switch_arm(u32 magic, const vm86_live_switch_plan *plan);" "arm proto"
gf "$H" "void vm86_live_switch_disarm(void);" "disarm proto"
gf "$H" "int  vm86_live_switch_is_armed(void);" "is_armed proto"
gf "$H" "const vm86_live_switch_plan *vm86_live_switch_get_plan(void);" "get_plan proto"
gf "$H" "vm86_live_exec_status vm86_live_switch_execute(void);" "execute proto"
gf "$H" "int vm86_live_switch_arm_probe(void);" "probe proto"

# Impl: default-disarmed static state.
gf "$C" "static int                            vm86_live_switch_armed_flag = 0;" "default disarmed flag"
gf "$C" "static const vm86_live_switch_plan   *vm86_live_switch_armed_plan = 0;" "default null plan"

# Impl: arm rejects wrong magic / null plan / non-ready plan.
gf "$C" "if (magic != VM86_LIVE_ARM_MAGIC) {" "arm magic check"
gf "$C" "reason=bad-magic" "arm bad magic marker"
gf "$C" "reason=null-plan" "arm null plan marker"
gf "$C" "reason=plan-not-ready" "arm not ready marker"

# Impl: execute blocked path emits marker + never calls stubs.
gf "$C" "if (!vm86_live_switch_armed_flag) {" "execute not-armed guard"
gf "$C" "blocked reason=not-armed" "execute blocked marker"

# Impl: armed path calls all three stubs in order.
gf "$C" "vm86_switch_long_to_pe32();" "long to pe32 call"
gf "$C" "vm86_switch_enter_v86_via_iret();" "enter v86 call"
gf "$C" "vm86_switch_pe32_to_long();" "pe32 to long call"
gf "$C" "vm86: live-switch execute begin mode=stub" "execute begin marker"
gf "$C" "vm86: live-switch execute complete mode=stub stubs=3" "execute complete marker"

# Probe ordering markers.
gf "$C" "vm86: live-switch arm-probe begin OPENGEM-029" "probe begin"
gf "$C" "vm86: live-switch arm-probe ready-surface=arm,disarm,execute-gate" "probe ready"
gf "$C" "vm86: live-switch arm-probe pending-surface=trampoline-bodies,real-lgdt,real-iret" "probe pending"
gf "$C" "vm86: live-switch arm-probe complete" "probe complete"

# No boot-path invocation of the armed execute or arm: shell.c /
# stage2.c / any other stage2 source may NOT call these symbols yet.
# OPENGEM-030 will add exactly one gated call site in shell.c.
for sym in vm86_live_switch_execute vm86_live_switch_arm; do
    hits=$(grep -RnE --include='*.c' --include='*.S' -w "$sym" stage2/ 2>/dev/null \
         | grep -v "^stage2/src/vm86.c:" \
         | grep -v "^stage2/include/vm86.h:" \
         | wc -l | tr -d ' ')
    if [ "$hits" = "0" ]; then
        pass
    else
        fail "forbidden boot-path reference to $sym ($hits hits)"
    fi
done

# No inline-asm lgdt/lidt/iret introduced.
if grep -nE '__asm__[^"]*"[^"]*(lgdt|lidt|iretd|iretq|iret|ljmp)' "$C" >/dev/null; then
    fail "inline asm lgdt/lidt/iret present in $C"
else
    pass
fi

# vm86_switch.S itself must still have only retq bodies (unchanged from 027).
S=stage2/src/vm86_switch.S
if grep -nE '^[ \t]*(lgdt|lidt|iret|iretd|iretq|ljmp|lretq)([ \t]|$)' "$S" >/dev/null; then
    fail "vm86_switch.S gained forbidden live instruction (should still be stub)"
else
    pass
fi

# Makefile target.
gf Makefile "test-vm86-arm-gate" "makefile target"

echo "[summary] $OK OK / $FAIL FAIL"
[ "$FAIL" = "0" ] && echo "[PASS] OPENGEM-029 vm86 arm-gate gate" || echo "[FAIL] OPENGEM-029 vm86 arm-gate gate"
exit "$FAIL"
