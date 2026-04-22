#!/usr/bin/env bash
# OPENGEM-024 — gem.exe T0 readiness static gate.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
H=stage2/include/vm86.h
C=stage2/src/vm86.c
gf "$C" "OPENGEM-024" "sentinel"
# Types / prototypes
gf "$H" "void vm86_int21_30_handler(vm86_task *task, vm86_trap_frame *frame);" "int21-30 prototype"
gf "$H" "int vm86_gem_t0_readiness_probe(void);" "probe prototype"
gf "$H" "#define VM86_DOS_VERSION_MAJOR 0x05" "DOS major macro"
gf "$H" "#define VM86_DOS_VERSION_MINOR 0x00" "DOS minor macro"
# Implementations
gf "$C" "void vm86_int21_30_handler(vm86_task *task, vm86_trap_frame *frame) {" "int21-30 impl"
gf "$C" "int vm86_gem_t0_readiness_probe(void) {" "probe impl"
# int21-30 correctness
gf "$C" "eax |= ((u32)VM86_DOS_VERSION_MINOR << 8) | (u32)VM86_DOS_VERSION_MAJOR;" "int21-30 sets AX"
gf "$C" "ebx |= 0x0000FF00u;  /* BH = 0xFF generic OEM, BL = 0 */" "int21-30 sets BH=0xFF"
gf "$C" "frame->ecx &= 0xFFFF0000u;  /* CX = 0 */" "int21-30 zeros CX"
# Entry seeding (MZ-like)
gf "$C" "task.entry_cs           = 0x1000u;" "entry CS"
gf "$C" "task.entry_ip           = 0x0100u;" "entry IP"
gf "$C" "task.entry_ss           = 0x1000u;" "entry SS"
gf "$C" "task.entry_sp           = 0xFFFEu;" "entry SP"
# Banner placement
gf "$C" "convbuf[0x80] = (u8)'G';" "banner G"
gf "$C" "convbuf[0x83] = (u8)'\$';" "banner terminator"
# Markers
gf "$C" "vm86: gem-t0 phase=024 status=planned" "marker 01"
gf "$C" "vm86: gem-t0 entry cs=0x" "marker 02"
gf "$C" "vm86: gem-t0 handlers registered count=0x" "marker 03"
gf "$C" "vm86: gem-t0 int21-30 status=0x" "marker 04"
gf "$C" "vm86: gem-t0 int21-09 status=0x" "marker 05"
gf "$C" "vm86: gem-t0 int10-0e status=0x" "marker 06"
gf "$C" "vm86: gem-t0 int21-4c status=0x" "marker 07"
gf "$C" "vm86: gem-t0 sink-bytes=G,E,M,! int-count=0x" "marker 08"
gf "$C" "vm86: gem-t0 ready-surface=int20,int10-0e,int21-02,int21-09,int21-30,int21-4c" "marker 09"
gf "$C" "vm86: gem-t0 pending-surface=mode-switch,pe32-host,gdt-commit,gp-decode,iret-vm" "marker 10"
gf "$C" "vm86: gem-t0 complete" "marker 11"
# Ordering
ORDER=$(awk '
  /serial_write\("vm86: gem-t0 phase=024/ && !a { a=NR }
  /serial_write\("vm86: gem-t0 entry cs=0x/ && !b { b=NR }
  /serial_write\("vm86: gem-t0 handlers registered count=0x/ && !c { c=NR }
  /serial_write\("vm86: gem-t0 int21-30 status=0x/ && !d { d=NR }
  /serial_write\("vm86: gem-t0 int21-09 status=0x/ && !e { e=NR }
  /serial_write\("vm86: gem-t0 int10-0e status=0x/ && !f { f=NR }
  /serial_write\("vm86: gem-t0 int21-4c status=0x/ && !g { g=NR }
  /serial_write\("vm86: gem-t0 sink-bytes=G,E,M,! int-count=0x/ && !h { h=NR }
  /serial_write\("vm86: gem-t0 ready-surface=/ && !i { i=NR }
  /serial_write\("vm86: gem-t0 pending-surface=/ && !j { j=NR }
  /serial_write\("vm86: gem-t0 complete/ && !k { k=NR }
  END { print (a&&b&&c&&d&&e&&f&&g&&h&&i&&j&&k && a<b && b<c && c<d && d<e && e<f && f<g && g<h && h<i && i<j && j<k) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
# Probe assertions
for a in \
  "s_ver  == VM86_DISPATCH_HANDLED" \
  "s_ban  == VM86_DISPATCH_HANDLED" \
  "s_tty  == VM86_DISPATCH_HANDLED" \
  "s_exit == VM86_DISPATCH_EXIT" \
  "(frame.eax & 0xFFu)        == (u32)VM86_DOS_VERSION_MAJOR" \
  "((frame.eax >> 8) & 0xFFu) == (u32)VM86_DOS_VERSION_MINOR" \
  "((frame.ebx >> 8) & 0xFFu) == 0xFFu" \
  "sink.count    == 0x4u" \
  "sink.buf[0]   == (u8)'G'" \
  "sink.buf[3]   == (u8)'!'" \
  "sink.overflow == 0u" \
  "task.state        == VM86_TASK_STATE_EXITED" \
  "task.exit_reason  == VM86_EXIT_REASON_INT21_4C" \
  "task.exit_errorlevel == 0u" \
  "task.int_count == 0x4u"; do
  grep -qF "$a" "$C" && pass || fail "probe assertion: $a"
done
# AH-keyed INT 21h rotation points (proves single slot reused)
gf "$C" "local.handler[0x21] = vm86_int21_09_handler;" "slot rotate to 09"
gf "$C" "local.handler[0x21] = vm86_int21_4c_handler;" "slot rotate to 4c"
# No live call site
if grep -rn "vm86_int21_30_handler\|vm86_gem_t0_readiness_probe" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null | grep -v '^Binary'; then
  fail "gem-t0 handlers must not be invoked from live boot path yet"
else
  pass
fi
gf Makefile "test-vm86-gem-t0:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
[ "$FAIL" -eq 0 ] && { echo "[PASS] OPENGEM-024 vm86 gem-t0 readiness gate"; exit 0; }
exit 1
