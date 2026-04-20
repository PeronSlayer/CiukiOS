#!/usr/bin/env bash
# OPENGEM-038 static gate: PE32 IDT install + live-arm shell gate.
#
# Safety invariants:
#   - install arm-flag default 0; magic-gated; idempotent;
#   - install writes exactly vector 0x0D of the 032 shim IDT and
#     caches the prior 8 bytes so uninstall is a pure byte-for-byte
#     restore;
#   - NO LIDT/IRETD/IRETQ opcodes in 038 C or asm files -- those
#     are OPENGEM-039+ pending surface;
#   - the shell `vm86-arm-live` command is the ONLY external caller
#     allowed for the install APIs; default boot never invokes it.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

SRC_C=stage2/src/vm86.c
SRC_H=stage2/include/vm86.h
SRC_SH=stage2/src/shell.c

# --- 1. Sentinels ----------------------------------------------------
grep -q 'static const char vm86_gp_isr_install_c_sentinel\[\] = "OPENGEM-038";' "$SRC_C" \
    && pass || fail "C sentinel OPENGEM-038 missing"
grep -q '#define VM86_GP_ISR_INSTALL_SENTINEL[[:space:]]*0x0380u' "$SRC_H" \
    && pass || fail "VM86_GP_ISR_INSTALL_SENTINEL missing"
grep -q '#define VM86_GP_ISR_INSTALL_ARM_MAGIC[[:space:]]*0xC1D39380u' "$SRC_H" \
    && pass || fail "VM86_GP_ISR_INSTALL_ARM_MAGIC missing"

# --- 2. Header API ---------------------------------------------------
for sig in \
    'int  vm86_gp_isr_install_arm(u32 magic);' \
    'void vm86_gp_isr_install_disarm(void);' \
    'int  vm86_gp_isr_install_is_armed(void);' \
    'int  vm86_gp_isr_install(u32 magic);' \
    'int  vm86_gp_isr_uninstall(void);' \
    'int  vm86_gp_isr_is_installed(void);' \
    'int vm86_gp_isr_install_probe(void);'
do
    grep -q "$sig" "$SRC_H" && pass || fail "header API missing: $sig"
done

# --- 3. Arm-flag defaults -------------------------------------------
grep -qE '^static int s_vm86_gp_isr_install_armed = 0;' "$SRC_C" \
    && pass || fail "install armed must default to 0"
grep -qE '^static int s_vm86_gp_isr_installed = 0;' "$SRC_C" \
    && pass || fail "installed must default to 0"

# --- 4. Magic enforcement in arm() + install() -----------------------
awk '/^int vm86_gp_isr_install_arm/,/^}/' "$SRC_C" \
    | grep -q 'VM86_GP_ISR_INSTALL_ARM_MAGIC' \
    && pass || fail "arm() does not check magic"

awk '/^int vm86_gp_isr_install\(u32 magic\)/,/^}/' "$SRC_C" > /tmp/vm86_038_install.c
grep -q 'VM86_GP_ISR_INSTALL_ARM_MAGIC' /tmp/vm86_038_install.c \
    && pass || fail "install() does not check magic"
grep -q 's_vm86_gp_isr_install_armed' /tmp/vm86_038_install.c \
    && pass || fail "install() does not consult arm flag"
grep -q 's_vm86_idt_shim_built' /tmp/vm86_038_install.c \
    && pass || fail "install() does not check shim built state"
grep -q 'VM86_IDT_VEC_GP' /tmp/vm86_038_install.c \
    && pass || fail "install() does not target VM86_IDT_VEC_GP"
grep -q 's_vm86_gp_isr_install_cache' /tmp/vm86_038_install.c \
    && pass || fail "install() does not cache prior slot"
grep -q 'vm86_idt_encode_gate' /tmp/vm86_038_install.c \
    && pass || fail "install() does not call vm86_idt_encode_gate"
grep -q 'vm86_gp_isr_real_entry' /tmp/vm86_038_install.c \
    && pass || fail "install() does not reference vm86_gp_isr_real_entry"
rm -f /tmp/vm86_038_install.c

# --- 5. uninstall() restores the cached bytes -----------------------
awk '/^int vm86_gp_isr_uninstall/,/^}/' "$SRC_C" > /tmp/vm86_038_uninst.c
grep -q 's_vm86_gp_isr_install_cache' /tmp/vm86_038_uninst.c \
    && pass || fail "uninstall() does not read cache"
