#!/usr/bin/env bash
# OPENGEM-022 — INT 20h + INT 21h AH=02/09 console static gate.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
H=stage2/include/vm86.h
C=stage2/src/vm86.c
gf "$C" "OPENGEM-022" "sentinel"
# Types/prototypes
gf "$H" "#define VM86_CONSOLE_SINK_BYTES 0x100" "sink size macro"
gf "$H" "typedef struct vm86_console_sink {" "sink struct"
gf "$H" "void vm86_console_sink_attach(vm86_console_sink *sink);" "attach prototype"
gf "$H" "void vm86_console_sink_reset(vm86_console_sink *sink);" "reset prototype"
gf "$H" "void vm86_int20_handler(vm86_task *task, vm86_trap_frame *frame);" "int20 prototype"
gf "$H" "void vm86_int21_02_handler(vm86_task *task, vm86_trap_frame *frame);" "int21-02 prototype"
gf "$H" "void vm86_int21_09_handler(vm86_task *task, vm86_trap_frame *frame);" "int21-09 prototype"
gf "$H" "int vm86_console_probe(void);" "probe prototype"
# Implementations
gf "$C" "void vm86_console_sink_attach(vm86_console_sink *sink) {" "attach impl"
gf "$C" "void vm86_console_sink_reset(vm86_console_sink *sink) {" "reset impl"
gf "$C" "void vm86_int20_handler(vm86_task *task, vm86_trap_frame *frame) {" "int20 impl"
gf "$C" "void vm86_int21_02_handler(vm86_task *task, vm86_trap_frame *frame) {" "int21-02 impl"
gf "$C" "void vm86_int21_09_handler(vm86_task *task, vm86_trap_frame *frame) {" "int21-09 impl"
gf "$C" "int vm86_console_probe(void) {" "probe impl"
# Handler correctness
gf "$C" "task->exit_reason     = VM86_EXIT_REASON_INT20;" "int20 sets reason"
gf "$C" "u8 dl = (u8)(frame->edx & 0xFFu);" "int21-02 reads DL"
gf "$C" "u32 seg    = (u32)frame->ds;" "int21-09 reads DS"
gf "$C" "u32 linear = (seg << 4) + off;" "int21-09 computes linear"
gf "$C" "if (c == (u8)'\$') {" "int21-09 dollar terminator"
gf "$C" "vm86_console_write_byte(c);" "int21-09 writes via sink"
# Conventional-memory bounds check
gf "$C" "if (linear >= task->conventional_bytes) {" "int21-09 bounds check"
gf "$C" "task->fault_count++;" "int21-09 fault count"
# Sink overflow protection
gf "$C" "if (s->count >= VM86_CONSOLE_SINK_BYTES) {" "sink overflow guard"
gf "$C" "s->overflow++;" "sink overflow counter"
# Markers
gf "$C" "vm86: console phase=022 status=planned" "marker 1"
gf "$C" "vm86: console registered vec=0x20 handler=int20" "marker 2"
gf "$C" "vm86: console registered vec=0x21 handler=int21-02" "marker 3"
gf "$C" "vm86: console ah=02 dl=0x48 status=0x" "marker 4"
gf "$C" "vm86: console ah=09 ds:dx=0000:0010 status=0x" "marker 5"
gf "$C" "vm86: console int20 status=0x" "marker 6"
gf "$C" "vm86: console sink-bytes=H,i,! overflow=0x" "marker 7"
gf "$C" "vm86: console complete" "marker 8"
# Ordering
ORDER=$(awk '
  /serial_write\("vm86: console phase=022/ && !a { a=NR }
  /serial_write\("vm86: console registered vec=0x20 handler=int20/ && !b { b=NR }
  /serial_write\("vm86: console registered vec=0x21 handler=int21-02/ && !c { c=NR }
  /serial_write\("vm86: console ah=02 dl=0x48 status=0x/ && !d { d=NR }
  /serial_write\("vm86: console ah=09 ds:dx=0000:0010 status=0x/ && !e { e=NR }
  /serial_write\("vm86: console int20 status=0x/ && !f { f=NR }
  /serial_write\("vm86: console sink-bytes=H,i,! overflow=0x/ && !g { g=NR }
  /serial_write\("vm86: console complete/ && !h { h=NR }
  END { print (a&&b&&c&&d&&e&&f&&g&&h && a<b && b<c && c<d && d<e && e<f && f<g && g<h) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
# Probe assertions
for a in \
  "s1 == VM86_DISPATCH_HANDLED" \
  "s2 == VM86_DISPATCH_HANDLED" \
  "s3 == VM86_DISPATCH_EXIT" \
  "task.state == VM86_TASK_STATE_EXITED" \
  "task.exit_reason == VM86_EXIT_REASON_INT20" \
  "sink.count    == 0x3u" \
  "sink.buf[0]   == (u8)'H'" \
  "sink.buf[1]   == (u8)'i'" \
  "sink.buf[2]   == (u8)'!'" \
  "sink.overflow == 0u"; do
  grep -qF "$a" "$C" && pass || fail "probe assertion: $a"
done
# Guard byte proves $ stop
gf "$C" "convbuf[0x13] = (u8)'X';   /* guard byte: must NOT reach sink */" "guard byte after dollar"
# No live call site
if grep -rn "vm86_console_probe\|vm86_int20_handler\|vm86_int21_02_handler\|vm86_int21_09_handler\|vm86_console_sink_attach" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null | grep -v '^Binary'; then
  fail "console handlers must not be invoked from live boot path yet"
else
  pass
fi
gf Makefile "test-vm86-console:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
[ "$FAIL" -eq 0 ] && { echo "[PASS] OPENGEM-022 vm86 console gate"; exit 0; }
exit 1
