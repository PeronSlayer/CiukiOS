#!/usr/bin/env bash
# OPENGEM-044 Task B static gate: legacy PM v86 host scaffold.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

H=stage2/include/legacy_v86.h
C=stage2/src/legacy_v86.c
S=stage2/src/legacy_v86_pm32.S

test -f "$H" && pass || fail "header $H missing"
test -f "$C" && pass || fail "source $C missing"
test -f "$S" && pass || fail "asm $S missing"

grep -q '#define LEGACY_V86_ARM_MAGIC[[:space:]]*0xC1D39450u' "$H" \
    && pass || fail "LEGACY_V86_ARM_MAGIC 0xC1D39450u missing"
grep -q '#define LEGACY_V86_SENTINEL[[:space:]]*0x0450u' "$H" \
    && pass || fail "LEGACY_V86_SENTINEL 0x0450u missing"
grep -q '#define LEGACY_V86_FAULT_MODE_SWITCH_NOT_IMPLEMENTED[[:space:]]*0x04500002u' "$H" \
    && pass || fail "fault code for MODE_SWITCH_ERR_NOT_IMPLEMENTED missing"
grep -q 'opengem_044_b_sentinel\[\] = "OPENGEM-044-B"' "$C" \
    && pass || fail "C sentinel OPENGEM-044-B missing"
grep -q '\.asciz "OPENGEM-044-B"' "$S" \
    && pass || fail "asm sentinel OPENGEM-044-B missing"

while IFS= read -r sig; do
    grep -qF "$sig" "$H" && pass || fail "header signature missing: $sig"
done <<'EOF'
int legacy_v86_enter(const legacy_v86_frame_t *entry, legacy_v86_exit_t *out);
int  legacy_v86_arm(uint32_t magic);
void legacy_v86_disarm(void);
int  legacy_v86_is_armed(void);
int  legacy_v86_probe(void);
EOF

while IFS= read -r token; do
    grep -q "$token" "$H" && pass || fail "header token missing: $token"
done <<'EOF'
uint16_t cs, ip;
uint16_t ss, sp;
uint16_t ds, es, fs, gs;
uint32_t eflags;
uint32_t reserved\[4\];
LEGACY_V86_EXIT_NORMAL = 0,
LEGACY_V86_EXIT_GP_INT,
LEGACY_V86_EXIT_HALT,
LEGACY_V86_EXIT_FAULT,
EOF

grep -qE '^static int s_legacy_v86_armed = 0;' "$C" \
    && pass || fail "s_legacy_v86_armed must default to 0"

awk '/^int legacy_v86_arm\(/,/^}/' "$C" > /tmp/legacy_v86_arm.c
grep -q 'LEGACY_V86_ARM_MAGIC' /tmp/legacy_v86_arm.c \
    && pass || fail "legacy_v86_arm does not check LEGACY_V86_ARM_MAGIC"
grep -q 's_legacy_v86_armed = 1' /tmp/legacy_v86_arm.c \
    && pass || fail "legacy_v86_arm does not set armed flag"
rm -f /tmp/legacy_v86_arm.c

awk '/^int legacy_v86_enter\(/,/^}/' "$C" > /tmp/legacy_v86_enter.c
head -n 10 /tmp/legacy_v86_enter.c | grep -q 's_legacy_v86_armed' \
    && pass || fail "legacy_v86_enter must check armed flag FIRST"
grep -q 'LEGACY_V86_ERR_NOT_ARMED' /tmp/legacy_v86_enter.c \
    && pass || fail "legacy_v86_enter must return NOT_ARMED when disarmed"
grep -q 'LEGACY_V86_ERR_BAD_INPUT' /tmp/legacy_v86_enter.c \
    && pass || fail "legacy_v86_enter must reject NULL input"
grep -q 'mode_switch_run_legacy_pm(legacy_v86_pm32_body, &context)' /tmp/legacy_v86_enter.c \
    && pass || fail "legacy_v86_enter must call mode_switch_run_legacy_pm with legacy_v86_pm32_body"
