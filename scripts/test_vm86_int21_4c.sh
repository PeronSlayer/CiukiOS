#!/usr/bin/env bash
# OPENGEM-021 — INT 21h AH=4Ch exit handler static gate.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
H=stage2/include/vm86.h
C=stage2/src/vm86.c
gf "$C" "OPENGEM-021" "sentinel"
# Prototypes
gf "$H" "void vm86_int21_4c_handler(vm86_task *task, vm86_trap_frame *frame);" "handler prototype"
gf "$H" "int vm86_int21_4c_probe(void);" "probe prototype"
# Implementations
gf "$C" "void vm86_int21_4c_handler(vm86_task *task, vm86_trap_frame *frame) {" "handler impl"
gf "$C" "int vm86_int21_4c_probe(void) {" "probe impl"
# Handler correctness points
gf "$C" "task->exit_errorlevel = (u32)errorlevel;" "handler sets errorlevel"
gf "$C" "task->exit_reason     = VM86_EXIT_REASON_INT21_4C;" "handler sets exit_reason"
gf "$C" "task->state           = VM86_TASK_STATE_EXITED;" "handler sets state"
gf "$C" "(frame->eax & 0xFFu)" "handler reads AL from EAX"
# Dispatcher extension: post-handler state inspection
gf "$C" "if (task->state == VM86_TASK_STATE_EXITED) {" "dispatcher EXIT path"
gf "$C" "if (task->state == VM86_TASK_STATE_FAULTED) {" "dispatcher FAULT path"
gf "$C" "return VM86_DISPATCH_EXIT;" "dispatcher returns EXIT"
# Markers
gf "$C" "vm86: int21-4c phase=021 status=planned" "marker 1"
gf "$C" "vm86: int21-4c registered vec=0x21 registered-count=0x" "marker 2"
gf "$C" "vm86: int21-4c invoke ah=0x4c al=0x" "marker 3"
gf "$C" "vm86: int21-4c post-dispatch status=0x" "marker 4"
gf "$C" "vm86: int21-4c complete" "marker 5"
# Ordering
ORDER=$(awk '
  /serial_write\("vm86: int21-4c phase=021/ && !a { a=NR }
  /serial_write\("vm86: int21-4c registered vec=0x21/ && !b { b=NR }
  /serial_write\("vm86: int21-4c invoke ah=0x4c/ && !c { c=NR }
  /serial_write\("vm86: int21-4c post-dispatch status=0x/ && !d { d=NR }
  /serial_write\("vm86: int21-4c complete/ && !e { e=NR }
  END { print (a&&b&&c&&d&&e && a<b && b<c && c<d && d<e) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
# Probe correctness assertions (must cross-check all four outcomes)
grep -qF 's == VM86_DISPATCH_EXIT' "$C" && pass || fail "probe asserts EXIT status"
grep -qF 'task.state == VM86_TASK_STATE_EXITED' "$C" && pass || fail "probe asserts EXITED state"
grep -qF 'task.exit_reason == VM86_EXIT_REASON_INT21_4C' "$C" && pass || fail "probe asserts INT21_4C reason"
grep -qF 'task.exit_errorlevel == 0x42u' "$C" && pass || fail "probe asserts errorlevel propagation"
grep -qF 'local.handled_count == 0x1u' "$C" && pass || fail "probe asserts handled count"
# Guest-state seed (AH=4Ch, AL=42h)
grep -qF 'frame.eax   = 0x4C42u' "$C" && pass || fail "probe seeds EAX=0x4C42"
# No live call site
if grep -rn "vm86_int21_4c_handler\|vm86_int21_4c_probe" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null | grep -v '^Binary'; then
  fail "INT 21h AH=4Ch handler must not be invoked from live boot path yet"
else
  pass
fi
gf Makefile "test-vm86-int21-4c:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
[ "$FAIL" -eq 0 ] && { echo "[PASS] OPENGEM-021 vm86 int21-4c gate"; exit 0; }
exit 1
