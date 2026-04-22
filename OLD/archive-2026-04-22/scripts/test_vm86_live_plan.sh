#!/usr/bin/env bash
# OPENGEM-028 — v8086 live-switch plan builder static gate.
#
# Observability-only. Verifies the plan type, its API, and the probe's
# cross-checks. Enforces no LGDT / LIDT / IRET in vm86.c.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf()   { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }

H=stage2/include/vm86.h
C=stage2/src/vm86.c

gf "$C" "OPENGEM-028" "impl sentinel in C"
gf "$H" "OPENGEM-028" "header sentinel"

# Plan sentinel + flags
gf "$H" "#define VM86_LIVE_PLAN_SENTINEL 0x0280u" "plan sentinel"
gf "$H" "#define VM86_LIVE_PLAN_F_BUFFERS_PRESENT" "flag buffers"
gf "$H" "#define VM86_LIVE_PLAN_F_GDTR_COMPUTED" "flag gdtr"
gf "$H" "#define VM86_LIVE_PLAN_F_IDTR_COMPUTED" "flag idtr"
gf "$H" "#define VM86_LIVE_PLAN_F_IRET_STAGED" "flag iret"
gf "$H" "#define VM86_LIVE_PLAN_F_VM_BIT_VERIFIED" "flag vm"
gf "$H" "#define VM86_LIVE_PLAN_F_READY" "flag ready"

# Struct
gf "$H" "typedef struct vm86_dtr {" "dtr type"
gf "$H" "typedef struct vm86_live_switch_plan {" "plan type"
gf "$H" "vm86_dtr   gdtr;" "plan gdtr field"
gf "$H" "vm86_dtr   idtr;" "plan idtr field"
gf "$H" "u16        cs_pe32_selector;" "cs selector field"
gf "$H" "u16        ss_pe32_selector;" "ss selector field"
gf "$H" "u16        tss_selector;" "tss selector field"

# API
gf "$H" "int vm86_live_switch_plan_build(vm86_live_switch_plan *plan," "build proto"
gf "$H" "u32 vm86_live_switch_plan_flags(const vm86_live_switch_plan *plan);" "flags proto"
gf "$H" "u16 vm86_live_switch_plan_gdtr_limit(const vm86_live_switch_plan *plan);" "gdtr limit proto"
gf "$H" "u32 vm86_live_switch_plan_gdtr_base (const vm86_live_switch_plan *plan);" "gdtr base proto"
gf "$H" "u16 vm86_live_switch_plan_idtr_limit(const vm86_live_switch_plan *plan);" "idtr limit proto"
gf "$H" "u32 vm86_live_switch_plan_idtr_base (const vm86_live_switch_plan *plan);" "idtr base proto"
gf "$H" "int vm86_live_switch_plan_probe(void);" "probe proto"

# Impl: the build function must compute GDTR/IDTR and force the ready bit
# only when VM_BIT_VERIFIED is set.
gf "$C" "plan->gdtr.limit = (u16)(VM86_GDT_BYTES - 1u);" "gdtr limit compute"
gf "$C" "plan->idtr.limit = (u16)(VM86_IDT_BYTES - 1u);" "idtr limit compute"
gf "$C" "plan->gdtr.base  = (u32)(u64)(unsigned long)gdt_buf;" "gdtr base compute"
gf "$C" "plan->idtr.base  = (u32)(u64)(unsigned long)idt_buf;" "idtr base compute"
gf "$C" "VM86_LIVE_PLAN_F_VM_BIT_VERIFIED" "vm verify check"
gf "$C" "VM86_LIVE_PLAN_F_READY" "ready bit set"

# Markers emitted by the probe
gf "$C" "vm86: live-plan begin sentinel=0x" "marker begin"
gf "$C" "vm86: live-plan gdtr limit=0x" "marker gdtr"
gf "$C" "vm86: live-plan idtr limit=0x" "marker idtr"
gf "$C" "vm86: live-plan tss base=0x" "marker tss"
gf "$C" "vm86: live-plan selectors cs=0x" "marker selectors"
gf "$C" "vm86: live-plan guest cs=0x" "marker guest"
gf "$C" "vm86: live-plan flags=0x" "marker flags"
gf "$C" "vm86: live-plan probe begin OPENGEM-028" "probe begin"
gf "$C" "vm86: live-plan probe ready-surface=buffers,gdtr,idtr,iret,vm-bit" "probe ready"
gf "$C" "vm86: live-plan probe pending-surface=lgdt-exec,lidt-exec,iret-exec,gp-decode" "probe pending"
gf "$C" "vm86: live-plan probe complete" "probe complete"

# Probe cross-check expectations (at least these are asserted).
gf "$C" "if (plan.sentinel != VM86_LIVE_PLAN_SENTINEL) ok = 0;" "probe sentinel check"
gf "$C" "if (plan.gdtr.limit != (u16)(VM86_GDT_BYTES - 1u)) ok = 0;" "probe gdtr limit"
gf "$C" "if (plan.idtr.limit != (u16)(VM86_IDT_BYTES - 1u)) ok = 0;" "probe idtr limit"
gf "$C" "if (!(plan.flags & VM86_LIVE_PLAN_F_READY)) ok = 0;" "probe ready check"
gf "$C" "if (bad_built) ok = 0;" "probe null-buf rejection"

# Null-guard contract.
gf "$C" "if (vm86_live_switch_plan_flags(0)      != 0u) ok = 0;" "null flags"
gf "$C" "if (vm86_live_switch_plan_gdtr_limit(0) != 0)  ok = 0;" "null gdtr limit"

# Invariant: no live LGDT / LIDT / IRET / far jump in vm86.c (observability-only).
forbid_in_c() {
    local mnem="$1" label="$2"
    if grep -nE "(^|[ \t\"(])$mnem(\"|[ \t(;])" "$C" | grep -vE "serial_write|^[^:]*:[ \t]*\*|//" >/dev/null; then
        fail "$label (forbidden mnemonic $mnem observed in $C)"
    else
        pass
    fi
}
# Use simpler matcher: look for inline asm sequences only.
if grep -nE '__asm__[^"]*"[^"]*(lgdt|lidt|iretd|iretq|iret|ljmp)' "$C" >/dev/null; then
    fail "inline asm lgdt/lidt/iret present in $C"
else
    pass
fi

# Makefile target present.
gf Makefile "test-vm86-live-plan" "makefile target"

echo "[summary] $OK OK / $FAIL FAIL"
[ "$FAIL" = "0" ] && echo "[PASS] OPENGEM-028 vm86 live-plan gate" || echo "[FAIL] OPENGEM-028 vm86 live-plan gate"
exit "$FAIL"
