#!/usr/bin/env bash
# OPENGEM-035 static gate for the #GP dispatcher host path.
#
# SAFETY CONTRACT: this phase wires the C entry point that OPENGEM-036+
# will reach from the PE32 #GP ISR. The default boot path never invokes
# it. The gate enforces:
#   - arm-flag default 0;
#   - magic constant required to flip the gate;
#   - handle() returns BLOCKED_NOT_ARMED while disarmed, without
#     calling into the decoder;
#   - no LIDT / LGDT / IRETD / IRETQ / CR-write introduced;
#   - new APIs unreferenced by every file that is NOT vm86.c /
#     vm86_gp_dispatch.S (boot-path isolation);
#   - prior phase asm files (vm86_switch.S, vm86_lidt_ping.S,
#     vm86_trap_stubs.S, vm86_snapshot.S) remain untouched.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

# --- 1. Sentinels ----------------------------------------------------
grep -q '"OPENGEM-035"' stage2/src/vm86_gp_dispatch.S \
    && pass || fail "OPENGEM-035 sentinel missing in vm86_gp_dispatch.S"
grep -q 'static const char vm86_gp_dispatch_c_sentinel\[\] = "OPENGEM-035";' stage2/src/vm86.c \
    && pass || fail "OPENGEM-035 C sentinel missing in vm86.c"
grep -q '#define VM86_GP_DISPATCH_SENTINEL[[:space:]]*0x0350u' stage2/include/vm86.h \
    && pass || fail "VM86_GP_DISPATCH_SENTINEL define missing"
grep -q '#define VM86_GP_DISPATCH_ARM_MAGIC[[:space:]]*0xC1D39350u' stage2/include/vm86.h \
    && pass || fail "VM86_GP_DISPATCH_ARM_MAGIC define missing"

# --- 2. Header API ---------------------------------------------------
for sig in \
    'int  vm86_gp_dispatch_arm(u32 magic);' \
    'void vm86_gp_dispatch_disarm(void);' \
    'int  vm86_gp_dispatch_is_armed(void);' \
    'vm86_gp_dispatch_action vm86_gp_dispatch_handle(' \
    'int vm86_gp_dispatch_probe(void);'
do
    grep -q "$sig" stage2/include/vm86.h && pass || fail "header API missing: $sig"
done

# --- 3. Every action enum token is declared at header level ----------
for v in BLOCKED_NOT_ARMED IRETD HLT BAD_INPUT; do
    grep -q "VM86_GP_DISPATCH_ACTION_$v" stage2/include/vm86.h \
        && pass || fail "enum VM86_GP_DISPATCH_ACTION_$v missing"
done

# --- 4. Asm shape: exactly one .global for the ISR stub --------------
n_labels=$(grep -cE '^[[:space:]]*\.global vm86_gp_dispatch_isr_stub\b' stage2/src/vm86_gp_dispatch.S)
[ "$n_labels" = "1" ] && pass || fail "expected exactly 1 .global for isr stub, got $n_labels"
grep -qE '^[[:space:]]*\.global vm86_gp_dispatch_sentinel\b' stage2/src/vm86_gp_dispatch.S \
    && pass || fail "sentinel .global missing in vm86_gp_dispatch.S"

# --- 5. Asm forbidden opcodes ---------------------------------------
for bad in '\blidt\b' '\blgdt\b' '\biretd\b' '\biretq\b' '\bltr\b' 'mov[[:space:]]+.*%cr[0-4]\b'; do
    if grep -qE "^[[:space:]]+$bad" stage2/src/vm86_gp_dispatch.S; then
        fail "forbidden opcode in gp-dispatch asm: $bad"
    else pass; fi
done

# --- 6. Arm-flag initialization --------------------------------------
grep -qE '^static int s_vm86_gp_dispatch_armed = 0;' stage2/src/vm86.c \
    && pass || fail "s_vm86_gp_dispatch_armed must default to 0"

# --- 7. arm() only flips with correct magic --------------------------
awk '/^int vm86_gp_dispatch_arm/,/^}/' stage2/src/vm86.c \
    | grep -q 'VM86_GP_DISPATCH_ARM_MAGIC' \
    && pass || fail "arm() does not check magic"

# --- 8. handle() consults arm-flag BEFORE invoking the decoder -------
awk '/^vm86_gp_dispatch_action vm86_gp_dispatch_handle/,/^}/' stage2/src/vm86.c \
    > /tmp/vm86_gp_dispatch_handle.c
grep -q 's_vm86_gp_dispatch_armed' /tmp/vm86_gp_dispatch_handle.c \
    && pass || fail "handle() does not consult arm-flag"
