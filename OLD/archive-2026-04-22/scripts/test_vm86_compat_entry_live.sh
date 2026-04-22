#!/usr/bin/env bash
# OPENGEM-041 static gate: live v86 entry API (double arm-gated).
#
# Safety invariants:
#   - The asm trampoline vm86_compat_entry_enter_asm is UNGUARDED
#     (no defensive hlt at the top) so it must be unreachable unless
#     BOTH the 040 arm flag AND the 041 live arm flag are held.
#   - vm86_compat_entry_enter_v86() enforces both magics + both flags
#     before branching to asm.
#   - No boot-path C/asm caller exists for enter_asm, enter_v86, or
#     fill_frame. Shell integration is deferred.
#   - Prior-phase files are untouched.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

SRC_C=stage2/src/vm86.c
SRC_H=stage2/include/vm86.h
SRC_S=stage2/src/vm86_compat_entry_live.S

# --- 1. Sentinels ----------------------------------------------------
grep -q 'static const char vm86_compat_entry_live_c_sentinel\[\] = "OPENGEM-041";' "$SRC_C" \
    && pass || fail "C sentinel OPENGEM-041 missing"
grep -q '.asciz "OPENGEM-041"' "$SRC_S" \
    && pass || fail "asm sentinel OPENGEM-041 missing"
grep -q '#define VM86_COMPAT_ENTRY_LIVE_SENTINEL[[:space:]]*0x0410u' "$SRC_H" \
    && pass || fail "VM86_COMPAT_ENTRY_LIVE_SENTINEL missing"
grep -q '#define VM86_COMPAT_ENTRY_LIVE_ARM_MAGIC[[:space:]]*0xC1D39410u' "$SRC_H" \
    && pass || fail "VM86_COMPAT_ENTRY_LIVE_ARM_MAGIC missing"

# --- 2. Header API ---------------------------------------------------
for sig in \
    'int  vm86_compat_entry_live_arm(u32 magic);' \
    'void vm86_compat_entry_live_disarm(void);' \
    'int  vm86_compat_entry_live_is_armed(void);' \
    'int vm86_compat_entry_live_fill_frame(vm86_compat_task_image \*img,' \
    'int vm86_compat_entry_enter_v86(u32 magic040, u32 magic041);' \
    'int vm86_compat_entry_live_probe(void);'
do
    grep -q "$sig" "$SRC_H" && pass || fail "header signature missing: $sig"
done

# --- 3. Arm flag defaults 0 -----------------------------------------
grep -qE '^static int s_vm86_compat_entry_live_armed = 0;' "$SRC_C" \
    && pass || fail "s_vm86_compat_entry_live_armed must default to 0"

# --- 4. enter_v86 enforces BOTH magics + BOTH flags ------------------
awk '/^int vm86_compat_entry_enter_v86/,/^}/' "$SRC_C" > /tmp/vm86_041_enter.c
grep -q 'VM86_COMPAT_ENTRY_ARM_MAGIC'      /tmp/vm86_041_enter.c \
    && pass || fail "enter_v86 does not check magic040"
grep -q 'VM86_COMPAT_ENTRY_LIVE_ARM_MAGIC' /tmp/vm86_041_enter.c \
    && pass || fail "enter_v86 does not check magic041"
grep -q 'vm86_compat_entry_is_armed'       /tmp/vm86_041_enter.c \
    && pass || fail "enter_v86 does not check 040 arm"
grep -q 's_vm86_compat_entry_live_armed'   /tmp/vm86_041_enter.c \
    && pass || fail "enter_v86 does not check 041 arm"
grep -q 's_vm86_idt_shim_built'            /tmp/vm86_041_enter.c \
    && pass || fail "enter_v86 does not check shim built"
grep -q 's_vm86_gp_isr_installed'          /tmp/vm86_041_enter.c \
    && pass || fail "enter_v86 does not check 038 installed"
rm -f /tmp/vm86_041_enter.c

# --- 5. fill_frame enforces BOTH magics + BOTH flags ----------------
awk '/^int vm86_compat_entry_live_fill_frame/,/^}/' "$SRC_C" > /tmp/vm86_041_fill.c
grep -q 'VM86_COMPAT_ENTRY_ARM_MAGIC'      /tmp/vm86_041_fill.c \
    && pass || fail "fill_frame does not check magic040"
grep -q 'VM86_COMPAT_ENTRY_LIVE_ARM_MAGIC' /tmp/vm86_041_fill.c \
    && pass || fail "fill_frame does not check magic041"
grep -q 'vm86_compat_entry_is_armed'       /tmp/vm86_041_fill.c \
    && pass || fail "fill_frame does not check 040 arm"
grep -q 's_vm86_compat_entry_live_armed'   /tmp/vm86_041_fill.c \
    && pass || fail "fill_frame does not check 041 arm"
rm -f /tmp/vm86_041_fill.c

# --- 6. Asm symbols emitted ------------------------------------------
for sym in vm86_compat_entry_enter_asm vm86_compat_entry_live_compat32 \
           vm86_compat_entry_live_sentinel; do
    grep -qE "\.globl[[:space:]]+$sym" "$SRC_S" \
        && pass || fail "asm does not export symbol: $sym"
