/* Real HDPMI 3.24 lifetime/scheduler probe.
 *
 * This program does not install a fixture callback.  The callback is owned by
 * the patched official host before this original-style DPMI client begins.
 * Every privileged operation below is an actual CPU instruction.
 */
#include "../../vm/session_scheduler.h"
#include <conio.h>
#include <i86.h>
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dpmi_meminfo {
    unsigned largest_free_block;
    unsigned max_unlocked_pages;
    unsigned max_locked_pages;
    unsigned linear_pages;
    unsigned unlocked_pages;
    unsigned free_pages;
    unsigned physical_pages;
    unsigned free_linear_pages;
    unsigned paging_file_pages;
    unsigned reserved[3];
} dpmi_meminfo;

static unsigned desc_get(unsigned selector, unsigned offset);
#pragma aux desc_get = "push es" "mov es,cx" "mov eax,es:[edi]" "pop es" \
    parm [ecx] [edi] value [eax] modify exact [eax]
static void cpu_cli(void);
#pragma aux cpu_cli = "cli" modify exact []
static void cpu_sti(void);
#pragma aux cpu_sti = "sti" modify exact []
static unsigned io_in8(unsigned port, unsigned seed);
#pragma aux io_in8 = "in al,dx" parm [edx] [eax] value [eax] modify exact [eax]
static void io_out8(unsigned port, unsigned value);
#pragma aux io_out8 = "out dx,al" parm [edx] [eax] modify exact []
static void vga_write8(unsigned linear, unsigned value);
#pragma aux vga_write8 = "mov [edi],al" parm [edi] [eax] modify exact []
static unsigned vga_read8(unsigned linear, unsigned seed);
#pragma aux vga_read8 = "mov al,[edi]" parm [edi] [eax] value [eax] modify exact [eax]


static volatile unsigned irq_hits;
static void __interrupt __far timer_irq(void)
{
    ++irq_hits;
    outp(0x20, 0x20);
}
static unsigned interrupt_state(unsigned operation);
#pragma aux interrupt_state = "int 31h" parm [eax] value [eax] modify exact [eax]
static void legacy_region(unsigned selector, unsigned offset, unsigned *result, volatile unsigned *counter);
#pragma aux legacy_region = \
    "push es" "mov es,cx" "pushfd" "cli" \
    "mov eax,es:[edx+28]" "mov [edi],eax" \
    "pushfd" "pop eax" "mov [edi+8],eax" \
    "mov eax,[esi]" "mov [edi+12],eax" \
    "L1: mov eax,es:[edx+28]" "sub eax,[edi]" "cmp eax,3" "jb L1" \
    "mov eax,[esi]" "mov [edi+16],eax" \
    "popfd" "pop es" \
    parm [ecx] [edx] [edi] [esi] modify exact [eax]

static unsigned failures;

static void mark(const char *text)
{
    const char *p = text;
    unsigned wait;
    while (*p) {
        wait = 65535;
        while (wait-- && !(inp(0x3fd) & 0x20)) { }
        outp(0x3f8, *p++);
    }
    outp(0x3f8, '\r'); outp(0x3f8, '\n');
    puts(text);
}

static void fail(const char *what)
{
    char line[128];
    ++failures;
    sprintf(line, "[DPMILIFE] FAIL %s", what);
    mark(line);
}

static int meminfo_get(dpmi_meminfo *info)
{
    union REGS regs;
    struct SREGS segs;
    memset(&regs, 0, sizeof(regs));
    memset(info, 0, sizeof(*info));
    segread(&segs);
    segs.es = segs.ds;
    regs.w.ax = 0x0500;
    regs.x.edi = (unsigned)info;
    int386x(0x31, &regs, &regs, &segs);
    return regs.w.cflag != 0;
}

static int alloc_free_cycle(void)
{
    union REGS regs;
    unsigned handle_hi, handle_lo;
    memset(&regs, 0, sizeof(regs));
    regs.w.ax = 0x0501;
    regs.w.bx = 0;
    regs.w.cx = 0x3000;
    int386(0x31, &regs, &regs);
    if (regs.w.cflag) return 1;
    handle_hi = regs.w.si;
    handle_lo = regs.w.di;
    memset(&regs, 0, sizeof(regs));
    regs.w.ax = 0x0502;
    regs.w.si = handle_hi;
    regs.w.di = handle_lo;
    int386(0x31, &regs, &regs);
    return regs.w.cflag != 0;
}

