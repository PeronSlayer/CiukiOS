#!/usr/bin/env bash
# OPENGEM-026 — IDT gate + v86 IRET frame encoder static gate.
set -u
cd "$(dirname "$0")/.."
OK=0; FAIL=0
pass() { OK=$((OK+1)); }
fail() { FAIL=$((FAIL+1)); echo "[FAIL] $1"; }
gf() { grep -qF -- "$2" "$1" && pass || fail "$3 (missing: $2)"; }
H=stage2/include/vm86.h
C=stage2/src/vm86.c
gf "$C" "OPENGEM-026" "sentinel"
# Macros
gf "$H" "#define VM86_IDT_ENTRY_COUNT   256" "entry count"
gf "$H" "#define VM86_IDT_BYTES         (VM86_IDT_ENTRY_COUNT * 8)" "byte size"
gf "$H" "#define VM86_IDT_TYPE_INT32    0x8E" "int32 type"
gf "$H" "#define VM86_IDT_TYPE_TRAP32   0x8F" "trap32 type"
gf "$H" "#define VM86_IRET_FRAME_BYTES  36u" "iret frame bytes"
gf "$H" "#define VM86_EFLAGS_VM         (1u << 17)" "eflags vm"
gf "$H" "#define VM86_EFLAGS_IOPL3      (3u << 12)" "eflags iopl3"
gf "$H" "#define VM86_EFLAGS_IF         (1u << 9)" "eflags if"
gf "$H" "#define VM86_EFLAGS_RESERVED1  (1u << 1)" "eflags reserved1"
# API prototypes
gf "$H" "void vm86_idt_encode_gate(u8 *dst, u32 handler_linear," "encode_gate proto"
gf "$H" "u32 vm86_idt_encode(u8 *out," "encode proto"
gf "$H" "u32 vm86_idt_read_offset(const u8 *buf, u32 vector);" "read_offset proto"
gf "$H" "u16 vm86_idt_read_selector(const u8 *buf, u32 vector);" "read_selector proto"
gf "$H" "u8  vm86_idt_read_type(const u8 *buf, u32 vector);" "read_type proto"
gf "$H" "u32 vm86_iret_encode_frame(u8 *out," "iret_encode proto"
gf "$H" "u32 vm86_iret_read_eip(const u8 *buf);" "read_eip proto"
gf "$H" "u32 vm86_iret_read_cs(const u8 *buf);" "read_cs proto"
gf "$H" "u32 vm86_iret_read_eflags(const u8 *buf);" "read_eflags proto"
gf "$H" "u32 vm86_iret_read_esp(const u8 *buf);" "read_esp proto"
gf "$H" "u32 vm86_iret_read_ss(const u8 *buf);" "read_ss proto"
gf "$H" "int vm86_idt_iret_encoder_probe(void);" "probe proto"
# Impls
gf "$C" "void vm86_idt_encode_gate(u8 *dst, u32 handler_linear," "encode_gate impl"
gf "$C" "u32 vm86_idt_encode(u8 *out," "encode impl"
gf "$C" "u32 vm86_iret_encode_frame(u8 *out," "iret_encode impl"
gf "$C" "int vm86_idt_iret_encoder_probe(void) {" "probe impl"
# IDT gate byte layout (CPU-visible encoding, per Intel SDM)
gf "$C" "dst[0] = (u8)(handler_linear & 0xFFu);" "gate b0"
gf "$C" "dst[1] = (u8)((handler_linear >> 8) & 0xFFu);" "gate b1"
gf "$C" "dst[2] = (u8)(cs_selector & 0xFFu);" "gate b2"
gf "$C" "dst[3] = (u8)((cs_selector >> 8) & 0xFFu);" "gate b3"
gf "$C" "dst[4] = 0;              /* reserved */" "gate b4 reserved"
gf "$C" "dst[5] = type_attr;" "gate b5 type"
gf "$C" "dst[6] = (u8)((handler_linear >> 16) & 0xFFu);" "gate b6"
gf "$C" "dst[7] = (u8)((handler_linear >> 24) & 0xFFu);" "gate b7"
# IRET frame byte layout (Intel SDM Vol.3A §20.2.1)
gf "$C" "vm86_iret_write_dword(out +  0, (u32)ip);" "iret EIP@0"
gf "$C" "vm86_iret_write_dword(out +  4, (u32)cs);" "iret CS@4"
gf "$C" "vm86_iret_write_dword(out +  8, flags);" "iret EFLAGS@8"
gf "$C" "vm86_iret_write_dword(out + 12, (u32)sp);" "iret ESP@12"
gf "$C" "vm86_iret_write_dword(out + 16, (u32)ss);" "iret SS@16"
gf "$C" "vm86_iret_write_dword(out + 20, (u32)es);" "iret ES@20"
gf "$C" "vm86_iret_write_dword(out + 24, (u32)ds);" "iret DS@24"
gf "$C" "vm86_iret_write_dword(out + 28, (u32)fs);" "iret FS@28"
gf "$C" "vm86_iret_write_dword(out + 32, (u32)gs);" "iret GS@32"
# EFLAGS forcing — the critical safety mechanism
gf "$C" "u32 flags = eflags" "eflags force start"
gf "$C" "| VM86_EFLAGS_VM" "eflags VM forced"
gf "$C" "| VM86_EFLAGS_IOPL3" "eflags IOPL3 forced"
gf "$C" "| VM86_EFLAGS_RESERVED1;" "eflags reserved1 forced"
# Markers
gf "$C" "vm86: idt-iret phase=026 status=planned" "m01"
gf "$C" "vm86: idt-iret idt-entries=0x" "m02"
gf "$C" "vm86: idt-iret spurious vec=0x50" "m03"
gf "$C" "vm86: idt-iret gp-vec=0x0D" "m04"
gf "$C" "vm86: idt-iret sw-vec sw20=0x" "m05"
gf "$C" "vm86: idt-iret frame-bytes=0x" "m06"
gf "$C" "vm86: idt-iret frame eip=0x" "m07"
gf "$C" "vm86: idt-iret eflags-bits vm=0x" "m08"
gf "$C" "vm86: idt-iret eflags-force zero-in vm=0x" "m09"
gf "$C" "vm86: idt-iret idtr-limit=0x" "m10"
gf "$C" "vm86: idt-iret ready-surface=idt-bytes,iret-frame,eflags-forced" "m11"
gf "$C" "vm86: idt-iret pending-surface=lidt-load,trap-stubs,iret-exec" "m12"
gf "$C" "vm86: idt-iret complete" "m13"
# Ordering
ORDER=$(awk '
  /serial_write\("vm86: idt-iret phase=026/        && !a { a=NR }
  /serial_write\("vm86: idt-iret idt-entries=0x/   && !b { b=NR }
  /serial_write\("vm86: idt-iret spurious vec=/    && !c { c=NR }
  /serial_write\("vm86: idt-iret gp-vec=0x0D/      && !d { d=NR }
  /serial_write\("vm86: idt-iret sw-vec sw20=/     && !e { e=NR }
  /serial_write\("vm86: idt-iret frame-bytes=0x/   && !f { f=NR }
  /serial_write\("vm86: idt-iret frame eip=0x/     && !g { g=NR }
  /serial_write\("vm86: idt-iret eflags-bits vm=/  && !h { h=NR }
  /serial_write\("vm86: idt-iret eflags-force zero-in/ && !i { i=NR }
  /serial_write\("vm86: idt-iret complete/         && !k { k=NR }
  END { print (a&&b&&c&&d&&e&&f&&g&&h&&i&&k && a<b && b<c && c<d && d<e && e<f && f<g && g<h && h<i && i<k) ? "ok" : "bad" }
' "$C")
[ "$ORDER" = "ok" ] && pass || fail "marker ordering"
# Probe assertions
for a in \
  "entries == (u32)VM86_IDT_ENTRY_COUNT" \
  "frame_bytes == VM86_IRET_FRAME_BYTES" \
  "spur_off == spurious_handler" \
  "spur_sel == cs_pe32" \
  "spur_typ == VM86_IDT_TYPE_INT32" \
  "gp_off == vector_handlers[6]" \
  "gp_sel == cs_pe32" \
  "f_eip == 0x0100u" \
  "f_cs  == 0x1000u" \
  "f_esp == 0xFFFEu" \
  "f_ss  == 0x1000u" \
  "vm_bit && iopl3 && if_bit && r1_bit" \
  "zvm && ziopl3"; do
  grep -qF "$a" "$C" && pass || fail "probe assertion: $a"
done
# No inline-asm lidt / iret instruction this phase (excluding the
# pre-existing stage2 host IDT install in interrupts.c, which has
# been in place since long before OPENGEM and is unrelated to v86).
if grep -rnE '__asm__[^"]*"[^"]*(lidt|iret)|asm[[:space:]]+volatile[^"]*"[^"]*(lidt|iret)' stage2/src/ stage2/include/ 2>/dev/null | grep -vE '(^|/)interrupts\.c:'; then
  fail "inline-asm lidt/iret must not appear in this phase"
else
  pass
fi
# No live call site
if grep -rn "vm86_idt_iret_encoder_probe\|vm86_idt_encode(\|vm86_iret_encode_frame(" stage2/src/shell.c stage2/src/stage2.c 2>/dev/null; then
  fail "idt/iret encoders must not be invoked from live boot path"
else
  pass
fi
gf Makefile "test-vm86-idt-iret:" "makefile target"
echo
echo "${OK} OK / ${FAIL} FAIL"
[ "$FAIL" -eq 0 ] && { echo "[PASS] OPENGEM-026 vm86 idt-iret-encoder gate"; exit 0; }
exit 1