grep -q 'VM86_IDT_VEC_GP' /tmp/vm86_038_uninst.c \
    && pass || fail "uninstall() does not target vector 0x0D"
rm -f /tmp/vm86_038_uninst.c

# --- 6. Forbidden opcodes/actions in 038 block ----------------------
awk '/OPENGEM-038 - PE32 IDT install/,0' "$SRC_C" > /tmp/vm86_038_block.c
for bad in '\blidt\b' '\blgdt\b' '\biretd\b' '\biretq\b' 'mov[[:space:]]+.*%cr[0-4]'; do
    if grep -qE "^[[:space:]]+$bad" /tmp/vm86_038_block.c; then
        fail "038 C block contains forbidden token: $bad"
    else pass; fi
done
rm -f /tmp/vm86_038_block.c

# --- 7. Shell wiring of vm86-arm-live / vm86-disarm-live ------------
grep -q 'str_eq(cmd, "vm86-arm-live")' "$SRC_SH" \
    && pass || fail "shell missing vm86-arm-live builtin"
grep -q 'str_eq(cmd, "vm86-disarm-live")' "$SRC_SH" \
    && pass || fail "shell missing vm86-disarm-live builtin"
grep -q 'vm86_gp_isr_install_arm(VM86_GP_ISR_INSTALL_ARM_MAGIC)' "$SRC_SH" \
    && pass || fail "shell does not arm via install_arm"
grep -q 'vm86_gp_isr_install(VM86_GP_ISR_INSTALL_ARM_MAGIC)' "$SRC_SH" \
    && pass || fail "shell does not call vm86_gp_isr_install"
grep -q 'vm86_gp_isr_uninstall()' "$SRC_SH" \
    && pass || fail "shell missing uninstall in disarm"
grep -q 'vm86_gp_isr_install_disarm()' "$SRC_SH" \
    && pass || fail "shell missing install_disarm in disarm"

# --- 8. Shell is the ONLY external caller of install APIs -----------
for fn in vm86_gp_isr_install_arm vm86_gp_isr_install \
          vm86_gp_isr_uninstall vm86_gp_isr_install_disarm \
          vm86_gp_isr_install_is_armed vm86_gp_isr_is_installed; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 \
              | grep -vE 'stage2/src/vm86\.c|stage2/src/shell\.c' || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done

# --- 9. Probe ensures round-trip --------------------------------------
awk '/^int vm86_gp_isr_install_probe/,/^}/' "$SRC_C" > /tmp/vm86_038_probe.c
grep -q 'default-armed=FAIL' /tmp/vm86_038_probe.c \
    && pass || fail "probe missing default-armed assertion"
grep -q 'disarmed-mutated=FAIL' /tmp/vm86_038_probe.c \
    && pass || fail "probe missing disarmed-mutation assertion"
grep -q 'roundtrip-byte=FAIL' /tmp/vm86_038_probe.c \
    && pass || fail "probe missing roundtrip byte assertion"
grep -q 'vector-offset=FAIL' /tmp/vm86_038_probe.c \
    && pass || fail "probe missing vector-offset assertion"
rm -f /tmp/vm86_038_probe.c

# --- 10. Prior phase asm files untouched by 038 ---------------------
# Symbol-only check: forward-looking "OPENGEM-038" comments in prior
# files are allowed; actual install symbols must not appear.
for f in stage2/src/vm86_switch.S stage2/src/vm86_lidt_ping.S \
         stage2/src/vm86_trap_stubs.S stage2/src/vm86_snapshot.S \
         stage2/src/vm86_gp_dispatch.S stage2/src/vm86_gp_isr_body.S; do
    if grep -q 'vm86_gp_isr_install' "$f"; then
        fail "038 symbols leaked into $f"
    else pass; fi
done

# --- 11. Build artifact exists --------------------------------------
[ -f build/obj/stage2/vm86.o ] && pass || fail "vm86.o missing"
[ -f build/obj/stage2/shell.o ] && pass || fail "shell.o missing"

# --- 12. Probe markers -----------------------------------------------
grep -q 'vm86: gp-isr-install ready-surface=arm-gate,install,uninstall,roundtrip' "$SRC_C" \
    && pass || fail "ready-surface marker missing"
grep -q 'vm86: gp-isr-install pending-surface=live-lidt,v86-entry,iretd-return' "$SRC_C" \
    && pass || fail "pending-surface marker missing"

echo "[test-vm86-gp-isr-install] OK=$OK FAIL=$FAIL"
[ "$FAIL" = "0" ]
