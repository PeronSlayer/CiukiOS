#!/usr/bin/env bash
# OPENGEM-025 — GDT byte-encoder static gate.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
H=stage2/include/vm86.h
C=stage2/src/vm86.c
gf "$C" "OPENGEM-025" "sentinel"
# Header API
gf "$H" "u32 vm86_gdt_encode(u8 *out, u32 tss_base, u16 tss_limit);" "encode prototype"
gf "$H" "u8  vm86_gdt_read_access(const u8 *buf, u32 slot);" "read_access prototype"
gf "$H" "u32 vm86_gdt_read_base(const u8 *buf, u32 slot);" "read_base prototype"
gf "$H" "u32 vm86_gdt_read_limit(const u8 *buf, u32 slot);" "read_limit prototype"
gf "$H" "int vm86_gdt_encoder_probe(void);" "probe prototype"
gf "$H" "#define VM86_GDT_BYTES  (VM86_GDT_SLOT_COUNT * 8)" "size macro"
gf "$H" "#define VM86_GDT_AR_CODE32  0x9A" "AR code32"
gf "$H" "#define VM86_GDT_AR_DATA32  0x92" "AR data32"
gf "$H" "#define VM86_GDT_AR_TSS32   0x89" "AR tss32"
gf "$H" "#define VM86_GDT_FLAGS_32   0xC" "flags 32"
gf "$H" "#define VM86_GDT_FLAGS_64   0xA" "flags 64"
gf "$H" "#define VM86_GDT_FLAGS_TSS  0x0" "flags tss"
# Implementations
gf "$C" "u32 vm86_gdt_encode(u8 *out, u32 tss_base, u16 tss_limit) {" "encode impl"
gf "$C" "u8 vm86_gdt_read_access(const u8 *buf, u32 slot) {" "read_access impl"
gf "$C" "u32 vm86_gdt_read_base(const u8 *buf, u32 slot) {" "read_base impl"
gf "$C" "u32 vm86_gdt_read_limit(const u8 *buf, u32 slot) {" "read_limit impl"
gf "$C" "int vm86_gdt_encoder_probe(void) {" "probe impl"
gf "$C" "static void vm86_gdt_write_slot(u8 *dst," "write_slot helper"
# Byte-layout contract (critical — these are the CPU-visible encodings)
gf "$C" "dst[0] = (u8)(limit & 0xFFu);" "byte0 limit_lo"
gf "$C" "dst[1] = (u8)((limit >> 8) & 0xFFu);" "byte1 limit_mid"
gf "$C" "dst[2] = (u8)(base & 0xFFu);" "byte2 base_lo"
gf "$C" "dst[3] = (u8)((base >> 8) & 0xFFu);" "byte3 base_mid_lo"
gf "$C" "dst[4] = (u8)((base >> 16) & 0xFFu);" "byte4 base_mid_hi"
gf "$C" "dst[5] = access;" "byte5 access"
gf "$C" "dst[6] = (u8)(((flags_nibble & 0x0Fu) << 4) | ((limit >> 16) & 0x0Fu));" "byte6 flags|limit_hi"
gf "$C" "dst[7] = (u8)((base >> 24) & 0xFFu);" "byte7 base_hi"
# Slot assignments match enum order
gf "$C" "VM86_GDT_AR_CODE32, VM86_GDT_FLAGS_32);" "code32 slot"
gf "$C" "VM86_GDT_AR_DATA32, VM86_GDT_FLAGS_32);" "data32 slot"
gf "$C" "VM86_GDT_AR_TSS32, VM86_GDT_FLAGS_TSS);" "tss slot"
gf "$C" "VM86_GDT_AR_CODE64, VM86_GDT_FLAGS_64);" "code64 slot"
# Markers
gf "$C" "vm86: gdt-encode phase=025 status=planned" "m01"
gf "$C" "vm86: gdt-encode slots=0x" "m02"
gf "$C" "vm86: gdt-encode slot0-null ok=0x" "m03"
gf "$C" "vm86: gdt-encode pe-code32 ar=0x" "m04"
gf "$C" "vm86: gdt-encode pe-data32 ar=0x" "m05"
gf "$C" "vm86: gdt-encode v86-tss ar=0x" "m06"
gf "$C" "vm86: gdt-encode ret-code64 ar=0x" "m07"
gf "$C" "vm86: gdt-encode gdtr-limit=0x" "m08"
gf "$C" "vm86: gdt-encode ready-surface=bytes-laid" "m09"
gf "$C" "vm86: gdt-encode pending-surface=lgdt-load,pe32-enter,mode-return" "m10"
gf "$C" "vm86: gdt-encode complete" "m11"
# Ordering
ORDER=$(awk '
  /serial_write\("vm86: gdt-encode phase=025/      && !a { a=NR }
  /serial_write\("vm86: gdt-encode slots=0x/       && !b { b=NR }
  /serial_write\("vm86: gdt-encode slot0-null ok=/ && !c { c=NR }
  /serial_write\("vm86: gdt-encode pe-code32 ar=/  && !d { d=NR }
  /serial_write\("vm86: gdt-encode pe-data32 ar=/  && !e { e=NR }
  /serial_write\("vm86: gdt-encode v86-tss ar=/    && !f { f=NR }
  /serial_write\("vm86: gdt-encode ret-code64 ar=/ && !g { g=NR }
  /serial_write\("vm86: gdt-encode gdtr-limit=/    && !h { h=NR }
  /serial_write\("vm86: gdt-encode complete/       && !k { k=NR }
  END { print (a&&b&&c&&d&&e&&f&&g&&h&&k && a<b && b<c && c<d && d<e && e<f && f<g && g<h && h<k) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
# Probe assertions
for a in \
  "null_ok" \
  "slots == (u32)VM86_GDT_SLOT_COUNT" \
  "ar1 == VM86_GDT_AR_CODE32" \
  "b1  == 0u" \
  "l1  == 0xFFFFFu" \
  "fl1 == VM86_GDT_FLAGS_32" \
  "ar2 == VM86_GDT_AR_DATA32" \
  "ar4 == VM86_GDT_AR_TSS32" \
  "b4  == tss_base" \
  "l4  == (u32)tss_limit" \
  "fl4 == VM86_GDT_FLAGS_TSS" \
  "ar5 == VM86_GDT_AR_CODE64" \
  "fl5 == VM86_GDT_FLAGS_64"; do
  grep -qF "$a" "$C" && pass || fail "probe assertion: $a"
done
# No LGDT INSTRUCTION anywhere — critical safety invariant for this phase.
# Comments/docstrings mentioning LGDT are expected and fine; only reject
# actual inline-assembly lgdt emissions.
if grep -rnE '__asm__[^"]*"[^"]*lgdt|asm[[:space:]]+volatile[^"]*"[^"]*lgdt' stage2/src/ stage2/include/ 2>/dev/null; then
  fail "inline-asm lgdt must not appear in this phase"
else
  pass
fi
# No live call site
if grep -rn "vm86_gdt_encoder_probe\|vm86_gdt_encode(" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null; then
  fail "gdt encoder must not be invoked from live boot path"
else
  pass
fi
gf Makefile "test-vm86-gdt-encode:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
[ "$FAIL" -eq 0 ] && { echo "[PASS] OPENGEM-025 vm86 gdt-encoder gate"; exit 0; }
exit 1
