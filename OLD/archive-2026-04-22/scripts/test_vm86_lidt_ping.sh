#!/usr/bin/env bash
# OPENGEM-033 static gate for the LIDT reversible trampoline.
#
# SAFETY CONTRACT: this phase introduces the LIDT opcode but the
# default boot path never reaches it. The gate enforces:
#   - arm-flag initial value is 0
#   - magic constant present and required for arming
#   - asm routine has exactly the expected opcode shape
#   - no IRETD / IRETQ / CS change / CR3 change introduced
#   - APIs unreachable from boot callers
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

# --- 1. Sentinels ----------------------------------------------------
grep -q '"OPENGEM-033"' stage2/src/vm86_lidt_ping.S \
    && pass || fail "OPENGEM-033 sentinel missing in vm86_lidt_ping.S"
grep -q 'static const char vm86_lidt_ping_c_sentinel\[\] = "OPENGEM-033";' stage2/src/vm86.c \
    && pass || fail "OPENGEM-033 C sentinel missing in vm86.c"
grep -q '#define VM86_LIDT_PING_SENTINEL[[:space:]]*0x0330u' stage2/include/vm86.h \
    && pass || fail "VM86_LIDT_PING_SENTINEL define missing"
grep -q '#define VM86_LIDT_PING_ARM_MAGIC[[:space:]]*0xC1036B33u' stage2/include/vm86.h \
    && pass || fail "VM86_LIDT_PING_ARM_MAGIC define missing"

# --- 2. Header API ---------------------------------------------------
for sig in \
    'int vm86_lidt_ping_asm(const vm86_idtr_image \*new_idtr,' \
    'int vm86_lidt_ping_arm(u32 magic);' \
    'void vm86_lidt_ping_disarm(void);' \
    'int  vm86_lidt_ping_is_armed(void);' \
    'int vm86_lidt_ping_execute(const vm86_idtr_image \*new_idtr,' \
    'int vm86_lidt_ping_probe(void);'
do
    grep -q "$sig" stage2/include/vm86.h && pass || fail "header API missing: $sig"
done

# --- 3. Asm shape: exactly one global label --------------------------
n_labels=$(grep -cE '^[[:space:]]*\.global vm86_lidt_ping_asm\b' stage2/src/vm86_lidt_ping.S)
[ "$n_labels" = "1" ] && pass || fail "expected exactly 1 .global label, got $n_labels"

# --- 4. Required opcodes present -------------------------------------
for op in 'pushfq' 'cli' 'sidt' 'lidt' 'popfq' 'retq'; do
    grep -qE "^[[:space:]]+$op\b" stage2/src/vm86_lidt_ping.S \
        && pass || fail "asm missing opcode: $op"
done

# --- 5. Exactly 2 LIDT opcodes (new, then saved) ---------------------
n_lidt=$(grep -cE '^[[:space:]]+lidt\b' stage2/src/vm86_lidt_ping.S)
[ "$n_lidt" = "2" ] && pass || fail "expected 2 LIDT in asm, got $n_lidt"

# --- 6. Forbidden opcodes in this asm file ---------------------------
for bad in '\biretd\b' '\biretq\b' '\blgdt\b' '\bltr\b' '\bmov[[:space:]]+.*%cr[0-4]\b'; do
    if grep -qE "^[[:space:]]+$bad" stage2/src/vm86_lidt_ping.S; then
        fail "forbidden opcode in ping asm: $bad"
    else pass; fi
done

# --- 7. Arm-flag initialization --------------------------------------
grep -qE '^static int s_vm86_lidt_ping_armed = 0;' stage2/src/vm86.c \
    && pass || fail "s_vm86_lidt_ping_armed must default to 0"

# --- 8. execute() checks arm-flag BEFORE calling asm -----------------
awk '/^int vm86_lidt_ping_execute/,/^}/' stage2/src/vm86.c \
    | grep -q 's_vm86_lidt_ping_armed' \
    && pass || fail "execute() does not consult arm-flag"

# --- 9. arm() only flips with correct magic --------------------------
awk '/^int vm86_lidt_ping_arm/,/^}/' stage2/src/vm86.c \
    | grep -q 'VM86_LIDT_PING_ARM_MAGIC' \
    && pass || fail "arm() does not check magic"

# --- 10. Boot-path isolation: new APIs unreferenced by boot code. ----
#     Allowed callers: stage2/src/vm86.c itself. Test scripts may
#     reference by symbol name, that's fine (grep filters stage2/).
for fn in vm86_lidt_ping_arm vm86_lidt_ping_disarm vm86_lidt_ping_is_armed \
          vm86_lidt_ping_execute vm86_lidt_ping_probe vm86_lidt_ping_asm; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 \
              | grep -vE 'stage2/src/vm86\.c|stage2/src/vm86_lidt_ping\.S' || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done

# --- 11. Switch/snapshot/trap-stubs untouched (code only, not comments) -
for f in stage2/src/vm86_switch.S stage2/src/vm86_snapshot.S stage2/src/vm86_trap_stubs.S; do
    # Strip /* … */ line comments and leading single-line comments
    # before scanning for forbidden tokens.
    if sed -e 's|/\*.*\*/||' -e '/^[[:space:]]*\*/d' -e '/^[[:space:]]*\/\//d' "$f" \
        | grep -qE '^[[:space:]]+lidt\b|OPENGEM-033|vm86_lidt_ping'; then
        fail "033 leaked into $f"
    else pass; fi
done

# --- 12. No LIDT anywhere in C code ----------------------------------
if grep -nE '\blidt\b' stage2/src/vm86.c | grep -vE 'serial_write|/\*|\*|//|pending-surface'; then
    fail "vm86.c introduces LIDT opcode (must remain in asm only)"
else pass; fi

# --- 13. Probe asserts default-disarmed ------------------------------
awk '/^int vm86_lidt_ping_probe/,/^}/' stage2/src/vm86.c \
    | grep -q 'Invariant #1: default must be disarmed' \
    && pass || fail "probe does not assert default-disarmed"

# --- 14. Probe emits required surface markers ------------------------
for mk in \
    '"vm86: lidt-ping sentinel=0x"' \
    '"vm86: lidt-ping arm-state=0 (disarmed)\\n"' \
    '"vm86: lidt-ping magic-reject=OK\\n"' \
    '"vm86: lidt-ping disarmed-noinvoke=OK\\n"' \
    '"vm86: lidt-ping null-guard=OK disarm=OK\\n"' \
    '"vm86: lidt-ping ready-surface=asm,arm-gate\\n"' \
    '"vm86: lidt-ping pending-surface=iretd,gp-handler\\n"' \
    '"vm86: lidt-ping probe complete\\n"'
do
    grep -q "$mk" stage2/src/vm86.c && pass || fail "probe marker missing: $mk"
done

# --- 15. Build artifact present --------------------------------------
[ -f build/stage2.elf ] && pass || fail "build/stage2.elf missing"

# --- 16. Makefile target registered ----------------------------------
grep -q '^test-vm86-lidt-ping:' Makefile \
    && pass || fail "test-vm86-lidt-ping Makefile target missing"

echo
echo "[summary] $OK OK / $FAIL FAIL"
if [ $FAIL -eq 0 ]; then
    echo "[PASS] OPENGEM-033 vm86 lidt-ping gate"
    exit 0
else
    exit 1
fi
