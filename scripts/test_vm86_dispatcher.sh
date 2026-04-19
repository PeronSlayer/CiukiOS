#!/usr/bin/env bash
# OPENGEM-020 — INT dispatcher skeleton static gate.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
H=stage2/include/vm86.h
C=stage2/src/vm86.c
gf "$C" "OPENGEM-020" "sentinel"
# Types
gf "$H" "typedef void (*vm86_int_handler)(vm86_task *task, vm86_trap_frame *frame);" "handler typedef"
gf "$H" "#define VM86_INT_VECTOR_COUNT  0x100" "vector count macro"
for s in VM86_DISPATCH_UNHANDLED VM86_DISPATCH_HANDLED VM86_DISPATCH_EXIT VM86_DISPATCH_FAULT; do
  gf "$H" "$s" "status $s"
done
gf "$H" "typedef struct vm86_dispatcher {" "dispatcher struct"
gf "$H" "    vm86_int_handler handler[VM86_INT_VECTOR_COUNT];" "handler array"
for f in registered_count unhandled_count handled_count; do
  grep -qE "    u32 $f" "$H" && pass || fail "dispatcher field $f"
done
# API
gf "$H" "int vm86_register_int_handler(vm86_dispatcher *d, u8 vec, vm86_int_handler h);" "register api"
gf "$H" "vm86_dispatch_status vm86_dispatch_int(vm86_dispatcher *d," "dispatch api"
gf "$H" "int vm86_dispatcher_probe(void);" "probe api"
# Implementations
gf "$C" "int vm86_register_int_handler(vm86_dispatcher *d, u8 vec, vm86_int_handler h) {" "register impl"
gf "$C" "vm86_dispatch_status vm86_dispatch_int(vm86_dispatcher *d," "dispatch impl"
gf "$C" "int vm86_dispatcher_probe(void) {" "probe impl"
# Markers (7)
gf "$C" "vm86: dispatcher phase=020 status=planned" "marker 1"
gf "$C" "vm86: dispatcher vector-count=0x" "marker 2"
gf "$C" "vm86: dispatcher status-codes=unhandled,handled,exit,fault" "marker 3"
gf "$C" "vm86: dispatcher empty-probe vec=0x21 status=0x" "marker 4"
gf "$C" "vm86: dispatcher registered-count=0x" "marker 5"
gf "$C" "vm86: dispatcher complete" "marker 6"
# Ordering
ORDER=$(awk '
  /serial_write\("vm86: dispatcher phase=020/ && !a { a=NR }
  /serial_write\("vm86: dispatcher vector-count=0x/ && !b { b=NR }
  /serial_write\("vm86: dispatcher status-codes=/ && !c { c=NR }
  /serial_write\("vm86: dispatcher empty-probe vec=0x21 status=0x/ && !d { d=NR }
  /serial_write\("vm86: dispatcher registered-count=0x/ && !e { e=NR }
  /serial_write\("vm86: dispatcher complete/ && !f { f=NR }
  END { print (a&&b&&c&&d&&e&&f && a<b && b<c && c<d && d<e && e<f) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
# Correctness assertions in the probe itself
grep -qF 'return (s1 == VM86_DISPATCH_UNHANDLED) ? 1 : 0;' "$C" && pass || fail "empty-table correctness check"
# Append-only registration contract (no overwrite)
grep -qF 'if (d->handler[vec]) {' "$C" && pass || fail "append-only handler guard"
# Null guards
grep -qF 'if (!d || !h) {' "$C" && pass || fail "register null guard"
grep -qF 'if (!d || !task || !frame) {' "$C" && pass || fail "dispatch null guard"
# Not invoked from live boot path
if grep -rn "vm86_dispatcher_probe\|vm86_register_int_handler\|vm86_dispatch_int" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null | grep -v '^Binary'; then
  fail "dispatcher APIs must not be invoked from live boot path yet"
else
  pass
fi
gf Makefile "test-vm86-dispatcher:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
[ "$FAIL" -eq 0 ] && { echo "[PASS] OPENGEM-020 vm86 dispatcher gate"; exit 0; }
exit 1
