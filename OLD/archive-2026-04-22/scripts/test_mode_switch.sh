#!/usr/bin/env bash
# OPENGEM-044 Task A static gate (stage-2): mode-switch engine with
# asm trampoline staged behind a second arm flag.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

H=stage2/include/mode_switch.h
C=stage2/src/mode_switch.c
S=stage2/src/mode_switch_asm.S

test -f "$H" && pass || fail "header $H missing"
test -f "$C" && pass || fail "source $C missing"
test -f "$S" && pass || fail "asm $S missing"

grep -q '#define MODE_SWITCH_ARM_MAGIC[[:space:]]*0xC1D39440u' "$H" \
    && pass || fail "MODE_SWITCH_ARM_MAGIC 0xC1D39440u missing"
grep -q '#define MODE_SWITCH_SENTINEL[[:space:]]*0x0440u' "$H" \
    && pass || fail "MODE_SWITCH_SENTINEL 0x0440u missing"
grep -q 'opengem_044_a_sentinel\[\] = "OPENGEM-044-A"' "$C" \
    && pass || fail "C sentinel OPENGEM-044-A missing"
grep -q '.asciz "OPENGEM-044-A-ASM"' "$S" \
    && pass || fail "asm sentinel OPENGEM-044-A-ASM missing"

while IFS= read -r sig; do
    grep -qF "$sig" "$H" && pass || fail "header signature missing: $sig"
done <<'EOF'
int  mode_switch_run_legacy_pm(mode_switch_legacy_pm_body_fn body, void *user);
int  mode_switch_arm(uint32_t magic);
void mode_switch_disarm(void);
int  mode_switch_is_armed(void);
int  mode_switch_probe(void);
EOF

grep -qE '^static int s_mode_switch_armed = 0;' "$C" \
    && pass || fail "s_mode_switch_armed must default to 0"
grep -qE '^static int s_mode_switch_trampoline_live = 0;' "$C" \
    && pass || fail "s_mode_switch_trampoline_live must default to 0"

grep -qE '#define MODE_SWITCH_TRAMPOLINE_ARM_MAGIC[[:space:]]+0xC1D3944Au' "$C" \
    && pass || fail "MODE_SWITCH_TRAMPOLINE_ARM_MAGIC missing"

awk '/^int mode_switch_arm\(/,/^}/' "$C" > /tmp/ms_arm.c
grep -q 'MODE_SWITCH_ARM_MAGIC' /tmp/ms_arm.c \
    && pass || fail "mode_switch_arm does not check MODE_SWITCH_ARM_MAGIC"
grep -q 's_mode_switch_armed = 1' /tmp/ms_arm.c \
    && pass || fail "mode_switch_arm does not set armed flag"
rm -f /tmp/ms_arm.c

awk '/^int mode_switch_trampoline_arm\(/,/^}/' "$C" > /tmp/ms_tarm.c
grep -q 'MODE_SWITCH_TRAMPOLINE_ARM_MAGIC' /tmp/ms_tarm.c \
    && pass || fail "mode_switch_trampoline_arm does not check its magic"
grep -q 's_mode_switch_trampoline_live = 1' /tmp/ms_tarm.c \
    && pass || fail "mode_switch_trampoline_arm does not set live flag"
rm -f /tmp/ms_tarm.c

awk '/^int mode_switch_run_legacy_pm\(/,/^}/' "$C" > /tmp/ms_run.c
head -n 6 /tmp/ms_run.c | grep -q 's_mode_switch_armed' \
    && pass || fail "run_legacy_pm must check armed flag FIRST"
grep -q 'MODE_SWITCH_ERR_NOT_ARMED' /tmp/ms_run.c \
    && pass || fail "run_legacy_pm must return NOT_ARMED"
grep -q 'MODE_SWITCH_ERR_BAD_INPUT' /tmp/ms_run.c \
    && pass || fail "run_legacy_pm must reject NULL body"
grep -q 's_mode_switch_trampoline_live' /tmp/ms_run.c \
    && pass || fail "run_legacy_pm must check trampoline-live flag"
grep -q 'MODE_SWITCH_ERR_NOT_IMPLEMENTED' /tmp/ms_run.c \
    && pass || fail "run_legacy_pm must return NOT_IMPLEMENTED when trampoline disarmed"
grep -q 'mode_switch_asm_enter' /tmp/ms_run.c \
    && pass || fail "run_legacy_pm must invoke asm trampoline when fully armed"
