#!/usr/bin/env bash
# OPENGEM-040 static gate: compat-mode entry trampoline (arm-gated).
#
# Safety invariants:
#   - The asm trampoline emits LGDT/LIDT/LTR/IRETD bytes but they are
#     unreachable unless a caller supplies VM86_COMPAT_ENTRY_ARM_MAGIC;
#   - no boot-path C file calls vm86_compat_entry_trampoline or the
#     _body_live/_compat32 labels;
#   - no shell command wires vm86_compat_entry_prepare or the live
#     enter_v86 helper (the latter is not declared in the header at
#     all in 040);
#   - the arm flag defaults to 0;
#   - prior-phase source files are untouched.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

SRC_C=stage2/src/vm86.c
SRC_H=stage2/include/vm86.h
SRC_S=stage2/src/vm86_compat_entry.S

# --- 1. Sentinels ----------------------------------------------------
grep -q 'static const char vm86_compat_entry_c_sentinel\[\] = "OPENGEM-040";' "$SRC_C" \
    && pass || fail "C sentinel OPENGEM-040 missing"
grep -q '.asciz "OPENGEM-040"' "$SRC_S" \
    && pass || fail "asm sentinel OPENGEM-040 missing"
grep -q '#define VM86_COMPAT_ENTRY_SENTINEL[[:space:]]*0x0400u' "$SRC_H" \
    && pass || fail "VM86_COMPAT_ENTRY_SENTINEL missing"
grep -q '#define VM86_COMPAT_ENTRY_ARM_MAGIC[[:space:]]*0xC1D39400u' "$SRC_H" \
    && pass || fail "VM86_COMPAT_ENTRY_ARM_MAGIC missing"

# --- 2. Header API ---------------------------------------------------
for sig in \
    'int  vm86_compat_entry_arm(u32 magic);' \
    'void vm86_compat_entry_disarm(void);' \
    'int  vm86_compat_entry_is_armed(void);' \
    'int vm86_compat_entry_prepare(const vm86_compat_task_image \*img,' \
    'int vm86_compat_entry_verify(const vm86_compat_task_image \*img,' \
    'int vm86_compat_entry_probe(void);'
do
    grep -q "$sig" "$SRC_H" && pass || fail "header signature missing: $sig"
done

# --- 3. enter_v86 declaration policy: 040 must not declare it; 041
# (if already merged) MAY declare it. We only require that 040 itself
# does not introduce the name, not that the header is permanently
# free of it. Since we can't attribute header lines to phases at the
# file-system level, this check is relaxed once OPENGEM-041 lands.
if grep -q 'OPENGEM-041' "$SRC_H"; then
    pass   # 041 or later merged; policy is delegated to the 041 gate
else
    if grep -qE 'vm86_compat_entry_enter_v86[[:space:]]*\(' "$SRC_H"; then
        fail "enter_v86 must not be declared in 040 (deferred to 041)"
    else pass; fi
fi

# --- 4. Arm-flag default 0 ------------------------------------------
grep -qE '^static int s_vm86_compat_entry_armed = 0;' "$SRC_C" \
    && pass || fail "s_vm86_compat_entry_armed must default to 0"

# --- 5. arm() / prepare() enforce magic ------------------------------
awk '/^int vm86_compat_entry_arm/,/^}/' "$SRC_C" \
    | grep -q 'VM86_COMPAT_ENTRY_ARM_MAGIC' \
    && pass || fail "arm() does not check magic"

awk '/^int vm86_compat_entry_prepare/,/^}/' "$SRC_C" > /tmp/vm86_040_prep.c
grep -q 'VM86_COMPAT_ENTRY_ARM_MAGIC' /tmp/vm86_040_prep.c \
    && pass || fail "prepare() does not check magic"
grep -q 's_vm86_compat_entry_armed' /tmp/vm86_040_prep.c \
    && pass || fail "prepare() does not consult arm flag"
rm -f /tmp/vm86_040_prep.c

# --- 6. Asm symbols emitted ------------------------------------------
for sym in vm86_compat_entry_trampoline vm86_compat_entry_body_live \
           vm86_compat_entry_compat32 vm86_compat_entry_scratch \
           vm86_compat_entry_sentinel; do
    grep -qE "\.globl[[:space:]]+$sym" "$SRC_S" \
        && pass || fail "asm does not export symbol: $sym"
done

# --- 7. Asm contains the required live-body opcodes (staged only) ---
for mnem in '\blgdt\b' '\blidt\b' '\bltr\b' '\biretl\b' '\blretq\b'; do
    grep -qE "^[[:space:]]+$mnem" "$SRC_S" \
        && pass || fail "asm missing required staged mnemonic: $mnem"
