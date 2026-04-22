#!/usr/bin/env bash
# OPENGEM-039 static gate: PE32 compat-task scaffold.
#
# Safety invariants:
#   - TSS32 + GDT + IDTR images are staged data only;
#   - NO LIDT / LGDT / LTR / IRETD / IRETQ / far-jmp introduced;
#   - build() requires 038 install to have run (prereq chain);
#   - verify() rejects mutations on any selector/limit/TSS descriptor;
#   - the new APIs are only callable from vm86.c (no shell wiring yet).
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

SRC_C=stage2/src/vm86.c
SRC_H=stage2/include/vm86.h

# --- 1. Sentinels ----------------------------------------------------
grep -q 'static const char vm86_compat_task_c_sentinel\[\] = "OPENGEM-039";' "$SRC_C" \
    && pass || fail "C sentinel OPENGEM-039 missing"
grep -q '#define VM86_COMPAT_TASK_SENTINEL[[:space:]]*0x0390u' "$SRC_H" \
    && pass || fail "VM86_COMPAT_TASK_SENTINEL missing"
grep -q '#define VM86_COMPAT_TASK_ARM_MAGIC[[:space:]]*0xC1D39390u' "$SRC_H" \
    && pass || fail "VM86_COMPAT_TASK_ARM_MAGIC missing"

# --- 2. Header API ---------------------------------------------------
for sig in \
    'int  vm86_compat_task_arm(u32 magic);' \
    'void vm86_compat_task_disarm(void);' \
    'int  vm86_compat_task_is_armed(void);' \
    'int vm86_compat_task_build(vm86_compat_task_image \*out, u32 magic);' \
    'int vm86_compat_task_verify(const vm86_compat_task_image \*img);' \
    'int vm86_compat_task_probe(void);'
do
    grep -q "$sig" "$SRC_H" && pass || fail "header API missing: $sig"
done

# --- 3. Arm-flag default 0 ------------------------------------------
grep -qE '^static int s_vm86_compat_task_armed = 0;' "$SRC_C" \
    && pass || fail "s_vm86_compat_task_armed must default to 0"

# --- 4. Arm magic enforced ------------------------------------------
awk '/^int vm86_compat_task_arm/,/^}/' "$SRC_C" \
    | grep -q 'VM86_COMPAT_TASK_ARM_MAGIC' \
    && pass || fail "arm() does not check magic"

# --- 5. build() enforces magic + arm + 038 prereq -------------------
awk '/^int vm86_compat_task_build/,/^}/' "$SRC_C" > /tmp/vm86_039_build.c
grep -q 'VM86_COMPAT_TASK_ARM_MAGIC' /tmp/vm86_039_build.c \
    && pass || fail "build() does not check magic"
grep -q 's_vm86_compat_task_armed' /tmp/vm86_039_build.c \
    && pass || fail "build() does not consult arm flag"
grep -q 's_vm86_idt_shim_built' /tmp/vm86_039_build.c \
    && pass || fail "build() does not check shim built state"
grep -q 's_vm86_gp_isr_installed' /tmp/vm86_039_build.c \
    && pass || fail "build() does not check 038 installed state"
grep -q 'vm86_gdt_encode' /tmp/vm86_039_build.c \
    && pass || fail "build() does not call vm86_gdt_encode"
grep -q 'vm86_idt_shim_idtr_image' /tmp/vm86_039_build.c \
    && pass || fail "build() does not harvest shim IDTR image"
rm -f /tmp/vm86_039_build.c

# --- 6. verify() covers all critical fields -------------------------
awk '/^int vm86_compat_task_verify/,/^}/' "$SRC_C" > /tmp/vm86_039_verify.c
for fld in cs_sel ds_sel ss_sel tss_sel tss_limit gdtr_limit idtr_limit; do
    grep -q "$fld" /tmp/vm86_039_verify.c \
        && pass || fail "verify() does not check $fld"
done
grep -q 'VM86_GDT_V86_TSS' /tmp/vm86_039_verify.c \
    && pass || fail "verify() does not inspect TSS descriptor"
rm -f /tmp/vm86_039_verify.c

# --- 7. Forbidden opcodes/tokens in 039 block -----------------------
awk '/OPENGEM-039 - PE32 compat-task scaffold/,0' "$SRC_C" > /tmp/vm86_039_block.c
for bad in '\blidt\b' '\blgdt\b' '\bltr\b' '\biretd\b' '\biretq\b' \
           '\bljmp\b' '\blret\b' '\bsti\b' '\bcli\b'; do
    if grep -qE "^[[:space:]]+$bad" /tmp/vm86_039_block.c; then
        fail "039 block contains forbidden token: $bad"
    else pass; fi
done
rm -f /tmp/vm86_039_block.c

# --- 8. No external caller of 039 APIs ------------------------------
# OPENGEM-043 whitelists shell.c for explicit user-typed gem loader.
for fn in vm86_compat_task_arm vm86_compat_task_disarm \
          vm86_compat_task_is_armed vm86_compat_task_build \
          vm86_compat_task_verify vm86_compat_task_probe; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 \
              | grep -vE 'stage2/src/vm86\.c|stage2/src/shell\.c' || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done

# --- 9. Prior phase asm files untouched by 039 ----------------------
for f in stage2/src/vm86_switch.S stage2/src/vm86_lidt_ping.S \
         stage2/src/vm86_trap_stubs.S stage2/src/vm86_snapshot.S \
         stage2/src/vm86_gp_dispatch.S stage2/src/vm86_gp_isr_body.S; do
    if grep -qE 'vm86_compat_task|VM86_COMPAT_TASK' "$f"; then
        fail "039 symbols leaked into $f"
    else pass; fi
done

# --- 10. Probe asserts default-disarm + verify-mutation -------------
awk '/^int vm86_compat_task_probe/,/^}/' "$SRC_C" > /tmp/vm86_039_probe.c
grep -q 'default-armed=FAIL' /tmp/vm86_039_probe.c \
    && pass || fail "probe does not assert default-disarmed"
grep -q 'disarmed-build=FAIL' /tmp/vm86_039_probe.c \
    && pass || fail "probe does not assert disarmed-build-refused"
grep -q 'verify-mutation=FAIL' /tmp/vm86_039_probe.c \
    && pass || fail "probe does not test verify rejects mutation"
grep -q 'prereq-038-install=FAIL' /tmp/vm86_039_probe.c \
    && pass || fail "probe does not arm+install 038 as prereq"
rm -f /tmp/vm86_039_probe.c

# --- 11. Build artifact exists --------------------------------------
[ -f build/obj/stage2/vm86.o ] && pass || fail "vm86.o missing"

# --- 12. Probe markers ----------------------------------------------
grep -q 'vm86: compat-task ready-surface=tss32,gdtr-image,idtr-image,verify' "$SRC_C" \
    && pass || fail "ready-surface marker missing"
grep -q 'vm86: compat-task pending-surface=lidt-live,ltr-live,compat-entry,iretd-to-v86' "$SRC_C" \
    && pass || fail "pending-surface marker missing"

echo "[test-vm86-compat-task] OK=$OK FAIL=$FAIL"
[ "$FAIL" = "0" ] && { echo "[PASS] OPENGEM-039 vm86 compat-task gate"; exit 0; } || { echo "[FAIL] OPENGEM-039 vm86 compat-task gate"; exit 1; }
