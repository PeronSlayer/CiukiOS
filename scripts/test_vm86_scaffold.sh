#!/usr/bin/env bash
# OPENGEM-017 — mode-switch scaffold static gate.
# Asserts that the v8086 observability scaffold is in place without
# enabling any runtime mode switch. This is a design-gated phase.
set -u
cd "$(dirname "$0")/.."
OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
check_grep_f() {
  local f="$1"; local pat="$2"; local desc="$3"
  if grep -qF -- "$pat" "$f"; then pass; else fail "$desc (missing: $pat)"; fi
}
check_grep_e() {
  local f="$1"; local pat="$2"; local desc="$3"
  if grep -qE -- "$pat" "$f"; then pass; else fail "$desc (regex: $pat)"; fi
}
H=stage2/include/vm86.h
C=stage2/src/vm86.c
[ -f "$H" ] || { echo "[FAIL] missing $H"; exit 1; }
[ -f "$C" ] || { echo "[FAIL] missing $C"; exit 1; }
# Sentinel + identity
check_grep_f "$C" "OPENGEM-017" "sentinel present"
check_grep_f "$H" "STAGE2_VM86_H" "header guard present"
# ABI types
check_grep_f "$H" "VM86_MODE_HOST_LONG" "mode enum host-long"
check_grep_f "$H" "VM86_MODE_COMPAT_PE32" "mode enum compat-pe32"
check_grep_f "$H" "VM86_MODE_GUEST_V8086" "mode enum guest-v8086"
check_grep_f "$H" "vm86_trap_frame" "trap frame typedef"
# Trap frame fields (design §5.2)
for f in eax ebx ecx edx esi edi ebp esp eip eflags cs ds es fs gs ss; do
  check_grep_e "$H" "    (u32|u16) $f" "trap frame field $f"
done
# Probe function
check_grep_f "$H" "int vm86_scaffold_probe(void);" "probe prototype"
check_grep_f "$C" "int vm86_scaffold_probe(void) {" "probe definition"
# Marker set (frozen, 4 lines)
check_grep_f "$C" "vm86: scaffold phase=017 status=planned" "marker 1"
check_grep_f "$C" "vm86: scaffold host-mode=long compat-mode=pe32 guest-mode=v8086" "marker 2"
check_grep_f "$C" "vm86: scaffold frame-bytes=0x" "marker 3"
check_grep_f "$C" "vm86: scaffold complete" "marker 4"
# No live call site yet — MUST NOT be referenced from boot path
if grep -rn "vm86_scaffold_probe" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null | grep -v '^Binary'; then
  fail "scaffold probe must not be invoked from live boot path yet"
else
  pass
fi
# Marker internal ordering (awk anchored on serial_write lines)
ORDER=$(awk '
  /serial_write\("vm86: scaffold phase=017/ && !a { a=NR }
  /serial_write\("vm86: scaffold host-mode=long/ && !b { b=NR }
  /serial_write\("vm86: scaffold frame-bytes=0x/ && !c { c=NR }
  /serial_write\("vm86: scaffold complete/ && !d { d=NR }
  END { print (a && b && c && d && a<b && b<c && c<d) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
# Makefile target present
check_grep_f Makefile "test-vm86-scaffold:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
if [ "$FAIL" -eq 0 ]; then
  echo "[PASS] OPENGEM-017 vm86 scaffold gate"
  exit 0
fi
exit 1
