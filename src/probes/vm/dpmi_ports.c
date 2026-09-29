/* DPMIPORT.EXE: protected-mode VGA register access through the session-bound
 * HDPMI host (DOS/4GW client). After a BIOS mode 13h set it reads and writes
 * sequencer, graphics and CRTC registers with byte and 16-bit port cycles,
 * exactly as Mode-Y games (DOOM) do, and reports every value on COM1:
 *   [DPMIPORT] <name> <value> <expected>
 *   [DPMIPORT] PASS | FAIL <count>
 */
#include <conio.h>
#include <i86.h>
#include <string.h>

static void serial_char(int c)
{
    unsigned wait = 60000;
    while (wait-- && !(inp(0x3fd) & 0x20)) { }
    outp(0x3f8, c);
}

static void serial_text(const char *s)
{
    while (*s) serial_char(*s++);
}

static void serial_hex(unsigned value, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    while (digits--) serial_char(hex[(value >> (digits * 4)) & 15]);
}

/* Exact instruction forms, independent of the compiler's choices. */
void store_reg8(volatile unsigned char *address, unsigned value);
#pragma aux store_reg8 = "mov [edi],al" parm [edi] [eax] modify exact []
void store_imm8_41(volatile unsigned char *address);
#pragma aux store_imm8_41 = "mov byte ptr [edi],41h" parm [edi] modify exact []
unsigned load_movzx8(volatile unsigned char *address);
#pragma aux load_movzx8 = "movzx eax,byte ptr [edi]" parm [edi] value [eax] modify exact [eax]
unsigned load_mov8(volatile unsigned char *address);
#pragma aux load_mov8 = "xor eax,eax" "mov al,[edi]" parm [edi] value [eax] modify exact [eax]

static int failures;

static void check(const char *name, unsigned value, unsigned expected, int digits)
{
    serial_text("[DPMIPORT] ");
    serial_text(name);
    serial_char(' ');
    serial_hex(value, digits);
    serial_char(' ');
    serial_hex(expected, digits);
    serial_text(value == expected ? "\r\n" : " MISMATCH\r\n");
    if (value != expected) ++failures;
}

static unsigned indexed(unsigned port, unsigned index)
{
    outp(port, index);
    return inp(port + 1);
}

