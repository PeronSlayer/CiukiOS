#!/usr/bin/env bash
# OPENGEM-019 — VM task descriptor static gate.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
H=stage2/include/vm86.h
C=stage2/src/vm86.c
gf "$C" "OPENGEM-019" "sentinel"
# State enum
for s in VM86_TASK_STATE_IDLE VM86_TASK_STATE_READY VM86_TASK_STATE_RUNNING VM86_TASK_STATE_INT_TRAP VM86_TASK_STATE_FAULTED VM86_TASK_STATE_EXITED VM86_TASK_STATE_COUNT; do
  gf "$H" "$s" "state $s"
done
# Exit reason enum
for r in VM86_EXIT_REASON_NONE VM86_EXIT_REASON_INT20 VM86_EXIT_REASON_INT21_4C VM86_EXIT_REASON_FAULT VM86_EXIT_REASON_HOST_ABORT VM86_EXIT_REASON_COUNT; do
  gf "$H" "$r" "exit reason $r"
done
# Task struct + fields
gf "$H" "typedef struct vm86_task {" "task struct"
for f in handle state exit_reason exit_errorlevel entry_cs entry_ip entry_ss entry_sp conventional_base conventional_bytes int_count fault_count; do
  grep -qE "    u(8|16|32|64) +$f" "$H" && pass || fail "task field $f"
done
grep -qE "    vm86_trap_frame +regs" "$H" && pass || fail "task field regs"
# Probe
gf "$H" "int vm86_task_probe(void);" "probe prototype"
gf "$C" "int vm86_task_probe(void) {" "probe definition"
# Markers
gf "$C" "vm86: task phase=019 status=planned" "marker 1"
gf "$C" "vm86: task bytes=0x" "marker 2"
gf "$C" "vm86: task fields=handle,state,exit-reason,exit-errorlevel,entry-cs,entry-ip,entry-ss,entry-sp,regs,conventional-base,conventional-bytes,int-count,fault-count" "marker 3"
gf "$C" "vm86: task states=idle,ready,running,int-trap,faulted,exited" "marker 4"
gf "$C" "vm86: task exit-reasons=none,int20,int21-4c,fault,host-abort" "marker 5"
gf "$C" "vm86: task conventional-window-bytes=0x" "marker 6"
gf "$C" "vm86: task complete" "marker 7"
# Ordering
ORDER=$(awk '
  /serial_write\("vm86: task phase=019/ && !a { a=NR }
  /serial_write\("vm86: task bytes=0x/ && !b { b=NR }
  /serial_write\("vm86: task fields=/ && !c { c=NR }
  /serial_write\("vm86: task states=/ && !d { d=NR }
  /serial_write\("vm86: task exit-reasons=/ && !e { e=NR }
  /serial_write\("vm86: task conventional-window-bytes=0x/ && !f { f=NR }
  /serial_write\("vm86: task complete/ && !g { g=NR }
  END { print (a&&b&&c&&d&&e&&f&&g && a<b && b<c && c<d && d<e && e<f && f<g) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
if grep -rn "vm86_task_probe" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null | grep -v '^Binary'; then
  fail "task probe must not be invoked from live boot path yet"
else
  pass
fi
gf Makefile "test-vm86-task:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
[ "$FAIL" -eq 0 ] && { echo "[PASS] OPENGEM-019 vm86 task gate"; exit 0; }
exit 1
