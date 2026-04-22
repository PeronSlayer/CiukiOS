#!/usr/bin/env bash
# OPENGEM-032 static gate for the v8086 IDT shim.
# Validates scaffolding without executing any mode switch.
set -u
cd "$(dirname "$0")/.."

OK=0
FAIL=0
pass() { OK=$((OK+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

# --- 1. Sentinels ----------------------------------------------------
grep -q '^__attribute__((used)) static const char vm86_idt_shim_sentinel\[\] = "OPENGEM-032";' stage2/src/vm86.c \
    && pass || fail "OPENGEM-032 sentinel missing in vm86.c"
grep -q '"OPENGEM-032"' stage2/src/vm86_trap_stubs.S \
    && pass || fail "OPENGEM-032 sentinel missing in vm86_trap_stubs.S"
grep -q '#define VM86_IDT_SHIM_SENTINEL[[:space:]]*0x0320u' stage2/include/vm86.h \
    && pass || fail "VM86_IDT_SHIM_SENTINEL define missing"
grep -q '#define VM86_IDT_SHIM_STUB_COUNT[[:space:]]*11u' stage2/include/vm86.h \
    && pass || fail "VM86_IDT_SHIM_STUB_COUNT define missing"

# --- 2. Header API ---------------------------------------------------
for sym in \
    'int vm86_idt_shim_build(void);' \
    'int vm86_idt_shim_idtr_image(u16 \*limit_out, u64 \*base_out);' \
    'int vm86_idt_shim_verify(void);' \
    'int vm86_idt_shim_probe(void);' \
    'typedef struct __attribute__((packed)) vm86_idtr_image'
do
    grep -q "$sym" stage2/include/vm86.h && pass || fail "header API missing: $sym"
done

# --- 3. Trap stub labels (11 globals) --------------------------------
for lbl in de ud nm ts np ss gp pf sw20 sw21 unexpected; do
    grep -q "^VM86_TRAP_STUB vm86_trap_stub_$lbl\$" stage2/src/vm86_trap_stubs.S \
        && pass || fail "trap stub label missing: vm86_trap_stub_$lbl"
    grep -q "extern char vm86_trap_stub_$lbl;" stage2/src/vm86.c \
        && pass || fail "C extern missing: vm86_trap_stub_$lbl"
done

# --- 4. Stub body: hlt + jmp . (no iret/iretd/lidt) ------------------
grep -q '\.byte 0xF4' stage2/src/vm86_trap_stubs.S \
    && pass || fail "stub body missing hlt opcode"
grep -q '\.byte 0xEB, 0xFE' stage2/src/vm86_trap_stubs.S \
    && pass || fail "stub body missing jmp-self opcode"

if grep -qE '\blidt\b' stage2/src/vm86_trap_stubs.S; then
    fail "stub file introduces LIDT (out of scope for 032)"
else pass; fi
if grep -qE '\biret\b|\biretd\b|\biretq\b' stage2/src/vm86_trap_stubs.S; then
    fail "stub file introduces IRET variants (out of scope for 032)"
else pass; fi

# --- 5. No LIDT anywhere in vm86.c (code, not string literals) -------
if grep -nE '\blidt\b' stage2/src/vm86.c | grep -vE 'serial_write|/\*|\*|//'; then
    fail "vm86.c introduces LIDT opcode (out of scope for 032)"
else pass; fi

# --- 6. Boot-path isolation: new APIs are not invoked by boot code. --
#     Only defined in vm86.c/vm86.h, declared in header, and referenced
#     by this gate script. No other caller is allowed for now.
for fn in vm86_idt_shim_build vm86_idt_shim_idtr_image vm86_idt_shim_verify vm86_idt_shim_probe; do
    callers=$(grep -RIln --include='*.c' --include='*.S' "$fn" stage2 | grep -v 'stage2/src/vm86.c' || true)
    if [ -n "$callers" ]; then
        fail "unexpected caller of $fn: $callers"
    else pass; fi
done

# --- 7. vm86_switch.S and vm86_snapshot.S untouched by 032 -----------
for f in stage2/src/vm86_switch.S stage2/src/vm86_snapshot.S; do
    if grep -qE '\blidt\b|vm86_trap_stub_|OPENGEM-032' "$f"; then
        fail "032 leaked into $f"
    else pass; fi
done

# --- 8. IDT entry count 256 ------------------------------------------
grep -q '#define VM86_IDT_ENTRY_COUNT[[:space:]]*256' stage2/include/vm86.h \
    && pass || fail "VM86_IDT_ENTRY_COUNT must remain 256"

# --- 9. CS selector: compat PE 32-bit code ---------------------------
grep -q 'u16 cs_sel   = (u16)(VM86_GDT_PE_CODE32 << 3);' stage2/src/vm86.c \
    && pass || fail "shim CS selector must be VM86_GDT_PE_CODE32"

# --- 10. Probe surface markers ---------------------------------------
for mk in \
    '"vm86: idt-shim sentinel=0x"' \
    '"vm86: idt-shim image.base=0x"' \
    '"vm86: idt-shim ready-surface=build,verify\\n"' \
    '"vm86: idt-shim pending-surface=lidt,iretd\\n"' \
    '"vm86: idt-shim probe complete\\n"'
do
    grep -q "$mk" stage2/src/vm86.c && pass || fail "probe marker missing: $mk"
done

# --- 11. Build (ELF link must succeed) -------------------------------
if [ ! -f build/stage2.elf ]; then
    fail "build/stage2.elf missing: run CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh first"
else pass; fi

# --- 12. Makefile target registered ----------------------------------
grep -q '^test-vm86-idt-shim:' Makefile && pass || fail "test-vm86-idt-shim target missing"

echo
echo "[summary] $OK OK / $FAIL FAIL"
if [ $FAIL -eq 0 ]; then
    echo "[PASS] OPENGEM-032 vm86 idt-shim gate"
    exit 0
else
    exit 1
fi
