#!/usr/bin/env bash
# OPENGEM-044 Stage 3B static gate: mstest trampoline smoke wiring.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK + 1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }

H=stage2/include/mode_switch.h
S=stage2/src/shell.c
A=stage2/src/mstest_pm32_body.S
M=Makefile

test -f "$H" && pass || fail "header $H missing"
test -f "$S" && pass || fail "shell source missing"
test -f "$A" && pass || fail "asm body $A missing"

gf "$H" "#define MODE_SWITCH_TRAMPOLINE_ARM_MAGIC 0xC1D3944Au" "trampoline arm magic exported"
gf "$H" "int  mode_switch_trampoline_arm(uint32_t magic);" "trampoline arm signature"
gf "$H" "void mode_switch_trampoline_disarm(void);" "trampoline disarm signature"
gf "$H" "int  mode_switch_trampoline_is_live(void);" "trampoline live signature"

gf "$S" "typedef struct shell_mstest_trampoline_user" "mstest user struct"
gf "$S" "extern void mstest_pm32_body(void *user);" "mstest body extern"
gf "$S" "extern const char mstest_pm32_sentinel[];" "mstest sentinel extern"
gf "$S" "if (str_eq(cmd, \"mstest\"))" "mstest command wiring"
gf "$S" "str_eq(sub, \"trampoline-smoke\")" "mstest subcommand wiring"
gf "$S" "SHELL_MODE_SWITCH_CALL(arm)(MODE_SWITCH_ARM_MAGIC)" "api arm call"
gf "$S" "SHELL_MODE_SWITCH_TRAMP_CALL(arm)(MODE_SWITCH_TRAMPOLINE_ARM_MAGIC)" "trampoline arm call"
gf "$S" "SHELL_MODE_SWITCH_CALL(run_legacy_pm)(mstest_pm32_body, &user)" "run legacy pm call"
gf "$S" "SHELL_MODE_SWITCH_TRAMP_CALL(disarm)();" "trampoline disarm call"
gf "$S" "[ mstest ] trampoline-smoke rc=" "mstest serial marker"
gf "$S" "Usage: mstest trampoline-smoke" "mstest usage string"

awk '/static void shell_mstest_trampoline_smoke\(void\)/,/^}/' "$S" > /tmp/mstest_044_stage3b.c
grep -qF 'SHELL_MODE_SWITCH_TRAMP_CALL(arm)' /tmp/mstest_044_stage3b.c \
    && pass || fail "mstest block must arm trampoline"
grep -qF 'SHELL_MODE_SWITCH_TRAMP_CALL(disarm)' /tmp/mstest_044_stage3b.c \
    && pass || fail "mstest block must disarm trampoline"
grep -qF 'mstest_pm32_body' /tmp/mstest_044_stage3b.c \
    && pass || fail "mstest block must call dedicated PM32 body"
if grep -q 'legacy_v86_pm32_body' /tmp/mstest_044_stage3b.c; then
    fail "mstest block must not reuse legacy_v86_pm32_body"
else
    pass
fi
rm -f /tmp/mstest_044_stage3b.c

gf "$A" '.global mstest_pm32_body' "asm exports body"
gf "$A" '.global mstest_pm32_sentinel' "asm exports sentinel"
gf "$A" '.asciz "OPENGEM-044-RT"' "asm sentinel string"
gf "$A" 'movw    $0x00E9, %dx' "asm debugcon port"
gf "$A" 'retl' "asm returns via retl"

OUTB_COUNT=$(grep -c 'outb    %al, %dx' "$A")
if [ "$OUTB_COUNT" -eq 14 ]; then
    pass
else
    fail "expected 14 outb writes in mstest_pm32_body.S, found $OUTB_COUNT"
fi

gf "$M" 'test-mstest-trampoline:' "Makefile target"
gf "$M" 'bash ./scripts/test_mstest_trampoline.sh' "Makefile recipe"

echo "[test-mstest-trampoline] OK=$OK FAIL=$FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "[PASS] OPENGEM-044 Stage 3B mstest trampoline gate"
    exit 0
fi

echo "[FAIL] OPENGEM-044 Stage 3B mstest trampoline gate"
exit 1