grep -q 'MODE_SWITCH_ERR_NOT_ARMED' /tmp/legacy_v86_enter.c \
    && pass || fail "legacy_v86_enter must map mode-switch not armed"
grep -q 'MODE_SWITCH_ERR_NOT_IMPLEMENTED' /tmp/legacy_v86_enter.c \
    && pass || fail "legacy_v86_enter must map mode-switch not implemented"
grep -q 'LEGACY_V86_FAULT_MODE_SWITCH_NOT_IMPLEMENTED' /tmp/legacy_v86_enter.c \
    && pass || fail "legacy_v86_enter must use dedicated NOT_IMPLEMENTED fault code"
rm -f /tmp/legacy_v86_enter.c

if grep -qE '\bmov[[:space:]]+%?cr[0-4]' "$C"; then
    fail "C file must not contain CR register writes"
else
    pass
fi
if grep -qE 'wrmsr|rdmsr' "$C"; then
    fail "C file must not contain MSR writes"
else
    pass
fi
if grep -qE '\blgdt\b|\blidt\b|\bltr\b|\blldt\b' "$C"; then
    fail "C file must not contain descriptor-register writes"
else
    pass
fi

grep -q '\.global legacy_v86_pm32_body' "$S" \
    && pass || fail "asm body symbol missing"
grep -q 'movw[[:space:]]*\$0x00E9, %dx' "$S" \
    && pass || fail "asm body must target port 0xE9"
grep -q 'outb[[:space:]]*%al, %dx' "$S" \
    && pass || fail "asm body must emit serial marker bytes"
grep -q 'retl' "$S" \
    && pass || fail "asm body must return with retl"

LEAKS=$(grep -RIlE '\blegacy_v86_(arm|disarm|is_armed|enter|probe)\b' \
    stage2/ 2>/dev/null \
    | grep -vE '^stage2/src/legacy_v86\.c$' \
    | grep -vE '^stage2/src/legacy_v86_pm32\.S$' \
    | grep -vE '^stage2/include/legacy_v86\.h$' \
    | grep -vE '^stage2/src/v86_dispatch\.c$' \
    | grep -vE '^stage2/include/v86_dispatch\.h$' \
    | grep -vE '^stage2/src/shell\.c$' \
    || true)
if [ -z "$LEAKS" ]; then
    pass
else
    fail "boot-path leak: legacy_v86 symbols referenced from: $LEAKS"
fi

awk '/^int legacy_v86_probe\(/,/^}/' "$C" > /tmp/legacy_v86_probe.c
grep -q 'LEGACY_V86_ERR_NOT_ARMED' /tmp/legacy_v86_probe.c \
    && pass || fail "probe missing NOT_ARMED case"
grep -q '0xDEADBEEFu' /tmp/legacy_v86_probe.c \
    && pass || fail "probe missing bad-magic case"
grep -q 'LEGACY_V86_ERR_BAD_INPUT' /tmp/legacy_v86_probe.c \
    && pass || fail "probe missing BAD_INPUT cases"
grep -q 'LEGACY_V86_FAULT_MODE_SWITCH_NOT_ARMED' /tmp/legacy_v86_probe.c \
    && pass || fail "probe missing mode-switch disarmed mapping"
grep -q 'LEGACY_V86_FAULT_MODE_SWITCH_NOT_IMPLEMENTED' /tmp/legacy_v86_probe.c \
    && pass || fail "probe missing not-implemented mapping"
grep -q 'mode_switch_arm(MODE_SWITCH_ARM_MAGIC)' /tmp/legacy_v86_probe.c \
    && pass || fail "probe must exercise Task A armed path"
grep -q 'legacy_v86_disarm();' /tmp/legacy_v86_probe.c \
    && pass || fail "probe must disarm host gate"
rm -f /tmp/legacy_v86_probe.c

echo "[test-legacy-v86] OK=$OK FAIL=$FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "[PASS] OPENGEM-044-B legacy-v86 scaffold gate"
    exit 0
else
    echo "[FAIL] OPENGEM-044-B legacy-v86 scaffold gate"
    exit 1
fi