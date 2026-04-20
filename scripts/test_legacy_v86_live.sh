#!/usr/bin/env bash
# OPENGEM-044 Stage 3C static gate: v86 live entry scaffold.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

H=stage2/include/legacy_v86_live.h
C=stage2/src/legacy_v86_live.c
S=stage2/src/legacy_v86_v86_body.S

test -f "$H" && pass || fail "header $H missing"
test -f "$C" && pass || fail "source $C missing"
test -f "$S" && pass || fail "asm $S missing"

# ------------------------------------------------------------------ header
grep -q '#define LEGACY_V86_LIVE_ARM_MAGIC[[:space:]]*0xC1D39470u' "$H" \
    && pass || fail "LEGACY_V86_LIVE_ARM_MAGIC 0xC1D39470u missing"
grep -q '#define LEGACY_V86_LIVE_SENTINEL[[:space:]]*0x0470u' "$H" \
    && pass || fail "LEGACY_V86_LIVE_SENTINEL 0x0470u missing"

while IFS= read -r sig; do
    grep -qF "$sig" "$H" && pass || fail "header signature missing: $sig"
done <<'EOF'
int  legacy_v86_live_arm(uint32_t magic);
void legacy_v86_live_disarm(void);
int  legacy_v86_live_is_armed(void);
int legacy_v86_live_enter(void);
int legacy_v86_live_probe(void);
EOF

grep -q '#define LEGACY_V86_LIVE_OK' "$H" \
    && pass || fail "status code LEGACY_V86_LIVE_OK missing"
grep -q '#define LEGACY_V86_LIVE_ERR_NOT_ARMED' "$H" \
    && pass || fail "status code LEGACY_V86_LIVE_ERR_NOT_ARMED missing"
grep -q '#define LEGACY_V86_LIVE_ERR_MODE_SWITCH_OFF' "$H" \
    && pass || fail "status code LEGACY_V86_LIVE_ERR_MODE_SWITCH_OFF missing"
grep -q '#define LEGACY_V86_LIVE_ERR_BAD_INPUT' "$H" \
    && pass || fail "status code LEGACY_V86_LIVE_ERR_BAD_INPUT missing"

# ------------------------------------------------------------------ C shape
grep -q 'opengem_044_stage3c_sentinel\[\] = "OPENGEM-044-STAGE3C"' "$C" \
    && pass || fail "C sentinel OPENGEM-044-STAGE3C missing"
grep -qE '^static int s_legacy_v86_live_armed = 0;' "$C" \
    && pass || fail "s_legacy_v86_live_armed must default to 0"
grep -q 'extern void legacy_v86_v86_body(void \*user);' "$C" \
    && pass || fail "C must declare extern legacy_v86_v86_body"
grep -q 'extern const char legacy_v86_v86_body_sentinel\[\];' "$C" \
    && pass || fail "C must declare extern sentinel"

awk '/^int legacy_v86_live_arm\(/,/^}/' "$C" > /tmp/v86live_arm.c
grep -q 'LEGACY_V86_LIVE_ARM_MAGIC' /tmp/v86live_arm.c \
    && pass || fail "arm does not check LEGACY_V86_LIVE_ARM_MAGIC"
grep -q 's_legacy_v86_live_armed = 1' /tmp/v86live_arm.c \
    && pass || fail "arm does not set armed flag"
rm -f /tmp/v86live_arm.c

awk '/^int legacy_v86_live_enter\(/,/^}/' "$C" > /tmp/v86live_enter.c
head -n 8 /tmp/v86live_enter.c | grep -q 's_legacy_v86_live_armed' \
    && pass || fail "enter must check live-armed flag FIRST"
grep -q 'LEGACY_V86_LIVE_ERR_NOT_ARMED' /tmp/v86live_enter.c \
    && pass || fail "enter must return NOT_ARMED when disarmed"
grep -q 'mode_switch_is_armed()' /tmp/v86live_enter.c \
    && pass || fail "enter must check Task A arm"
grep -q 'LEGACY_V86_LIVE_ERR_MODE_SWITCH_OFF' /tmp/v86live_enter.c \
    && pass || fail "enter must surface mode-switch-off"
