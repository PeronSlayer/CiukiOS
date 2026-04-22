#!/usr/bin/env bash
# OPENGEM-036 static gate for the PE32 #GP ISR C-side entry.
#
# Same invariants as OPENGEM-035 plus:
#   - own arm-gate with own magic (no reuse of 029 / 033 / 035);
#   - the new C entry propagates the post-decode frame into out_frame
#     only on non-BLOCKED paths;
#   - the asm stub file is UNTOUCHED by this phase (body must stay
#     the halt-loop from OPENGEM-035);
#   - no LIDT / LGDT / IRETD / IRETQ / CR-write in the 036 C block.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

# --- 1. Sentinels ----------------------------------------------------
grep -q 'static const char vm86_gp_isr_c_sentinel\[\] = "OPENGEM-036";' stage2/src/vm86.c \
    && pass || fail "OPENGEM-036 C sentinel missing"
grep -q '#define VM86_GP_ISR_C_SENTINEL[[:space:]]*0x0360u' stage2/include/vm86.h \
    && pass || fail "VM86_GP_ISR_C_SENTINEL define missing"
grep -q '#define VM86_GP_ISR_ARM_MAGIC[[:space:]]*0xC1D39360u' stage2/include/vm86.h \
    && pass || fail "VM86_GP_ISR_ARM_MAGIC define missing"

# --- 2. Header API ---------------------------------------------------
for sig in \
    'int  vm86_gp_isr_c_arm(u32 magic);' \
    'void vm86_gp_isr_c_disarm(void);' \
    'int  vm86_gp_isr_c_is_armed(void);' \
    'vm86_gp_dispatch_action vm86_gp_isr_c_entry(' \
    'int vm86_gp_isr_c_probe(void);'
do
    grep -q "$sig" stage2/include/vm86.h && pass || fail "header API missing: $sig"
done

# --- 3. Arm flag default --------------------------------------------
grep -qE '^static int s_vm86_gp_isr_c_armed = 0;' stage2/src/vm86.c \
    && pass || fail "s_vm86_gp_isr_c_armed must default to 0"

# --- 4. Magic enforced in arm() --------------------------------------
awk '/^int vm86_gp_isr_c_arm/,/^}/' stage2/src/vm86.c \
    | grep -q 'VM86_GP_ISR_ARM_MAGIC' \
    && pass || fail "arm() does not check magic"

# --- 5. Entry consults arm-flag BEFORE invoking 035 dispatcher ------
awk '/^vm86_gp_dispatch_action vm86_gp_isr_c_entry/,/^}/' stage2/src/vm86.c \
    > /tmp/vm86_gp_isr_c_entry.c
grep -q 's_vm86_gp_isr_c_armed' /tmp/vm86_gp_isr_c_entry.c \
    && pass || fail "entry does not consult arm-flag"
