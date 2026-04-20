#!/usr/bin/env bash
# OPENGEM-037 static gate for the PE32 #GP real ISR asm body.
#
# Safety contract enforced:
#   - the vm86_gp_dispatch.S halt-stub from OPENGEM-035 is UNMODIFIED;
#   - the new asm file introduces only its own symbols;
#   - the ISR is never referenced outside its own file (boot isolation);
#   - .code32 directive is present (required for the PE32 compat task);
#   - required prologue/capture/halt mnemonics are present;
#   - iretd / iretq / lidt / lgdt / ltr / CR-writes are still FORBIDDEN
#     in the 037 asm file (those are phase 038+);
#   - arm-flag default 0, own magic, own sentinel disjoint from prior.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

SRC_ASM=stage2/src/vm86_gp_isr_body.S
SRC_C=stage2/src/vm86.c
SRC_H=stage2/include/vm86.h

# --- 1. Sentinels ----------------------------------------------------
grep -q '"OPENGEM-037"' "$SRC_ASM" && pass || fail "asm sentinel OPENGEM-037 missing"
grep -q 'static const char vm86_gp_isr_real_c_sentinel\[\] = "OPENGEM-037";' "$SRC_C" \
    && pass || fail "C sentinel OPENGEM-037 missing"
grep -q '#define VM86_GP_ISR_REAL_SENTINEL[[:space:]]*0x0370u' "$SRC_H" \
    && pass || fail "VM86_GP_ISR_REAL_SENTINEL missing"
grep -q '#define VM86_GP_ISR_REAL_ARM_MAGIC[[:space:]]*0xC1D39370u' "$SRC_H" \
    && pass || fail "VM86_GP_ISR_REAL_ARM_MAGIC missing"

# --- 2. Asm shape ----------------------------------------------------
grep -qE '^[[:space:]]*\.code32\b' "$SRC_ASM" \
    && pass || fail ".code32 directive missing"
grep -qE '^[[:space:]]*\.global[[:space:]]+vm86_gp_isr_real_entry\b' "$SRC_ASM" \
    && pass || fail "asm .global vm86_gp_isr_real_entry missing"
grep -qE '^[[:space:]]*\.global[[:space:]]+vm86_gp_isr_real_sentinel\b' "$SRC_ASM" \
    && pass || fail "asm .global vm86_gp_isr_real_sentinel missing"
grep -qE '^[[:space:]]*\.global[[:space:]]+vm86_gp_isr_capture_area\b' "$SRC_ASM" \
    && pass || fail "asm .global vm86_gp_isr_capture_area missing"
grep -qE '^[[:space:]]*\.global[[:space:]]+vm86_gp_isr_capture_flag\b' "$SRC_ASM" \
    && pass || fail "asm .global vm86_gp_isr_capture_flag missing"
grep -qE '^[[:space:]]*\.global[[:space:]]+vm86_gp_isr_capture_seq\b' "$SRC_ASM" \
    && pass || fail "asm .global vm86_gp_isr_capture_seq missing"

# --- 3. Required prologue mnemonics ---------------------------------
grep -qE '^[[:space:]]+pushl[[:space:]]+%gs\b' "$SRC_ASM" \
    && pass || fail "asm body missing pushl %gs"
grep -qE '^[[:space:]]+pushl[[:space:]]+%fs\b' "$SRC_ASM" \
    && pass || fail "asm body missing pushl %fs"
grep -qE '^[[:space:]]+pushl[[:space:]]+%es\b' "$SRC_ASM" \
    && pass || fail "asm body missing pushl %es"
grep -qE '^[[:space:]]+pushl[[:space:]]+%ds\b' "$SRC_ASM" \
    && pass || fail "asm body missing pushl %ds"
grep -qE '^[[:space:]]+pushal\b' "$SRC_ASM" \
    && pass || fail "asm body missing pushal"

# --- 4. Capture writes to capture_area ------------------------------
grep -qE '\$vm86_gp_isr_capture_area' "$SRC_ASM" \
    && pass || fail "asm does not load capture_area address"
grep -qE 'movb[[:space:]]+\$1,[[:space:]]+vm86_gp_isr_capture_flag' "$SRC_ASM" \
    && pass || fail "asm does not set capture_flag"
grep -qE 'incl[[:space:]]+%eax' "$SRC_ASM" \
    && pass || fail "asm does not bump seq"

# --- 5. Halt terminal, no iretd yet ---------------------------------
grep -qE '^[[:space:]]+\.byte[[:space:]]+0xF4\b' "$SRC_ASM" \
    && pass || fail "asm halt byte 0xF4 missing"
grep -qE '^[[:space:]]+\.byte[[:space:]]+0xEB,[[:space:]]*0xFE\b' "$SRC_ASM" \
    && pass || fail "asm jmp-in-place 0xEB 0xFE missing"

# --- 6. Forbidden opcodes in 037 asm (still observability-only) -----
for bad in '\blidt\b' '\blgdt\b' '\biretd\b' '\biretq\b' '\bltr\b' 'mov[[:space:]]+.*%cr[0-4]\b'; do
    if grep -qE "^[[:space:]]+$bad" "$SRC_ASM"; then
        fail "forbidden opcode in 037 asm: $bad"
    else pass; fi