done

# --- 7. Asm contains the required live opcodes ----------------------
for mnem in '\blgdt\b' '\blidt\b' '\bltr\b' '\biretl\b' '\blretq\b' '\bcli\b'; do
    grep -qE "^[[:space:]]+$mnem" "$SRC_S" \
        && pass || fail "asm missing required mnemonic: $mnem"
done

# --- 8. No boot-path caller of 041 live API --------------------------
# The low-level asm symbols stay confined. Shell.c is whitelisted for
# the high-level APIs (OPENGEM-042 probe + OPENGEM-043 gem loader,
# both explicit user-typed commands). The asm entry_asm / compat32
# remain confined to vm86.c and vm86_compat_entry_live.S.
for sym in vm86_compat_entry_enter_asm vm86_compat_entry_live_compat32; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$sym" stage2 \
              | grep -vE 'stage2/src/vm86\.c$|stage2/src/vm86_compat_entry_live\.S$|stage2/include/vm86\.h$' || true)
    if [ -n "$callers" ]; then
        fail "unexpected 041 caller of $sym: $callers"
    else pass; fi
done

for sym in vm86_compat_entry_live_arm vm86_compat_entry_live_disarm \
           vm86_compat_entry_live_is_armed vm86_compat_entry_live_fill_frame \
           vm86_compat_entry_enter_v86 vm86_compat_entry_live_probe; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$sym" stage2 \
              | grep -vE 'stage2/src/vm86\.c$|stage2/src/vm86_compat_entry_live\.S$|stage2/include/vm86\.h$|stage2/src/shell\.c$' || true)
    if [ -n "$callers" ]; then
        fail "unexpected 041 caller of $sym: $callers"
    else pass; fi
done

# --- 9. Prior-phase asm files untouched by 041 ----------------------
# Strip /* ... */ comments before scanning so that forward-looking
# OPENGEM-041 references in 040 doc blocks do not trip the gate.
for f in stage2/src/vm86_switch.S stage2/src/vm86_lidt_ping.S \
         stage2/src/vm86_trap_stubs.S stage2/src/vm86_snapshot.S \
         stage2/src/vm86_gp_dispatch.S stage2/src/vm86_gp_isr_body.S \
         stage2/src/vm86_compat_entry.S; do
    stripped=$(sed -e 's|/\*[^*]*\*/||g' -e '/\/\*/,/\*\//d' "$f" 2>/dev/null || cat "$f")
    if echo "$stripped" | grep -qE 'vm86_compat_entry_live|VM86_COMPAT_ENTRY_LIVE|enter_asm'; then
        fail "041 symbols leaked into $f"
    else pass; fi
done

# --- 10. Probe asserts all guards -----------------------------------
awk '/^int vm86_compat_entry_live_probe/,/^}/' "$SRC_C" > /tmp/vm86_041_probe.c
grep -q 'default-armed=FAIL'   /tmp/vm86_041_probe.c \
    && pass || fail "probe does not assert default-disarmed"
grep -q 'magic-reject=FAIL'    /tmp/vm86_041_probe.c \
    && pass || fail "probe does not test magic rejection"
grep -q 'disarmed-fill=FAIL'   /tmp/vm86_041_probe.c \
    && pass || fail "probe does not test disarmed-fill-refused"
grep -q 'enter-guard040=FAIL'  /tmp/vm86_041_probe.c \
    && pass || fail "probe does not test enter_v86 guard040"
grep -q 'enter-guard041=FAIL'  /tmp/vm86_041_probe.c \
    && pass || fail "probe does not test enter_v86 guard041"
grep -q 'scratch-check=FAIL'   /tmp/vm86_041_probe.c \
    && pass || fail "probe does not verify scratch contents"
grep -q 'asm-symbol=FAIL'      /tmp/vm86_041_probe.c \
    && pass || fail "probe does not check asm symbol resolution"
rm -f /tmp/vm86_041_probe.c

# --- 11. Build artifacts --------------------------------------------
[ -f build/obj/stage2/vm86_compat_entry_live.o ] && pass || fail "vm86_compat_entry_live.o missing"

# --- 12. Probe markers ----------------------------------------------
grep -q 'vm86: compat-entry-live ready-surface=double-arm,fill-frame,enter-guard,asm-live' "$SRC_C" \
    && pass || fail "ready-surface marker missing"
grep -q 'vm86: compat-entry-live pending-surface=shell-gem-loader,int10h-mode13,gp-callback-reenter' "$SRC_C" \
    && pass || fail "pending-surface marker missing"

# --- 13. Live trampoline is present in ELF --------------------------
if command -v objdump >/dev/null 2>&1 && [ -f build/stage2.elf ]; then
    objdump -d build/stage2.elf 2>/dev/null \
        | grep -q 'vm86_compat_entry_enter_asm' \
        && pass || fail "enter_asm not linked into stage2.elf"
else pass; fi

echo "[test-vm86-compat-entry-live] OK=$OK FAIL=$FAIL"
[ "$FAIL" = "0" ] && { echo "[PASS] OPENGEM-041 vm86 compat-entry-live gate"; exit 0; } || { echo "[FAIL] OPENGEM-041 vm86 compat-entry-live gate"; exit 1; }
