#!/usr/bin/env bash
# OPENGEM-031 - CPU snapshot + identity-map verification for v8086 window.
#
# Static gate. Verifies the observability surface lands correctly,
# that the snapshot ABI struct layout matches the asm offsets, that
# the identity-verify walk is read-only, and that no CR3/LGDT/LIDT/
# IRET mutation is introduced on the boot path.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf()   { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }

H=stage2/include/vm86.h
C=stage2/src/vm86.c
S=stage2/src/vm86_snapshot.S

gf "$C" "OPENGEM-031" "impl sentinel"
gf "$H" "OPENGEM-031" "header sentinel"

# Sentinels.
gf "$H" "#define VM86_CPU_SNAPSHOT_SENTINEL  0x0310u" "snapshot sentinel"
gf "$H" "#define VM86_PE32_IDENTITY_SENTINEL 0x0311u" "identity sentinel"

# Struct + API.
gf "$H" "typedef struct vm86_cpu_snapshot {" "snapshot struct"
gf "$H" "int vm86_cpu_snapshot_capture(vm86_cpu_snapshot *out);" "capture proto"
gf "$H" "int vm86_pe32_identity_verify(u64 cr3_phys," "verify proto"
gf "$H" "int vm86_pe32_identity_probe(void);" "probe proto"

# Asm helper exists + single definition.
gf "$S" ".global vm86_snapshot_capture_asm" "asm global"
n=$(grep -c "^vm86_snapshot_capture_asm:" "$S" || true)
if [ "$n" = "1" ]; then pass; else fail "asm label count $n != 1"; fi
# Struct offsets in asm match header (spot checks on the bytes used).
gf "$S" "movl    \$0x00000310, 0(%rdi)" "asm sentinel write"
gf "$S" "mov     %cr3, %rax" "asm cr3 read"
gf "$S" "mov     %rax, 16(%rdi)" "asm cr3 store @16"
gf "$S" "mov     %rax, 32(%rdi)" "asm efer store @32"
gf "$S" "movw    %ax, 40(%rdi)" "asm gdtr.limit store @40"
gf "$S" "movq    %rax, 48(%rdi)" "asm gdtr.base store @48"
gf "$S" "movw    %ax, 56(%rdi)" "asm idtr.limit store @56"
gf "$S" "movq    %rax, 64(%rdi)" "asm idtr.base store @64"

# Asm is non-mutating (reads only, plus structured stack use).
# Forbid writes to control regs in this file.
if grep -nE 'mov[^,]*,[ \t]+%cr[0-9]' "$S" >/dev/null; then
    fail "snapshot asm writes to a control register"
else
    pass
fi
# Forbid lgdt/lidt/iret/etc in snapshot asm.
if grep -nE '^[ \t]*(lgdt|lidt|iret|iretd|iretq|ljmp)([ \t]|$)' "$S" >/dev/null; then
    fail "snapshot asm contains forbidden live instruction"
else
    pass
fi

# Probe-side invariants.
gf "$C" "vm86: pe32-ident probe begin OPENGEM-031" "probe begin marker"
gf "$C" "vm86: pe32-ident probe complete" "probe complete marker"
gf "$C" "vm86: pe32-ident ready-surface=snapshot,identity-window" "ready surface marker"
gf "$C" "vm86: pe32-ident pending-surface=cr3-mutation,lgdt,lidt,iretd" "pending surface marker"
gf "$C" "window=[0x0,0x100000) identity=OK" "window ok marker"
gf "$C" "efer.LME" "efer LME check"
gf "$C" "efer.LMA" "efer LMA check"
gf "$C" "cr0.PE" "cr0 PE check"
gf "$C" "cr0.PG" "cr0 PG check"
gf "$C" "cr4.PAE" "cr4 PAE check"
gf "$C" "ADDR_MASK = 0x000FFFFFFFFFF000ULL" "4K phys mask"

# No new boot-path call sites for the probe (observability only).
# OPENGEM-043 whitelists shell.c for explicit user-typed gem loader.
for sym in vm86_cpu_snapshot_capture vm86_pe32_identity_verify vm86_pe32_identity_probe; do
    hits=$(grep -RnE --include='*.c' -w "$sym" stage2/ 2>/dev/null \
         | grep -v "^stage2/src/vm86.c:" \
         | grep -v "^stage2/src/shell.c:" \
         | grep -v "^stage2/include/vm86.h:" \
         | wc -l | tr -d ' ')
    if [ "$hits" = "0" ]; then
        pass
    else
        fail "forbidden boot-path reference to $sym ($hits hits)"
    fi
done

# No CR3 write anywhere introduced in this phase.
# Exception: mode_switch_asm.S is the sanctioned OPENGEM-044 mode-switch
# trampoline that legitimately restores CR3 when re-entering IA-32e.
if grep -RnE --include='*.c' --include='*.S' --exclude='mode_switch_asm.S' 'mov[^,]*,[ \t]+%cr3' stage2/ >/dev/null; then
    fail "phase introduces a CR3 write"
else
    pass
fi

# Trampolines remain retq stubs.
SW=stage2/src/vm86_switch.S
if grep -nE '^[ \t]*(lgdt|lidt|iret|iretd|iretq|ljmp|lretq)([ \t]|$)' "$SW" >/dev/null; then
    fail "vm86_switch.S gained forbidden live instruction"
else
    pass
fi

# Makefile target.
gf Makefile "test-vm86-pe32-ident" "makefile target"

echo "[summary] $OK OK / $FAIL FAIL"
[ "$FAIL" = "0" ] && echo "[PASS] OPENGEM-031 vm86 pe32-ident gate" || echo "[FAIL] OPENGEM-031 vm86 pe32-ident gate"
exit "$FAIL"