int main(void)
{
    union REGS regs;
    serial_text("[DPMIPORT] START\r\n");
    memset(&regs, 0, sizeof(regs));
    regs.w.ax = 0x13;
    int386(0x10, &regs, &regs);

    check("SEQ4", indexed(0x3c4, 4), 0x0e, 2);
    check("GC5", indexed(0x3ce, 5), 0x40, 2);
    check("GC6", indexed(0x3ce, 6), 0x05, 2);
    check("CRTC17", indexed(0x3d4, 0x17), 0xa3, 2);
    check("CRTC14", indexed(0x3d4, 0x14), 0x40, 2);
    check("SEQ-INDEX", (outp(0x3c4, 2), inp(0x3c4)), 0x02, 2);

    /* 16-bit OUT: index in AL, data in AH (DOOM: outpw(SC_INDEX, 0xf02)). */
    outpw(0x3c4, 0x0302);
    check("OUTW-MAPMASK", indexed(0x3c4, 2), 0x03, 2);
    outpw(0x3d4, 0x280c);
    check("OUTW-START", indexed(0x3d4, 0x0c), 0x28, 2);
    outpw(0x3d4, 0x000c);
    /* 16-bit IN: index register then data register. */
    outp(0x3c4, 4);
    check("INW-SEQ", inpw(0x3c4), 0x0e04, 4);

    /* DOOM's Mode-Y sequence, then the values it must leave. */
    outp(0x3c4, 4);
    outp(0x3c5, (inp(0x3c5) & ~8) | 4);
    outp(0x3ce, 5);
    outp(0x3cf, inp(0x3cf) & ~0x13);
    outp(0x3ce, 6);
    outp(0x3cf, inp(0x3cf) & ~2);
    outp(0x3d4, 0x14);
    outp(0x3d5, inp(0x3d5) & ~0x40);
    outp(0x3d4, 0x17);
    outp(0x3d5, inp(0x3d5) | 0x40);
    check("MODEY-SEQ4", indexed(0x3c4, 4), 0x06, 2);
    check("MODEY-GC5", indexed(0x3ce, 5), 0x40, 2);
    check("MODEY-GC6", indexed(0x3ce, 6), 0x05, 2);
    check("MODEY-CRTC14", indexed(0x3d4, 0x14), 0x00, 2);
    check("MODEY-CRTC17", indexed(0x3d4, 0x17), 0xe3, 2);

    /* Mode-Y memory: one plane per write (map mask), read back per plane
     * through Read Map Select, with 8/16/32-bit stores as DOOM uses. */
    {
        volatile unsigned char *vram = (volatile unsigned char *)0xa0000;
        unsigned p, v;
        for (p = 0; p < 4; ++p) {
            outp(0x3c4, 2); outp(0x3c5, 1 << p);
            vram[0x10] = (unsigned char)(0x10 + p);
        }
        outp(0x3c4, 2); outp(0x3c5, 4);
        *(volatile unsigned short *)(vram + 0x20) = 0x2b2a;
        outp(0x3c4, 2); outp(0x3c5, 8);
        *(volatile unsigned long *)(vram + 0x31) = 0x3d3c3b3aUL;
        for (p = 0; p < 4; ++p) {
            outp(0x3ce, 4); outp(0x3cf, p);
            v = vram[0x10];
            check(p == 0 ? "PLANE0" : p == 1 ? "PLANE1" : p == 2 ? "PLANE2" : "PLANE3", v, 0x10 + p, 2);
        }
        outp(0x3ce, 4); outp(0x3cf, 2);
        check("WORD-PLANE2", *(volatile unsigned short *)(vram + 0x20), 0x2b2a, 4);
        outp(0x3ce, 4); outp(0x3cf, 1);
        check("WORD-PLANE1-UNTOUCHED", vram[0x20], 0x00, 2);
        outp(0x3ce, 4); outp(0x3cf, 3);
        check("DWORD-PLANE3", *(volatile unsigned long *)(vram + 0x31), 0x3d3c3b3aUL, 8);
    }

    /* Step by step: computed value, then immediate read-back. */
    {
        unsigned v;
        regs.w.ax = 0x13;
        int386(0x10, &regs, &regs);
        outp(0x3c4, 4);
        v = (inp(0x3c5) & ~8) | 4;
        check("STEP-SEQ4-VALUE", v, 0x06, 2);
        outp(0x3c5, v);
        check("STEP-SEQ4-AFTER", indexed(0x3c4, 4), 0x06, 2);
        outp(0x3ce, 6);
        v = inp(0x3cf) & ~2;
        check("STEP-GC6-VALUE", v, 0x05, 2);
        outp(0x3cf, v);
        check("STEP-GC6-AFTER", indexed(0x3ce, 6), 0x05, 2);
        outp(0x3c4, 4);
        outp(0x3c5, 0x06);
        check("CONST-SEQ4", indexed(0x3c4, 4), 0x06, 2);
        outp(0x3ce, 6);
        outp(0x3cf, 0x05);
        check("CONST-GC6", indexed(0x3ce, 6), 0x05, 2);
        outp(0x3ce, 6);
        outp(0x3cf, 0x01);
        check("CONST-GC6-01", indexed(0x3ce, 6), 0x01, 2);
    }

    /* Raw reads right after an index write, repeated: which ones arrive? */
    {
        int round;
        for (round = 0; round < 3; ++round) {
            unsigned a, b, c, d;
            regs.w.ax = 0x13;
            int386(0x10, &regs, &regs);
            outp(0x3c4, 4);
            a = inp(0x3c5);
            outp(0x3ce, 6);
            b = inp(0x3cf);
            outp(0x3ce, 5);
            c = inp(0x3cf);
            outp(0x3c4, 4);
            d = inp(0x3c5);
            check("RAW-SEQ4", a, 0x0e, 2);
            check("RAW-GC6", b, 0x05, 2);
            check("RAW-GC5", c, 0x40, 2);
            check("RAW-SEQ4B", d, 0x0e, 2);
        }
    }

    /* Last accesses before exit (the host's diagnostic ring keeps 16). */
    regs.w.ax = 0x13;
    int386(0x10, &regs, &regs);
    {
        unsigned r1, r2, r3;
        volatile unsigned long spin;
        outp(0x3c4, 4);
        outp(0x3c5, 0x06);
        r1 = inp(0x3c5);
        r2 = indexed(0x3c4, 4);
        for (spin = 0; spin < 40000000UL; ++spin) { }
        r3 = indexed(0x3c4, 4);
        check("TIME-SEQ4-IMMEDIATE", r1, 0x06, 2);
        check("TIME-SEQ4-REINDEX", r2, 0x06, 2);
        check("TIME-SEQ4-LATER", r3, 0x06, 2);
    }

    /* One instruction form at a time: plane 2 written, planes 1/2 read. */
    {
        volatile unsigned char *vram = (volatile unsigned char *)0xa0000;
        outp(0x3c4, 2); outp(0x3c5, 15);
        store_imm8_41(vram + 0x50);                 /* 41h in every plane */
        outp(0x3c4, 2); outp(0x3c5, 4);
        store_reg8(vram + 0x50, 0x5a);              /* 5Ah in plane 2 only */
        outp(0x3ce, 4); outp(0x3cf, 2);
        check("FORM-MOV8-PLANE2", load_mov8(vram + 0x50), 0x5a, 2);
        check("FORM-MOVZX-PLANE2", load_movzx8(vram + 0x50), 0x5a, 2);
        outp(0x3ce, 4); outp(0x3cf, 1);
        check("FORM-MOV8-PLANE1", load_mov8(vram + 0x50), 0x41, 2);
        check("FORM-MOVZX-PLANE1", load_movzx8(vram + 0x50), 0x41, 2);
    }

    /* Final accesses (the host keeps its last 16 port cycles for diagnosis). */
    {
        volatile unsigned char *vram = (volatile unsigned char *)0xa0000;
        unsigned v1, v3;
        outp(0x3c4, 2); outp(0x3c5, 1);
        vram[0x40] = 0x41;
        outp(0x3c4, 2); outp(0x3c5, 8);
        vram[0x40] = 0x48;
        outp(0x3ce, 4); outp(0x3cf, 1);
        v1 = vram[0x40];
        outp(0x3ce, 4); outp(0x3cf, 3);
        v3 = vram[0x40];
        check("LAST-PLANE0", v1, 0x00, 2);
        check("LAST-PLANE3", v3, 0x48, 2);
    }
    serial_text(failures ? "[DPMIPORT] FAIL " : "[DPMIPORT] PASS ");
    serial_hex(failures, 2);
    serial_text("\r\n");
    return failures ? 1 : 0;
}