grep -q 'mode_switch_run_legacy_pm(legacy_v86_v86_body' /tmp/v86live_enter.c \
    && pass || fail "enter must invoke mode_switch_run_legacy_pm with legacy_v86_v86_body"
rm -f /tmp/v86live_enter.c

# C file must NOT touch any privileged control register / descriptor reg / MSR.
if grep -qE '\bmov[[:space:]]+%?cr[0-4]' "$C"; then
    fail "C file must not contain CR register writes"
else pass; fi
if grep -qE 'wrmsr|rdmsr' "$C"; then
    fail "C file must not contain MSR access"
else pass; fi
if grep -qE '\blgdt\b|\blidt\b|\bltr\b|\blldt\b|\biretl\b|\biretq\b' "$C"; then
    fail "C file must not contain descriptor-register or iret ops"
else pass; fi

# ------------------------------------------------------------------ ASM shape
grep -q '\.asciz "V86-BODY-3C"' "$S" \
    && pass || fail "asm sentinel V86-BODY-3C missing"
grep -q '\.global legacy_v86_v86_body' "$S" \
    && pass || fail "asm body symbol missing"
grep -q '\.global legacy_v86_v86_body_sentinel' "$S" \
    && pass || fail "asm sentinel symbol missing"
grep -q '\.global legacy_v86_live_v86_code' "$S" \
    && pass || fail "asm v86 code page symbol missing"
grep -q '\.global legacy_v86_live_idt' "$S" \
    && pass || fail "asm IDT symbol missing"
grep -q '\.global legacy_v86_live_idtr' "$S" \
    && pass || fail "asm IDTR pseudo-descriptor symbol missing"
grep -q '\.global legacy_v86_live_saved_esp' "$S" \
    && pass || fail "asm saved-esp scratch symbol missing"
grep -q '\.global legacy_v86_live_gp_handler' "$S" \
    && pass || fail "asm #GP handler symbol missing"
grep -q '\.global legacy_v86_live_spurious' "$S" \
    && pass || fail "asm spurious-vector handler missing"

grep -q '\.code32' "$S" \
    && pass || fail "asm must include .code32 section for PM32 body"
grep -q '\blidt\b' "$S" \
    && pass || fail "asm must LIDT a 32-bit IDT"
grep -q '\biretl\b' "$S" \
    && pass || fail "asm must IRETL to enter v86"

# IRET frame order sanity: v86 IRETL frame requires EFLAGS with VM=1.
# VM bit is bit 17 → 0x00020000. EFLAGS push must include reserved1 (bit 1)
# per SDM, so 0x00020002 is expected.
grep -q 'pushl[[:space:]]*\$0x00020002' "$S" \
    && pass || fail "asm must push EFLAGS with VM=1 and reserved1=1 (0x00020002)"

# v86 code page must start with HLT (0xF4) to force deterministic #GP.
grep -q '\.byte 0xF4' "$S" \
    && pass || fail "asm v86 code page must begin with HLT (0xF4)"

# #GP handler must emit marker "V86!" on debugcon and unwind via saved ESP.
awk '/^legacy_v86_live_gp_handler:/,/\.size legacy_v86_v86_body/' "$S" > /tmp/v86live_gp.S
grep -q "movw[[:space:]]*\$0xE9, %dx" /tmp/v86live_gp.S \
    && pass || fail "#GP handler must write to port 0xE9"
grep -q "movb[[:space:]]*\$'V', %al" /tmp/v86live_gp.S \
    && pass || fail "#GP handler must emit 'V'"
grep -q "movb[[:space:]]*\$'8', %al" /tmp/v86live_gp.S \
    && pass || fail "#GP handler must emit '8'"
grep -q "movb[[:space:]]*\$'6', %al" /tmp/v86live_gp.S \
    && pass || fail "#GP handler must emit '6'"
grep -q "movb[[:space:]]*\$'!', %al" /tmp/v86live_gp.S \
    && pass || fail "#GP handler must emit '!'"
grep -q 'movl legacy_v86_live_saved_esp, %esp' /tmp/v86live_gp.S \
    && pass || fail "#GP handler must restore saved ESP"
grep -q '\bretl\b' /tmp/v86live_gp.S \
    && pass || fail "#GP handler must return with retl"
