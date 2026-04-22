#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

SHELL=stage2/src/shell.c
MAKEFILE=Makefile

test -f "$SHELL" && pass || fail "shell.c missing"
test -f "$MAKEFILE" && pass || fail "Makefile missing"

grep -q 'mstest X' "$SHELL" \
    && pass || fail "help missing mstest entry"
grep -q 'Usage: mstest <probe|arm|disarm>' "$SHELL" \
    && pass || fail "usage string missing"

for fn in shell_serial_write_int shell_cmd_mstest_probe shell_cmd_mstest_arm shell_cmd_mstest_disarm shell_cmd_mstest; do
    grep -q "static void $fn" "$SHELL" \
        && pass || fail "missing handler $fn"
done

probe_line=$(grep -n 'static void shell_cmd_mstest_probe' "$SHELL" | head -n1 | cut -d: -f1)
arm_line=$(grep -n 'static void shell_cmd_mstest_arm' "$SHELL" | head -n1 | cut -d: -f1)
disarm_line=$(grep -n 'static void shell_cmd_mstest_disarm' "$SHELL" | head -n1 | cut -d: -f1)
cmd_line=$(grep -n 'static void shell_cmd_mstest(const char \*args)' "$SHELL" | head -n1 | cut -d: -f1)
if [ -n "$probe_line" ] && [ -n "$arm_line" ] && [ -n "$disarm_line" ] \
   && [ -n "$cmd_line" ] && [ "$probe_line" -lt "$arm_line" ] \
   && [ "$arm_line" -lt "$disarm_line" ] && [ "$disarm_line" -lt "$cmd_line" ]; then
    pass
else
    fail "mstest handlers not ordered probe -> arm -> disarm -> cmd"
fi

awk '/^static void shell_cmd_mstest_probe\(void\)/,/^}/' "$SHELL" > /tmp/mstest_probe.c
grep -q 'SHELL_MODE_SWITCH_CALL(probe)' /tmp/mstest_probe.c \
    && pass || fail "probe handler missing mode_switch probe call"
grep -q 'legacy_v86_probe' /tmp/mstest_probe.c \
    && pass || fail "probe handler missing legacy_v86_probe"
grep -q 'v86_dispatch_probe' /tmp/mstest_probe.c \
    && pass || fail "probe handler missing v86_dispatch_probe"
grep -q '\[ mstest \] probe mode_switch=' /tmp/mstest_probe.c \
    && pass || fail "probe marker missing mode_switch"
if grep -q 'legacy_v86_enter' /tmp/mstest_probe.c; then
    fail "probe handler must not call legacy_v86_enter"
else
    pass
fi

awk '/^static void shell_cmd_mstest_arm\(void\)/,/^}/' "$SHELL" > /tmp/mstest_arm.c
grep -q 'MODE_SWITCH_ARM_MAGIC' /tmp/mstest_arm.c \
    && pass || fail "arm handler missing MODE_SWITCH_ARM_MAGIC"
grep -q 'LEGACY_V86_ARM_MAGIC' /tmp/mstest_arm.c \
    && pass || fail "arm handler missing LEGACY_V86_ARM_MAGIC"
grep -q 'V86_DISPATCH_ARM_MAGIC' /tmp/mstest_arm.c \
    && pass || fail "arm handler missing V86_DISPATCH_ARM_MAGIC"
if grep -q 'trampoline_arm' /tmp/mstest_arm.c; then
    fail "arm handler must not call trampoline_arm"
else
    pass
fi
if grep -q 'legacy_v86_enter' /tmp/mstest_arm.c; then
    fail "arm handler must not call legacy_v86_enter"
else
    pass
fi

awk '/^static void shell_cmd_mstest_disarm\(void\)/,/^}/' "$SHELL" > /tmp/mstest_disarm.c
grep -q 'v86_dispatch_disarm' /tmp/mstest_disarm.c \
    && pass || fail "disarm handler missing v86_dispatch_disarm"
grep -q 'legacy_v86_disarm' /tmp/mstest_disarm.c \
    && pass || fail "disarm handler missing legacy_v86_disarm"
grep -q 'SHELL_MODE_SWITCH_CALL(disarm)' /tmp/mstest_disarm.c \
    && pass || fail "disarm handler missing mode_switch disarm call"
if grep -q 'trampoline_arm' /tmp/mstest_disarm.c; then
    fail "disarm handler must not reference trampoline_arm"
else
    pass
fi
if grep -q 'legacy_v86_enter' /tmp/mstest_disarm.c; then
    fail "disarm handler must not reference legacy_v86_enter"
else
    pass
fi

awk '/^static void shell_cmd_mstest\(const char \*args\)/,/^}/' "$SHELL" > /tmp/mstest_cmd.c
grep -q 'str_eq(subcmd, "probe")' /tmp/mstest_cmd.c \
    && pass || fail "mstest dispatcher missing probe subcommand"
grep -q 'str_eq(subcmd, "arm")' /tmp/mstest_cmd.c \
    && pass || fail "mstest dispatcher missing arm subcommand"
grep -q 'str_eq(subcmd, "disarm")' /tmp/mstest_cmd.c \
    && pass || fail "mstest dispatcher missing disarm subcommand"
grep -q 'shell_cmd_mstest(get_arg_ptr(line))' "$SHELL" \
    && pass || fail "shell dispatcher missing mstest command"

grep -q '^test-mstest-shell:' "$MAKEFILE" \
    && pass || fail "Makefile missing test-mstest-shell target"
grep -q 'scripts/test_mstest_shell.sh' "$MAKEFILE" \
    && pass || fail "Makefile target does not invoke scripts/test_mstest_shell.sh"

rm -f /tmp/mstest_probe.c /tmp/mstest_arm.c /tmp/mstest_disarm.c /tmp/mstest_cmd.c

echo "[test-mstest-shell] OK=$OK FAIL=$FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "[PASS] OPENGEM-044 stage3A mstest shell gate"
    exit 0
else
    echo "[FAIL] OPENGEM-044 stage3A mstest shell gate"
    exit 1
fi