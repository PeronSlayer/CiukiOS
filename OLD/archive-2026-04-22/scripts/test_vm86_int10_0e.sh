#!/usr/bin/env bash
# OPENGEM-023 — INT 10h AH=0Eh teletype static gate.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
H=stage2/include/vm86.h
C=stage2/src/vm86.c
gf "$C" "OPENGEM-023" "sentinel"
gf "$H" "void vm86_int10_0e_handler(vm86_task *task, vm86_trap_frame *frame);" "handler prototype"
gf "$H" "int vm86_int10_0e_probe(void);" "probe prototype"
gf "$C" "void vm86_int10_0e_handler(vm86_task *task, vm86_trap_frame *frame) {" "handler impl"
gf "$C" "int vm86_int10_0e_probe(void) {" "probe impl"
# Handler correctness: AL extraction + sink routing
gf "$C" "u8 al = (u8)(frame->eax & 0xFFu);" "handler reads AL"
gf "$C" "vm86_console_write_byte(al);" "handler routes via sink"
gf "$C" "task->int_count++;" "handler counts INT"
# Markers
gf "$C" "vm86: int10-0e phase=023 status=planned" "marker 1"
gf "$C" "vm86: int10-0e registered vec=0x10 handler=teletype" "marker 2"
gf "$C" "vm86: int10-0e stream len=0x" "marker 3"
gf "$C" "vm86: int10-0e sink-bytes=O,K handled-count=0x" "marker 4"
gf "$C" "vm86: int10-0e complete" "marker 5"
# Ordering
ORDER=$(awk '
  /serial_write\("vm86: int10-0e phase=023/ && !a { a=NR }
  /serial_write\("vm86: int10-0e registered vec=0x10/ && !b { b=NR }
  /serial_write\("vm86: int10-0e stream len=0x/ && !c { c=NR }
  /serial_write\("vm86: int10-0e sink-bytes=O,K handled-count=0x/ && !d { d=NR }
  /serial_write\("vm86: int10-0e complete/ && !e { e=NR }
  END { print (a&&b&&c&&d&&e && a<b && b<c && c<d && d<e) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
# Probe assertions
for a in \
  "s0 == VM86_DISPATCH_HANDLED" \
  "s1 == VM86_DISPATCH_HANDLED" \
  "sink.count == 0x2u" \
  "sink.buf[0] == (u8)'O'" \
  "sink.buf[1] == (u8)'K'" \
  "sink.overflow == 0u" \
  "local.handled_count == 0x2u" \
  "task.int_count == 0x2u"; do
  grep -qF "$a" "$C" && pass || fail "probe assertion: $a"
done
# AH=0Eh seed (0x0E00 | AL)
grep -qF '(u32)0x0E00u | (u32)payload[0]' "$C" && pass || fail "AH=0Eh seed byte 0"
grep -qF '(u32)0x0E00u | (u32)payload[1]' "$C" && pass || fail "AH=0Eh seed byte 1"
# No live call site
if grep -rn "vm86_int10_0e_handler\|vm86_int10_0e_probe" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null | grep -v '^Binary'; then
  fail "INT 10h handler must not be invoked from live boot path yet"
else
  pass
fi
gf Makefile "test-vm86-int10-0e:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
[ "$FAIL" -eq 0 ] && { echo "[PASS] OPENGEM-023 vm86 int10-0e gate"; exit 0; }
exit 1