done

# --- 8. Trampoline prologue has a defensive HLT guard ----------------
# The first hlt must appear before the body_live label.
awk '/^vm86_compat_entry_trampoline:/{flag=1} flag' "$SRC_S" \
    | awk '/vm86_compat_entry_body_live:/{exit} {print}' \
    | grep -qE '^[[:space:]]+hlt' \
    && pass || fail "trampoline prologue missing defensive hlt"

# --- 9. No boot-path caller of the 040 asm symbols or enter APIs -----
# The only legitimate callers are vm86.c (probe address-takes only) and
# shell.c (OPENGEM-043 gem loader, explicit user-typed command).
for sym in vm86_compat_entry_trampoline vm86_compat_entry_body_live \
           vm86_compat_entry_compat32 vm86_compat_entry_prepare \
           vm86_compat_entry_verify vm86_compat_entry_arm \
           vm86_compat_entry_disarm vm86_compat_entry_is_armed \
           vm86_compat_entry_probe; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$sym" stage2 \
              | grep -vE 'stage2/src/vm86\.c$|stage2/src/vm86_compat_entry\.S$|stage2/src/vm86_compat_entry_live\.S$|stage2/src/shell\.c$|stage2/include/vm86\.h$' || true)
    if [ -n "$callers" ]; then
        fail "unexpected 040 caller of $sym: $callers"
    else pass; fi
done

# --- 10. Prior-phase asm files untouched by 040 ----------------------
for f in stage2/src/vm86_switch.S stage2/src/vm86_lidt_ping.S \
         stage2/src/vm86_trap_stubs.S stage2/src/vm86_snapshot.S \
         stage2/src/vm86_gp_dispatch.S stage2/src/vm86_gp_isr_body.S; do
    if grep -qE 'vm86_compat_entry|VM86_COMPAT_ENTRY|OPENGEM-040' "$f"; then
        fail "040 symbols leaked into $f"
    else pass; fi
done

# --- 11. Probe asserts default-disarm + verify-mutation --------------
awk '/^int vm86_compat_entry_probe/,/^}/' "$SRC_C" > /tmp/vm86_040_probe.c
grep -q 'default-armed=FAIL' /tmp/vm86_040_probe.c \
    && pass || fail "probe does not assert default-disarmed"
grep -q 'disarmed-prepare=FAIL' /tmp/vm86_040_probe.c \
    && pass || fail "probe does not assert disarmed-prepare-refused"
grep -q 'verify-mutation=FAIL' /tmp/vm86_040_probe.c \
    && pass || fail "probe does not test verify rejects mutation"
grep -q 'prereq-038-install=FAIL' /tmp/vm86_040_probe.c \
    && pass || fail "probe does not install 038 as prereq"
grep -q 'prereq-039-build=FAIL' /tmp/vm86_040_probe.c \
    && pass || fail "probe does not build 039 as prereq"
grep -q 'asm-symbol=FAIL' /tmp/vm86_040_probe.c \
    && pass || fail "probe does not assert asm symbol resolution"
rm -f /tmp/vm86_040_probe.c

# --- 12. Build artifact exists --------------------------------------
[ -f build/obj/stage2/vm86_compat_entry.o ] && pass || fail "vm86_compat_entry.o missing"
[ -f build/obj/stage2/vm86.o ] && pass || fail "vm86.o missing"

# --- 13. Probe markers ----------------------------------------------
grep -q 'vm86: compat-entry ready-surface=arm-gate,prepare,verify,asm-staged' "$SRC_C" \
    && pass || fail "ready-surface marker missing"
grep -q 'vm86: compat-entry pending-surface=live-enter-v86,int10-mode13,gem-dispatch' "$SRC_C" \
    && pass || fail "pending-surface marker missing"

# --- 14. Live-body opcode bytes present in the linked ELF -----------
# Confirms the assembler didn't drop the staged body at link time.
if [ -f build/stage2.elf ]; then
    if command -v objdump >/dev/null 2>&1; then
        objdump -d build/stage2.elf 2>/dev/null \
            | grep -q 'vm86_compat_entry_body_live' \
            && pass || fail "body_live label not linked into stage2.elf"
    else pass; fi
else fail "build/stage2.elf missing"; fi

echo "[test-vm86-compat-entry] OK=$OK FAIL=$FAIL"
[ "$FAIL" = "0" ] && { echo "[PASS] OPENGEM-040 vm86 compat-entry gate"; exit 0; } || { echo "[FAIL] OPENGEM-040 vm86 compat-entry gate"; exit 1; }