done

# --- 7. Header API ---------------------------------------------------
for sig in \
    'int  vm86_gp_isr_real_arm(u32 magic);' \
    'void vm86_gp_isr_real_disarm(void);' \
    'int  vm86_gp_isr_real_is_armed(void);' \
    'int vm86_gp_isr_real_probe(void);' \
    'extern u8  vm86_gp_isr_capture_area\[64\];' \
    'extern u8  vm86_gp_isr_capture_flag;' \
    'extern u32 vm86_gp_isr_capture_seq;'
do
    grep -q "$sig" "$SRC_H" && pass || fail "header API missing: $sig"
done

# --- 8. Arm-flag default 0 ------------------------------------------
grep -qE '^static int s_vm86_gp_isr_real_armed = 0;' "$SRC_C" \
    && pass || fail "s_vm86_gp_isr_real_armed must default to 0"

# --- 9. arm() only flips with correct magic -------------------------
awk '/^int vm86_gp_isr_real_arm/,/^}/' "$SRC_C" \
    | grep -q 'VM86_GP_ISR_REAL_ARM_MAGIC' \
    && pass || fail "arm() does not check magic"

# --- 10. 035 halt-stub file UNMODIFIED -------------------------------
# It must still contain the halt-loop bytes and the OPENGEM-035 sentinel
# and nothing referencing 037 symbols.
grep -q '"OPENGEM-035"' stage2/src/vm86_gp_dispatch.S \
    && pass || fail "035 sentinel disappeared from dispatch.S"
grep -qE '^[[:space:]]+\.byte[[:space:]]+0xF4\b' stage2/src/vm86_gp_dispatch.S \
    && pass || fail "035 halt byte disappeared"
if grep -qE 'vm86_gp_isr_real_entry|vm86_gp_isr_capture_' stage2/src/vm86_gp_dispatch.S; then
    fail "037 symbols leaked into 035 asm file"
else pass; fi

# --- 11. Boot-path isolation: 037 symbols unreferenced outside owners -
for fn in vm86_gp_isr_real_entry vm86_gp_isr_capture_area \
          vm86_gp_isr_capture_flag vm86_gp_isr_capture_seq \
          vm86_gp_isr_real_sentinel; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 \
              | grep -vE 'stage2/src/vm86_gp_isr_body\.S|stage2/src/vm86\.c|stage2/include/vm86\.h' \
              || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done
for fn in vm86_gp_isr_real_arm vm86_gp_isr_real_disarm \
          vm86_gp_isr_real_is_armed vm86_gp_isr_real_probe; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 \
              | grep -vE 'stage2/src/vm86\.c' || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done

# --- 12. Prior phase asm files untouched by 037 ---------------------
for f in stage2/src/vm86_switch.S stage2/src/vm86_lidt_ping.S \
         stage2/src/vm86_trap_stubs.S stage2/src/vm86_snapshot.S \
         stage2/src/vm86_gp_dispatch.S; do
    if grep -qE 'OPENGEM-037|vm86_gp_isr_real|vm86_gp_isr_capture' "$f"; then
        fail "037 leaked into $f"
    else pass; fi
done

# --- 13. Probe assertion: default-disarmed, asm sentinel match ------
awk '/^int vm86_gp_isr_real_probe/,/^}/' "$SRC_C" > /tmp/vm86_037_probe.c
grep -q 'default-armed=FAIL' /tmp/vm86_037_probe.c \
    && pass || fail "probe does not assert default-disarmed"
grep -q 'asm-sentinel=FAIL' /tmp/vm86_037_probe.c \
    && pass || fail "probe does not validate asm sentinel"
grep -q 'capture-flag-nonzero=FAIL' /tmp/vm86_037_probe.c \
    && pass || fail "probe does not validate capture flag"
grep -q 'capture-seq-nonzero=FAIL' /tmp/vm86_037_probe.c \
    && pass || fail "probe does not validate capture seq"
grep -q 'independence=FAIL' /tmp/vm86_037_probe.c \
    && pass || fail "probe does not assert gate independence"
rm -f /tmp/vm86_037_probe.c

# --- 14. Build artifact exists --------------------------------------
[ -f build/obj/stage2/vm86_gp_isr_body.o ] \
    && pass || fail "build/obj/stage2/vm86_gp_isr_body.o missing (run make)"

# --- 15. Probe markers -----------------------------------------------
grep -q 'vm86: gp-isr-real ready-surface=arm-gate,asm-capture-body,halt-terminal' "$SRC_C" \
    && pass || fail "ready-surface marker missing"
grep -q 'vm86: gp-isr-real pending-surface=live-idt-install,iretd-return,v86-entry' "$SRC_C" \
    && pass || fail "pending-surface marker missing"

echo "[test-vm86-gp-isr-real] OK=$OK FAIL=$FAIL"
[ "$FAIL" = "0" ]