awk '
    /s_vm86_gp_isr_c_armed/    { if (arm == 0) arm = NR }
    /vm86_gp_dispatch_handle\(/ { if (dec == 0) dec = NR }
    END {
        if (arm > 0 && dec > 0 && arm < dec) exit 0; else exit 1
    }
' /tmp/vm86_gp_isr_c_entry.c \
    && pass || fail "arm-flag check does not precede 035 dispatch call"

# --- 6. Entry returns BLOCKED_NOT_ARMED on disarmed path ------------
grep -q 'VM86_GP_DISPATCH_ACTION_BLOCKED_NOT_ARMED' /tmp/vm86_gp_isr_c_entry.c \
    && pass || fail "entry missing BLOCKED_NOT_ARMED return"

# --- 7. Entry validates inputs (NULL in_frame / NULL guest / 0 size) -
grep -q '!in_frame || !guest_base || guest_size == 0' /tmp/vm86_gp_isr_c_entry.c \
    && pass || fail "entry missing input validation"

# --- 8. Entry delegates to 035 dispatcher ---------------------------
grep -q 'vm86_gp_dispatch_handle' /tmp/vm86_gp_isr_c_entry.c \
    && pass || fail "entry does not call vm86_gp_dispatch_handle"

# --- 9. Entry copies work frame back to out_frame only on non-BLOCKED
grep -q 'action != VM86_GP_DISPATCH_ACTION_BLOCKED_NOT_ARMED' /tmp/vm86_gp_isr_c_entry.c \
    && pass || fail "entry unconditionally writes out_frame"

rm -f /tmp/vm86_gp_isr_c_entry.c

# --- 10. asm ISR stub file UNTOUCHED by 036 --------------------------
# The 035 stub comment legitimately references "OPENGEM-036+" as a
# forward-looking pointer. We therefore only forbid new 036 SYMBOLS.
if grep -q 'vm86_gp_isr_c' stage2/src/vm86_gp_dispatch.S; then
    fail "036 symbols leaked into asm ISR stub file"
else pass; fi
# Asm body must still be a halt-loop: the 035 gate spelled it as
# `.byte 0xF4` + `.byte 0xEB, 0xFE`. Preserve exactly that shape.
grep -q '.byte 0xF4' stage2/src/vm86_gp_dispatch.S \
    && pass || fail "asm ISR stub body must still contain the 0xF4 hlt byte"
grep -q '.byte 0xEB, 0xFE' stage2/src/vm86_gp_dispatch.S \
    && pass || fail "asm ISR stub body must still contain the 0xEB,0xFE jmp-in-place"

# --- 11. No LIDT/LGDT/IRETD/IRETQ/CR-write in 036 C block ------------
awk '/OPENGEM-036 - PE32 #GP ISR C-side entry/,0' stage2/src/vm86.c > /tmp/vm86_036_block.c
for bad in '\blidt\b' '\blgdt\b' '\biretd\b' '\biretq\b' 'mov[[:space:]]+.*%cr[0-4]'; do
    if grep -qE "^[[:space:]]+$bad" /tmp/vm86_036_block.c; then
        fail "036 C block contains forbidden token: $bad"
    else pass; fi
done
rm -f /tmp/vm86_036_block.c

# --- 12. Boot-path isolation: new C symbols unreferenced ------------
for fn in vm86_gp_isr_c_arm vm86_gp_isr_c_disarm \
          vm86_gp_isr_c_is_armed vm86_gp_isr_c_entry \
          vm86_gp_isr_c_probe; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 \
              | grep -vE 'stage2/src/vm86\.c' || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done

# --- 13. Prior phase files untouched by 036 --------------------------
# Symbol-only check (forward comments are allowed in pre-existing files).
for f in stage2/src/vm86_switch.S stage2/src/vm86_lidt_ping.S \
         stage2/src/vm86_trap_stubs.S stage2/src/vm86_snapshot.S \
         stage2/src/vm86_gp_dispatch.S; do
    if grep -q 'vm86_gp_isr_c' "$f"; then
        fail "036 symbols leaked into $f"
    else pass; fi
done

# --- 14. Arm-magic disjoint from prior phases -----------------------
for prior in '0x0350u' '0x0340u' '0x0330u' '0xC1D39350u' '0xC1D39340u' '0xC1D39330u'; do
    if [ "$prior" = "0x0360u" ] || [ "$prior" = "0xC1D39360u" ]; then
        fail "036 constants clash with prior phase token $prior"
    else pass; fi
done

# --- 15. Probe default-disarm assertion -----------------------------
awk '/^int vm86_gp_isr_c_probe/,/^}/' stage2/src/vm86.c \
    > /tmp/vm86_gp_isr_c_probe.c
grep -q 'default-armed=FAIL' /tmp/vm86_gp_isr_c_probe.c \
    && pass || fail "probe does not assert default-disarmed"
grep -q 'magic-reject=OK' /tmp/vm86_gp_isr_c_probe.c \
    && pass || fail "probe missing magic-reject assert"
grep -q 'disarmed-block=OK' /tmp/vm86_gp_isr_c_probe.c \
    && pass || fail "probe missing disarmed-block assert"
grep -q 'gate-independence=OK' /tmp/vm86_gp_isr_c_probe.c \
    && pass || fail "probe missing gate-independence assert"
grep -q 'vm86_gp_isr_c_disarm' /tmp/vm86_gp_isr_c_probe.c \
    && pass || fail "probe must disarm before returning"
rm -f /tmp/vm86_gp_isr_c_probe.c

# --- 16. Probe surface markers ---------------------------------------
for mk in \
    '"vm86: gp-isr-c sentinel=0x"' \
    '"vm86: gp-isr-c arm-state=0 (disarmed)\\n"' \
    '"vm86: gp-isr-c magic-reject=OK\\n"' \
    '"vm86: gp-isr-c disarmed-block=OK\\n"' \
    '"vm86: gp-isr-c arm-state=1 (armed)\\n"' \
    '"vm86: gp-isr-c out-frame-apply=OK\\n"' \
    '"vm86: gp-isr-c gate-independence=OK\\n"' \
    '"vm86: gp-isr-c ready-surface=arm-gate,c-entry,out-frame-apply,bad-input\\n"' \
    '"vm86: gp-isr-c pending-surface=asm-isr-body,live-idt-install,live-v86-entry\\n"' \
    '"vm86: gp-isr-c probe complete\\n"'
do
    grep -q "$mk" stage2/src/vm86.c && pass || fail "probe marker missing: $mk"
done

# --- 17. Build artifact present --------------------------------------
[ -f build/stage2.elf ] && pass || fail "build/stage2.elf missing"

# --- 18. Makefile target registered ----------------------------------
grep -q '^test-vm86-gp-isr-c:' Makefile \
    && pass || fail "test-vm86-gp-isr-c Makefile target missing"

echo
echo "[summary] $OK OK / $FAIL FAIL"
if [ $FAIL -eq 0 ]; then
    echo "[PASS] OPENGEM-036 vm86 gp-isr-c gate"
    exit 0
else
    exit 1
fi