# Handler MUST NOT iretl back into v86 (one-shot unwind design).
if grep -qE '\biretl\b' /tmp/v86live_gp.S; then
    fail "#GP handler must NOT iretl (would re-enter v86)"
else pass; fi
rm -f /tmp/v86live_gp.S

# Body prologue must stash ESP before any IRETL.
awk '/^legacy_v86_v86_body:/,/^\._body_iret_fell_through:/' "$S" > /tmp/v86live_body.S
grep -q 'movl %esp, legacy_v86_live_saved_esp' /tmp/v86live_body.S \
    && pass || fail "body must stash ESP in legacy_v86_live_saved_esp"
# Must construct full 9-field v86 IRET frame by pushing 9 dwords before iretl.
PUSH_COUNT=$(awk '/^legacy_v86_v86_body:/,/iretl/' "$S" | grep -cE '^[[:space:]]*pushl[[:space:]]')
if [ "$PUSH_COUNT" -ge 9 ]; then
    pass
else
    fail "body must push at least 9 dwords for v86 IRET frame (got $PUSH_COUNT)"
fi
rm -f /tmp/v86live_body.S

# ------------------------------------------------------------------ boot-path leak check
# legacy_v86_live_* must only appear in its own files (plus this gate).
LEAKS=$(grep -RIlE '\blegacy_v86_live_(arm|disarm|is_armed|enter|probe)\b' \
    stage2/ 2>/dev/null \
    | grep -vE '^stage2/src/legacy_v86_live\.c$' \
    | grep -vE '^stage2/include/legacy_v86_live\.h$' \
    || true)
if [ -z "$LEAKS" ]; then
    pass
else
    fail "boot-path leak: legacy_v86_live symbols referenced from: $LEAKS"
fi

# v86-body symbols (the asm internals) must only appear in the asm file itself
# plus the wrapper C (which declares externs).
ASM_LEAKS=$(grep -RIlE '\blegacy_v86_v86_body\b' \
    stage2/ 2>/dev/null \
    | grep -vE '^stage2/src/legacy_v86_v86_body\.S$' \
    | grep -vE '^stage2/src/legacy_v86_live\.c$' \
    || true)
if [ -z "$ASM_LEAKS" ]; then
    pass
else
    fail "boot-path leak: legacy_v86_v86_body referenced from: $ASM_LEAKS"
fi

# ------------------------------------------------------------------ probe coverage
awk '/^int legacy_v86_live_probe\(/,/^}/' "$C" > /tmp/v86live_probe.c
grep -q 'legacy_v86_live_is_armed()' /tmp/v86live_probe.c \
    && pass || fail "probe must test is_armed default 0"
grep -q 'LEGACY_V86_LIVE_ERR_NOT_ARMED' /tmp/v86live_probe.c \
    && pass || fail "probe must cover NOT_ARMED path"
grep -q '0xDEADBEEFu' /tmp/v86live_probe.c \
    && pass || fail "probe must cover bad-magic path"
grep -q 'LEGACY_V86_LIVE_ERR_BAD_INPUT' /tmp/v86live_probe.c \
    && pass || fail "probe must cover BAD_INPUT path"
grep -q 'LEGACY_V86_LIVE_ARM_MAGIC' /tmp/v86live_probe.c \
    && pass || fail "probe must cover successful arm path"
grep -q 'mode_switch_disarm();' /tmp/v86live_probe.c \
    && pass || fail "probe must exercise MODE_SWITCH_OFF path"
grep -q 'LEGACY_V86_LIVE_ERR_MODE_SWITCH_OFF' /tmp/v86live_probe.c \
    && pass || fail "probe must assert MODE_SWITCH_OFF return"
grep -q 'legacy_v86_live_disarm();' /tmp/v86live_probe.c \
    && pass || fail "probe must clean up (disarm)"
rm -f /tmp/v86live_probe.c

echo "[test-legacy-v86-live] OK=$OK FAIL=$FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "[PASS] OPENGEM-044 Stage 3C v86 live scaffold gate"
    exit 0
else
    echo "[FAIL] OPENGEM-044 Stage 3C v86 live scaffold gate"
    exit 1
fi