rm -f /tmp/ms_run.c

if grep -qE '\bmov[[:space:]]+%?cr[0-4]' "$C"; then
    fail "C file must not contain CR register writes"
else
    pass
fi
if grep -qE 'wrmsr|rdmsr' "$C"; then
    fail "C file must not contain MSR instructions"
else
    pass
fi
if grep -qE '\blgdt\b|\blidt\b|\bltr\b|\blldt\b' "$C"; then
    fail "C file must not contain descriptor-register instructions"
else
    pass
fi

grep -q '\.globl mode_switch_asm_enter' "$S" \
    && pass || fail "asm must export mode_switch_asm_enter"
grep -q '\.code64' "$S" \
    && pass || fail "asm must contain .code64 section"
grep -q '\.code32' "$S" \
    && pass || fail "asm must contain .code32 section"
grep -q 'lretq' "$S" \
    && pass || fail "asm must use lretq for long->compat hop"
grep -q 'lretl' "$S" \
    && pass || fail "asm must use lretl for compat->long hop"
grep -q 'rdmsr' "$S" \
    && pass || fail "asm must read EFER via rdmsr"
grep -q 'wrmsr' "$S" \
    && pass || fail "asm must write EFER via wrmsr"
grep -q 'sgdt' "$S" \
    && pass || fail "asm must save host GDTR"
grep -q 'lgdt' "$S" \
    && pass || fail "asm must load legacy GDTR"
grep -q 'sidt' "$S" \
    && pass || fail "asm must save host IDTR"
grep -q 'lidt' "$S" \
    && pass || fail "asm must restore host IDTR"

LEAKS=$(grep -RIlE '\bmode_switch_(arm|disarm|is_armed|run_legacy_pm|probe|trampoline_arm|trampoline_disarm|trampoline_is_live|asm_enter)\b' \
    stage2/ 2>/dev/null \
    | grep -vE '^stage2/src/mode_switch\.c$' \
    | grep -vE '^stage2/src/mode_switch_asm\.S$' \
    | grep -vE '^stage2/include/mode_switch\.h$' \
    | grep -vE '^stage2/src/legacy_v86\.c$' \
    | grep -vE '^stage2/src/legacy_v86_live\.c$' \
    | grep -vE '^stage2/include/legacy_v86_live\.h$' \
    | grep -vE '^stage2/src/legacy_v86_v86_body\.S$' \
    || true)
if [ -z "$LEAKS" ]; then
    pass
else
    fail "boot-path leak: mode_switch symbols referenced from: $LEAKS"
fi

awk '/^int mode_switch_probe\(/,/^}/' "$C" > /tmp/ms_probe.c
grep -q 'MODE_SWITCH_ERR_NOT_ARMED'       /tmp/ms_probe.c \
    && pass || fail "probe missing NOT_ARMED case"
grep -q '0xDEADBEEFu'                      /tmp/ms_probe.c \
    && pass || fail "probe missing API bad-magic case"
grep -q '0xBADBADu'                        /tmp/ms_probe.c \
    && pass || fail "probe missing trampoline bad-magic case"
grep -q 'MODE_SWITCH_ERR_BAD_INPUT'        /tmp/ms_probe.c \
    && pass || fail "probe missing BAD_INPUT case"
grep -q 'MODE_SWITCH_ERR_NOT_IMPLEMENTED'  /tmp/ms_probe.c \
    && pass || fail "probe missing NOT_IMPLEMENTED case"
grep -q 'mode_switch_trampoline_is_live'   /tmp/ms_probe.c \
    && pass || fail "probe missing trampoline_is_live check"
grep -q 'mode_switch_asm_sentinel'         /tmp/ms_probe.c \
    && pass || fail "probe missing asm sentinel reference"
rm -f /tmp/ms_probe.c

STATIC_ASSERT_COUNT=$(grep -c '_Static_assert(__builtin_offsetof(mode_switch_scratch_t' "$C")
if [ "$STATIC_ASSERT_COUNT" -ge 12 ]; then
    pass
else
    fail "not enough _Static_assert offset guards (have $STATIC_ASSERT_COUNT, need >=12)"
fi

echo "[test-mode-switch] OK=$OK FAIL=$FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "[PASS] OPENGEM-044-A mode-switch scaffold gate"
    exit 0
else
    echo "[FAIL] OPENGEM-044-A mode-switch scaffold gate"
    exit 1
fi
