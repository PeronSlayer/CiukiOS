#!/usr/bin/env bash
# OPENGEM-018 — descriptor shim static gate.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
H=stage2/include/vm86.h
C=stage2/src/vm86.c
gf "$C" "OPENGEM-018" "sentinel"
# Descriptor types
for t in vm86_gdt_entry vm86_tss32 vm86_idt_gate; do
  gf "$H" "typedef struct $t {" "type $t"
done
# GDT slot enum
for s in VM86_GDT_NULL VM86_GDT_PE_CODE32 VM86_GDT_PE_DATA32 VM86_GDT_V86_STACK VM86_GDT_V86_TSS VM86_GDT_RETURN_CODE64 VM86_GDT_RETURN_DATA64 VM86_GDT_SLOT_COUNT; do
  gf "$H" "$s" "gdt slot $s"
done
# IDT vector enum
for v in VM86_IDT_VEC_DE VM86_IDT_VEC_UD VM86_IDT_VEC_NM VM86_IDT_VEC_TS VM86_IDT_VEC_NP VM86_IDT_VEC_SS VM86_IDT_VEC_GP VM86_IDT_VEC_PF VM86_IDT_VEC_SW20 VM86_IDT_VEC_SW21 VM86_IDT_VEC_SLOT_COUNT; do
  gf "$H" "$v" "idt vec $v"
done
# TSS32 canonical fields (spot-check subset)
for f in link esp0 ss0 cr3 eip eflags ldt iopb_trap; do
  grep -qE "    u32 $f" "$H" && pass || fail "tss32 field $f"
done
# GDT entry fields
for f in limit_lo base_lo base_mid access limit_hi_flags base_hi; do
  grep -qE "u(8|16) +$f" "$H" && pass || fail "gdt entry field $f"
done
# IDT gate fields
for f in offset_lo selector reserved type_attr offset_hi; do
  grep -qE "u(8|16) +$f" "$H" && pass || fail "idt gate field $f"
done
# Probe prototype + definition
gf "$H" "int vm86_descriptors_probe(void);" "probe prototype"
gf "$C" "int vm86_descriptors_probe(void) {" "probe definition"
# Markers (6)
gf "$C" "vm86: descriptors phase=018 status=planned" "marker 1"
gf "$C" "vm86: descriptors gdt-slots=0x" "marker 2"
gf "$C" "vm86: descriptors gdt-entry-bytes=0x" "marker 3"
gf "$C" "vm86: descriptors gdt-layout=pe-code32,pe-data32,v86-stack,v86-tss,ret-code64,ret-data64" "marker 4"
gf "$C" "vm86: descriptors idt-layout=de,ud,nm,ts,np,ss,gp,pf,sw20,sw21" "marker 5"
gf "$C" "vm86: descriptors complete" "marker 6"
# Marker ordering
ORDER=$(awk '
  /serial_write\("vm86: descriptors phase=018/ && !a { a=NR }
  /serial_write\("vm86: descriptors gdt-slots=0x/ && !b { b=NR }
  /serial_write\("vm86: descriptors gdt-entry-bytes=0x/ && !c { c=NR }
  /serial_write\("vm86: descriptors gdt-layout=/ && !d { d=NR }
  /serial_write\("vm86: descriptors idt-layout=/ && !e { e=NR }
  /serial_write\("vm86: descriptors complete/ && !f { f=NR }
  END { print (a&&b&&c&&d&&e&&f && a<b && b<c && c<d && d<e && e<f) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
# Not invoked from live boot path
if grep -rn "vm86_descriptors_probe" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null | grep -v '^Binary'; then
  fail "descriptors probe must not be invoked from live boot path yet"
else
  pass
fi
gf Makefile "test-vm86-descriptors:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
[ "$FAIL" -eq 0 ] && { echo "[PASS] OPENGEM-018 vm86 descriptors gate"; exit 0; }
exit 1
