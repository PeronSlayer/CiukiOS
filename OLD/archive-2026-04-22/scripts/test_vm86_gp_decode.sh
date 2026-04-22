#!/usr/bin/env bash
# OPENGEM-034 static gate for the synthetic #GP opcode decoder.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

# --- 1. Sentinels ----------------------------------------------------
grep -q 'static const char vm86_gp_decode_sentinel_id\[\] = "OPENGEM-034";' stage2/src/vm86.c \
    && pass || fail "OPENGEM-034 C sentinel missing"
grep -q '#define VM86_GP_DECODE_SENTINEL[[:space:]]*0x0340u' stage2/include/vm86.h \
    && pass || fail "VM86_GP_DECODE_SENTINEL define missing"

# --- 2. Header API ---------------------------------------------------
grep -q 'vm86_gp_decode_result vm86_gp_decode(const u8' stage2/include/vm86.h \
    && pass || fail "vm86_gp_decode signature missing"
grep -q 'int vm86_gp_decode_probe(void);' stage2/include/vm86.h \
    && pass || fail "vm86_gp_decode_probe signature missing"

# --- 3. Every enum value has a definition at header level ------------
for v in NONE INT INTO INT3 IRET PUSHF POPF IN_IMM OUT_IMM IN_DX OUT_DX CLI STI HLT UNHANDLED NULL_ARG OOB; do
    grep -q "VM86_GP_RESULT_$v" stage2/include/vm86.h \
        && pass || fail "enum VM86_GP_RESULT_$v missing"
done

# --- 4. Switch coverage: every expected opcode is routed -------------
for op in '0xCD:' '0xCC:' '0xCE:' '0xCF:' '0x9C:' '0x9D:' '0xE4:' '0xE5:' '0xE6:' '0xE7:' '0xEC:' '0xED:' '0xEE:' '0xEF:' '0xFA:' '0xFB:' '0xF4:'; do
    grep -qE "case[[:space:]]+${op}" stage2/src/vm86.c \
        && pass || fail "missing opcode case: $op"
done

# --- 5. Bounds checking: guest_size guard present --------------------
awk '/^vm86_gp_decode_result vm86_gp_decode/,/^}/' stage2/src/vm86.c \
    | grep -q 'lin >= guest_size' \
    && pass || fail "decoder missing guest_size bounds check"

# --- 6. NULL-arg guard at top of decode ------------------------------
awk '/^vm86_gp_decode_result vm86_gp_decode/,/^}/' stage2/src/vm86.c \
    | grep -q 'VM86_GP_RESULT_NULL_ARG' \
    && pass || fail "decoder missing NULL-arg path"

# --- 7. Decoder does NOT introduce LIDT / IRETD / LGDT / CR writes ---
awk '/^vm86_gp_decode_result vm86_gp_decode/,/^}/' stage2/src/vm86.c > /tmp/vm86_gp_decode_body.c
for bad in '\blidt\b' '\blgdt\b' '\biretd\b' '\biretq\b' 'mov[[:space:]]+.*%cr'; do
    if grep -qE "$bad" /tmp/vm86_gp_decode_body.c; then
        fail "decoder body contains forbidden token: $bad"
    else pass; fi
done
rm -f /tmp/vm86_gp_decode_body.c

# --- 8. Probe registers INT 21h / INT3 / INTO handlers ---------------
awk '/^int vm86_gp_decode_probe/,/^}/' stage2/src/vm86.c > /tmp/vm86_gp_probe_body.c
grep -q 'vm86_register_int_handler.*0x21' /tmp/vm86_gp_probe_body.c && pass || fail "probe does not register 0x21"
grep -q 'vm86_register_int_handler.*0x03' /tmp/vm86_gp_probe_body.c && pass || fail "probe does not register 0x03"
grep -q 'vm86_register_int_handler.*0x04' /tmp/vm86_gp_probe_body.c && pass || fail "probe does not register 0x04"

# --- 9. Probe asserts EIP advancement + hit counters -----------------
grep -q 's_gp_hit_21 != 1' /tmp/vm86_gp_probe_body.c && pass || fail "probe missing INT 21h hit-assert"
grep -q 's_gp_hit_3 != 1'  /tmp/vm86_gp_probe_body.c && pass || fail "probe missing INT3 hit-assert"
grep -q 's_gp_hit_4 != 1'  /tmp/vm86_gp_probe_body.c && pass || fail "probe missing INTO hit-assert"
rm -f /tmp/vm86_gp_probe_body.c

# --- 10. Boot-path isolation: decoder + probe unreferenced by boot ---
for fn in vm86_gp_decode vm86_gp_decode_probe; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 \
              | grep -v 'stage2/src/vm86.c' || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done

# --- 11. No CPU mutation ops in this phase --------------------------
#     Restrict scan to the new 034 block: find the sentinel line and
#     from there until EOF, no LIDT/LGDT/IRETD/IRETQ/CR-write opcodes.
if awk '/vm86_gp_decode_sentinel_id/{flag=1} flag' stage2/src/vm86.c \
    | grep -nE '^[[:space:]]+lidt\b|^[[:space:]]+lgdt\b|^[[:space:]]+iretd\b|^[[:space:]]+iretq\b|mov[[:space:]]+.*%cr[0-4]' ; then
    fail "034 block introduces CPU mutation opcode"
else pass; fi

# --- 12. Untouched phase files --------------------------------------
for f in stage2/src/vm86_lidt_ping.S stage2/src/vm86_trap_stubs.S stage2/src/vm86_switch.S stage2/src/vm86_snapshot.S; do
    if sed -e 's|/\*.*\*/||' -e '/^[[:space:]]*\*/d' "$f" | grep -qE 'OPENGEM-034|vm86_gp_decode'; then
        fail "034 leaked into $f"
    else pass; fi
done

# --- 13. Build artifact present --------------------------------------
[ -f build/stage2.elf ] && pass || fail "build/stage2.elf missing"

# --- 14. Makefile target registered ----------------------------------
grep -q '^test-vm86-gp-decode:' Makefile && pass || fail "test-vm86-gp-decode target missing"

# --- 15. Probe surface markers --------------------------------------
for mk in \
    '"vm86: gp-decode sentinel=0x"' \
    '"vm86: gp-decode hits int21=0x"' \
    '"vm86: gp-decode ready-surface=int-n,int3,into,iret,pushf,popf,in,out,cli,sti,hlt\\n"' \
    '"vm86: gp-decode pending-surface=handler-frame-apply,guest-stack-iret\\n"' \
    '"vm86: gp-decode probe complete\\n"'
do
    grep -q "$mk" stage2/src/vm86.c && pass || fail "probe marker missing: $mk"
done

echo
echo "[summary] $OK OK / $FAIL FAIL"
if [ $FAIL -eq 0 ]; then
    echo "[PASS] OPENGEM-034 vm86 gp-decode gate"
    exit 0
else
    exit 1
fi