# The arm-flag check must textually precede the first vm86_gp_decode call.
awk '
    /s_vm86_gp_dispatch_armed/ { if (arm == 0) arm = NR }
    /vm86_gp_decode\(/         { if (dec == 0) dec = NR }
    END {
        if (arm > 0 && dec > 0 && arm < dec) exit 0; else exit 1
    }
' /tmp/vm86_gp_dispatch_handle.c \
    && pass || fail "arm-flag check does not precede decoder call"

# --- 9. handle() returns BLOCKED_NOT_ARMED on disarmed path ---------
grep -q 'VM86_GP_DISPATCH_ACTION_BLOCKED_NOT_ARMED' /tmp/vm86_gp_dispatch_handle.c \
    && pass || fail "handle() missing BLOCKED_NOT_ARMED return"

# --- 10. IRETD slot application via vm86_iret_encode_frame ----------
grep -q 'vm86_iret_encode_frame' /tmp/vm86_gp_dispatch_handle.c \
    && pass || fail "handle() does not apply IRETD via vm86_iret_encode_frame"
rm -f /tmp/vm86_gp_dispatch_handle.c

# --- 11. No LIDT / LGDT / IRETD / IRETQ / CR-write in the C body ----
awk '/OPENGEM-035 - #GP dispatcher host path/,0' stage2/src/vm86.c > /tmp/vm86_035_block.c
for bad in '\blidt\b' '\blgdt\b' '\biretd\b' '\biretq\b' 'mov[[:space:]]+.*%cr[0-4]'; do
    if grep -qE "^[[:space:]]+$bad" /tmp/vm86_035_block.c; then
        fail "035 C block contains forbidden token: $bad"
    else pass; fi
done
rm -f /tmp/vm86_035_block.c

# --- 12. Boot-path isolation: new C symbols unreferenced ------------
#     Allowed callers: stage2/src/vm86.c itself (and the asm file for
#     its own sentinel symbol). Test scripts are matched by
#     scripts/... path and are scoped out.
for fn in vm86_gp_dispatch_arm vm86_gp_dispatch_disarm \
          vm86_gp_dispatch_is_armed vm86_gp_dispatch_handle \
          vm86_gp_dispatch_probe; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 \
              | grep -vE 'stage2/src/vm86\.c' || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done

# --- 13. Boot-path isolation: new asm symbols unreferenced ----------
for fn in vm86_gp_dispatch_isr_stub vm86_gp_dispatch_sentinel; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 \
              | grep -vE 'stage2/src/vm86_gp_dispatch\.S' || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done

# --- 14. Prior phase files untouched by 035 --------------------------
for f in stage2/src/vm86_switch.S stage2/src/vm86_lidt_ping.S \
         stage2/src/vm86_trap_stubs.S stage2/src/vm86_snapshot.S; do
    if sed -e 's|/\*.*\*/||' -e '/^[[:space:]]*\*/d' -e '/^[[:space:]]*\/\//d' "$f" \
        | grep -qE 'OPENGEM-035|vm86_gp_dispatch'; then
        fail "035 leaked into $f"
    else pass; fi
done

# --- 15. Probe default-disarm assertion -----------------------------
awk '/^int vm86_gp_dispatch_probe/,/^}/' stage2/src/vm86.c \
    | grep -q 'default-armed=FAIL' \
    && pass || fail "probe does not assert default-disarmed"

# --- 16. Probe registers INT 21h / INT3 / INTO handlers --------------
awk '/^int vm86_gp_dispatch_probe/,/^}/' stage2/src/vm86.c \
    > /tmp/vm86_gp_dispatch_probe.c
grep -q 'vm86_register_int_handler.*0x21' /tmp/vm86_gp_dispatch_probe.c \
    && pass || fail "probe does not register 0x21"
grep -q 'vm86_register_int_handler.*0x03' /tmp/vm86_gp_dispatch_probe.c \
    && pass || fail "probe does not register 0x03"
grep -q 'vm86_register_int_handler.*0x04' /tmp/vm86_gp_dispatch_probe.c \
    && pass || fail "probe does not register 0x04"
grep -q 's_gpd_hit_21 != 1' /tmp/vm86_gp_dispatch_probe.c \
    && pass || fail "probe missing INT 21h hit-assert"
grep -q 's_gpd_hit_3 != 1'  /tmp/vm86_gp_dispatch_probe.c \
    && pass || fail "probe missing INT3 hit-assert"
grep -q 's_gpd_hit_4 != 1'  /tmp/vm86_gp_dispatch_probe.c \
    && pass || fail "probe missing INTO hit-assert"
grep -q 'vm86_gp_dispatch_disarm()' /tmp/vm86_gp_dispatch_probe.c \
    && pass || fail "probe must disarm before returning"
rm -f /tmp/vm86_gp_dispatch_probe.c

# --- 17. Probe surface markers ---------------------------------------
for mk in \
    '"vm86: gp-dispatch sentinel=0x"' \
    '"vm86: gp-dispatch arm-state=0 (disarmed)\\n"' \
    '"vm86: gp-dispatch magic-reject=OK\\n"' \
    '"vm86: gp-dispatch disarmed-block=OK\\n"' \
    '"vm86: gp-dispatch arm-state=1 (armed)\\n"' \
    '"vm86: gp-dispatch iretd-frame-apply=OK\\n"' \
    '"vm86: gp-dispatch ready-surface=arm-gate,decode,iretd-frame-apply\\n"' \
    '"vm86: gp-dispatch pending-surface=pe32-isr-wire,live-v86-entry\\n"' \
    '"vm86: gp-dispatch probe complete\\n"'
do
    grep -q "$mk" stage2/src/vm86.c && pass || fail "probe marker missing: $mk"
done

# --- 18. Build artifact present --------------------------------------
[ -f build/stage2.elf ] && pass || fail "build/stage2.elf missing"

# --- 19. Makefile target registered ----------------------------------
grep -q '^test-vm86-gp-dispatch:' Makefile \
    && pass || fail "test-vm86-gp-dispatch Makefile target missing"

echo
echo "[summary] $OK OK / $FAIL FAIL"
if [ $FAIL -eq 0 ]; then
    echo "[PASS] OPENGEM-035 vm86 gp-dispatch gate"
    exit 0
else
    exit 1
fi