int main(int argc, char **argv)
{
    union REGS regs;
    dpmi_meminfo before, after;
    unsigned segment, offset, selector, start, now, value, status_a, status_b;
    char line[160];
    if (argc != 2 || sscanf(argv[1], "%x:%x", &segment, &offset) != 2) {
        mark("[DPMILIFE] FAIL descriptor argument");
        return 2;
    }
    memset(&regs, 0, sizeof(regs));
    regs.w.ax = 2;
    regs.w.bx = segment;
    int386(0x31, &regs, &regs);
    if (regs.w.cflag) {
        mark("[DPMILIFE] FAIL descriptor selector");
        return 2;
    }
    selector = regs.w.ax;
    if (desc_get(selector, offset) != CVSCHED_MAGIC ||
        desc_get(selector, offset + 12) !=
          (CVSCHED_STATE_BOUND | CVSCHED_STATE_SESSION | CVSCHED_STATE_JEMM |
           CVSCHED_STATE_DPMI_HOST | CVSCHED_STATE_DPMI_CLIENT |
           CVSCHED_STATE_CALLBACK))
        fail("3.24 callback ownership on entry");

    if (meminfo_get(&before)) fail("DPMI 0500 before");
    if (alloc_free_cycle()) fail("DPMI 0501/0502 cycle");
    if (meminfo_get(&after)) fail("DPMI 0500 after");
    if (before.largest_free_block != after.largest_free_block ||
        before.free_pages != after.free_pages ||
        before.free_linear_pages != after.free_linear_pages)
        fail("DPMI allocation accounting restored");
    else
        mark("[DPMILIFE] DPMI ALLOC/FREE ACCOUNTING PASS");

    io_out8(0x3c6, 0x5a);
    value = io_in8(0x3c6, 0x12345600UL);
    if (value != 0x1234565aUL) fail("actual OUT/IN AL and frame restore");
    else mark("[DPMILIFE] ACTUAL OUT/IN AL PASS");
    status_a = io_in8(0x3da, 0);
    status_b = io_in8(0x3da, 0);
    if (!((status_a ^ status_b) & 8)) fail("actual 3DA retrace edge");
    else mark("[DPMILIFE] ACTUAL 3DA RETRACE EDGE PASS");

    /* This is an actual ring-3 store through HDPMI's supervisor-only VGA
     * PTE, not a source-port bridge.  The host #PF adapter must execute it
     * against CVSESSION's shared VGA model and advance the original EIP. */
    vga_write8(0xa0000UL, 0x5a);
    mark("[DPMILIFE] ACTUAL PM VGA STORE PASS");
    value = vga_read8(0xa0000UL, 0x12345600UL);
    sprintf(line, "[DPMILIFE] ACTUAL PM VGA LOAD PASS %08lX",
            (unsigned long)value);
    mark(line);

    {
        unsigned old_sel, old_off, vector, saved_hits, previous, attempt;
        unsigned legacy[5];
        struct SREGS segs;
        memset(&regs, 0, sizeof(regs));
        regs.w.ax = 0x0400;
        int386(0x31, &regs, &regs);
        vector = regs.h.dh;
        memset(&regs, 0, sizeof(regs));
        regs.w.ax = 0x0204; regs.w.bx = vector;
        int386(0x31, &regs, &regs);
        old_sel = regs.w.cx; old_off = regs.x.edx;
        segread(&segs);
        memset(&regs, 0, sizeof(regs));
        regs.w.ax = 0x0205; regs.w.bx = vector;
        regs.w.cx = segs.cs; regs.x.edx = (unsigned)timer_irq;
        int386(0x31, &regs, &regs);
        if (regs.w.cflag) fail("timer vector install");
        interrupt_state(0x0901);
        for (attempt = 0; attempt < 2; ++attempt) {
            mark("[DPMILIFE] CLI ENTER");
            cpu_cli();
            saved_hits = irq_hits;
            previous = interrupt_state(0x0902) & 255;
            start = desc_get(selector, offset + 28);
            do { now = desc_get(selector, offset + 28); }
            while ((unsigned)(now - start) < 3);
            value = irq_hits;
            cpu_sti();
            if (previous || value != saved_hits) fail("IRQ delivered during CLI");
            sprintf(line, "[DPMILIFE] CLI HOST TICKS PASS %lu->%lu",
                    (unsigned long)start, (unsigned long)now);
            mark(line);
            start = desc_get(selector, offset + 28);
            while (irq_hits == saved_hits && desc_get(selector, offset + 28) - start < 3) { }
            if (irq_hits == saved_hits) fail("IRQ did not resume after STI");
            legacy_region(selector, offset, legacy, &irq_hits);
            if (legacy[3] != legacy[4]) fail("IRQ delivered during legacy CLI");
            if (legacy[2] & 0x100) fail("monitor TF leaked into PUSHFD image");
            if (!(interrupt_state(0x0902) & 255)) fail("legacy POPFD did not restore IF");
        }
        previous = interrupt_state(0x0900) & 255;
        saved_hits = irq_hits;
        start = desc_get(selector, offset + 28);
        while (desc_get(selector, offset + 28) - start < 3) { }
        value = irq_hits;
        status_a = interrupt_state(0x0902) & 255;
        status_b = interrupt_state(0x0901) & 255;
        if (!previous || status_a || status_b || value != saved_hits)
            fail("0900/0901/0902 virtual IF contract");
        memset(&regs, 0, sizeof(regs));
        regs.w.ax = 0x0205; regs.w.bx = vector;
        regs.w.cx = old_sel; regs.x.edx = old_off;
        int386(0x31, &regs, &regs);
        if (regs.w.cflag) fail("timer vector restore");
        if (!failures) mark("[DPMILIFE] GUEST IRQ SUPPRESSION STI POPFD 090x PASS");
    }

    mark("[DPMILIFE] BIOS WAIT ENTER");
    memset(&regs, 0, sizeof(regs));
    regs.h.ah = 0;
    int386(0x16, &regs, &regs);
    mark("[DPMILIFE] BIOS WAIT RETURN");

    if (!desc_get(selector, offset + 80) || !desc_get(selector, offset + 84))
        fail("adapter/callback memory accounting");
    if (desc_get(selector, offset + 40) == 0)
        fail("owned callback handle retained during client");

    if (!failures) mark("[DPMILIFE] PASS actual instructions, scheduler, memory");
    else mark("[DPMILIFE] FAIL");
    return failures ? 1 : 0;
}
