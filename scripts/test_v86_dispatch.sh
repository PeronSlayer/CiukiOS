#!/usr/bin/env bash
# OPENGEM-044 Task C static gate: INT dispatcher + loader integration scaffold.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK + 1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }

H=stage2/include/v86_dispatch.h
C=stage2/src/v86_dispatch.c
S=stage2/src/shell.c
M=Makefile

test -f "$H" && pass || fail "header $H missing"
test -f "$C" && pass || fail "source $C missing"
test -f "$S" && pass || fail "shell source missing"

gf "$H" "#define V86_DISPATCH_ARM_MAGIC   0xC1D39460u" "dispatch magic"
gf "$H" "#define V86_DISPATCH_SENTINEL    0x0460u" "dispatch sentinel"
gf "$H" "typedef enum {" "enum declaration present"
gf "$H" "V86_DISPATCH_CONT" "enum CONT"
gf "$H" "V86_DISPATCH_EXIT_OK" "enum EXIT_OK"
gf "$H" "V86_DISPATCH_EXIT_ERR" "enum EXIT_ERR"
gf "$H" "v86_dispatch_result_t v86_dispatch_int(uint8_t vector, legacy_v86_frame_t *frame);" "dispatch signature"
gf "$H" "int  v86_dispatch_arm(uint32_t magic);" "arm signature"
gf "$H" "void v86_dispatch_disarm(void);" "disarm signature"
gf "$H" "int  v86_dispatch_is_armed(void);" "is_armed signature"
gf "$H" "int  v86_dispatch_probe(void);" "probe signature"
gf "$H" "LEGACY_V86_ARM_MAGIC   0xC1D39450u" "fallback legacy_v86 magic"

gf "$C" "OPENGEM-044-C" "C sentinel"
gf "$C" "static int s_v86_dispatch_armed = 0;" "armed flag defaults disarmed"
gf "$C" "return V86_DISPATCH_CONT;" "stub dispatch returns CONT"
gf "$C" "__attribute__((weak)) int legacy_v86_enter" "weak legacy_v86 enter stub"
gf "$C" "__attribute__((weak)) int legacy_v86_arm" "weak legacy_v86 arm stub"
gf "$C" "legacy_v86_frame_t frame;" "probe canned frame"
gf "$C" "0xAAAA5555u" "probe frame sentinel 0"
gf "$C" "0xDDDD8888u" "probe frame sentinel 3"

gf "$S" "#include \"mode_switch.h\"" "mode_switch include"
gf "$S" "#include \"v86_dispatch.h\"" "v86_dispatch include"
gf "$S" "[dosrun] mz dispatch=pending reason=task-b" "dosrun pending marker updated"
gf "$S" "MODE_SWITCH_ARM_MAGIC" "044A magic in gem"
gf "$S" "SHELL_MODE_SWITCH_CALL(arm)" "044A arm in gem"
gf "$S" "legacy_v86_arm(LEGACY_V86_ARM_MAGIC)" "044B arm in gem"
gf "$S" "v86_dispatch_arm(V86_DISPATCH_ARM_MAGIC)" "044C arm in gem"
gf "$S" "legacy_v86_enter(&frame, &exit_state)" "legacy_v86 enter call"
gf "$S" "v86_dispatch_int(exit_state.int_vector, &frame)" "dispatcher loop call"
gf "$S" "[gem] dispatch int=0x" "dispatch marker"
gf "$S" "[gem] pending task B arm-044B" "pending Task B arm marker"
gf "$S" "[gem] pending task B enter-044B" "pending Task B enter marker"

awk '/if \(str_eq\(cmd, "gem"\)\) \{/,/if \(str_eq\(cmd, "pwd"\)\) \{/' "$S" > /tmp/opengem044c_gem.c
grep -q "legacy_v86_enter(&frame, &exit_state)" /tmp/opengem044c_gem.c \
    && pass || fail "gem block must call legacy_v86_enter"
if grep -q "vm86_compat_entry_enter_v86" /tmp/opengem044c_gem.c; then
    fail "gem block must not call vm86_compat_entry_enter_v86"
else
    pass
fi
if grep -q "vm86_compat_task_arm" /tmp/opengem044c_gem.c; then
    fail "gem block must not arm 039 compat task path"
else
    pass
fi
if grep -q "vm86_compat_entry_live_fill_frame" /tmp/opengem044c_gem.c; then
    fail "gem block must not fill 041 compat frame"
else
    pass
fi
rm -f /tmp/opengem044c_gem.c

gf "$M" "test-v86-dispatch:" "Makefile target"
gf "$M" "bash ./scripts/test_v86_dispatch.sh" "Makefile recipe"

echo "[test-v86-dispatch] OK=$OK FAIL=$FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "[PASS] OPENGEM-044-C dispatch scaffold gate"
    exit 0
fi

echo "[FAIL] OPENGEM-044-C dispatch scaffold gate"
exit 1