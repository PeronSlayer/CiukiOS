/*
 * PMIRQSB.EXE - DOS/4GW protected-mode Sound Blaster IRQ/DMA probe.
 *
 * Build with OpenWatcom:
 *   wcl386 -zq -bt=dos -l=dos4g -fe=build/full/obj/pmirqsb.exe src/probes/pmirqsb/pmirqsb.c
 *
 * This is deliberately a probe, not a Stage1 fix. It installs a protected-mode
 * IRQ handler through the DOS/4GW-compatible vector API, starts one SB DMA
 * playback, and reports whether the IRQ reached protected-mode code.
 */

#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COM1_BASE 0x3F8
#define SB_BASE   0x220
#define SB_DMA    1
#define DMA_LEN   4096

static volatile unsigned irq_hits;
static volatile unsigned timer_hits;
static volatile unsigned ack_after_eoi;
static volatile unsigned task_mode_enabled;
static volatile unsigned task_dma_busy;
static volatile unsigned task_dma_irqs;
static unsigned old_irq_sel;
static unsigned long old_irq_off;
static unsigned old_timer_sel;
static unsigned long old_timer_off;
static unsigned sb_int_no;
static unsigned timer_int_no;
static unsigned master_pic_base;

static void serial_putc(char c)
{
    unsigned spin = 0xffff;
    while (spin-- && !(inp(COM1_BASE + 5) & 0x20)) {
    }
    outp(COM1_BASE, (unsigned char)c);
}

static void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

static void mark(const char *s)
{
    puts(s);
    serial_puts(s);
    serial_puts("\n");
}

static void mark_hex8(const char *name, unsigned value)
{
    printf("[PMIRQSB] %s=0x%02X\n", name, value & 0xff);
    serial_puts("[PMIRQSB] ");
    serial_puts(name);
    serial_puts("=0x");
    serial_putc("0123456789ABCDEF"[(value >> 4) & 0x0f]);
    serial_putc("0123456789ABCDEF"[value & 0x0f]);
    serial_puts("\n");
}

static void mark_hex16(const char *name, unsigned value)
{
    printf("[PMIRQSB] %s=0x%04X\n", name, value & 0xffff);
    serial_puts("[PMIRQSB] ");
    serial_puts(name);
    serial_puts("=0x");
    serial_putc("0123456789ABCDEF"[(value >> 12) & 0x0f]);
    serial_putc("0123456789ABCDEF"[(value >> 8) & 0x0f]);
    serial_putc("0123456789ABCDEF"[(value >> 4) & 0x0f]);
    serial_putc("0123456789ABCDEF"[value & 0x0f]);
    serial_puts("\n");
}

static int dsp_wait_write(void)
{
    unsigned i;
    for (i = 0; i < 0xffff; ++i) {
        if (!(inp(SB_BASE + 0x0c) & 0x80)) {
            return 1;
        }
    }
    return 0;
}

static int dsp_write(unsigned char v)
{
    if (!dsp_wait_write()) {
        return 0;
    }
    outp(SB_BASE + 0x0c, v);
    return 1;
}

static int dsp_read(unsigned char *v)
{
    unsigned i;
    for (i = 0; i < 0xffff; ++i) {
        if (inp(SB_BASE + 0x0e) & 0x80) {
            *v = inp(SB_BASE + 0x0a);
            return 1;
        }
    }
    return 0;
}

static int dsp_reset(void)
{
    unsigned i;
    unsigned char v;
    outp(SB_BASE + 0x06, 1);
    for (i = 0; i < 1000; ++i) {
    }
    outp(SB_BASE + 0x06, 0);
    if (!dsp_read(&v)) {
        return 0;
    }
    return v == 0xaa;
}

static void sb_mixer_setup(void)
{
    outp(SB_BASE + 0x04, 0x80);
    outp(SB_BASE + 0x05, 0x04); /* IRQ7 */
    outp(SB_BASE + 0x04, 0x81);
    outp(SB_BASE + 0x05, 0x22); /* DMA1 + HDMA5 */
}

static void sb_mixer_irq(unsigned irq)
{
    unsigned char mask = 0x04;     /* IRQ7 */
    if (irq == 5) {
        mask = 0x02;
    }
    outp(SB_BASE + 0x04, 0x80);
    outp(SB_BASE + 0x05, mask);
}

static void unmask_irq(unsigned irq)
{
    outp(0x21, inp(0x21) & (unsigned char)~(1U << irq));
}

static unsigned pic_read_irr(void)
{
    outp(0x20, 0x0a);
    return inp(0x20);
}

static unsigned pic_read_isr(void)
{
    outp(0x20, 0x0b);
    return inp(0x20);
}

static unsigned dma1_count_left(void)
{
    unsigned lo;
    unsigned hi;
    outp(0x0c, 0x00);
    lo = inp(0x03);
    hi = inp(0x03);
    return lo | (hi << 8);
}

static void program_dma1(unsigned long phys, unsigned count, unsigned auto_init)
{
    unsigned len = count - 1;

    outp(0x0a, 0x05);              /* mask DMA1 */
    outp(0x0c, 0x00);              /* clear flip-flop */
    outp(0x0b, auto_init ? 0x59 : 0x49); /* single, read, channel 1 */
    outp(0x02, (unsigned char)(phys & 0xff));
    outp(0x02, (unsigned char)((phys >> 8) & 0xff));
    outp(0x83, (unsigned char)((phys >> 16) & 0xff));
    outp(0x03, (unsigned char)(len & 0xff));
    outp(0x03, (unsigned char)((len >> 8) & 0xff));
    outp(0x0a, 0x01);              /* unmask DMA1 */
}

static void __interrupt __far sb_irq_handler(void)
{
    ++irq_hits;
    if (ack_after_eoi) {
        outp(0x20, 0x20);          /* master PIC EOI */
        inp(SB_BASE + 0x0e);       /* ACK 8-bit DSP IRQ */
    } else {
        inp(SB_BASE + 0x0e);
        outp(0x20, 0x20);
    }
    if (task_mode_enabled && task_dma_busy) {
        task_dma_busy = 0;
        ++task_dma_irqs;
    }
}

static void __interrupt __far timer_irq_handler(void)
{
    ++timer_hits;
    outp(0x20, 0x20);
}

static int alloc_dos_buffer(unsigned paras, unsigned *rm_seg, unsigned *pm_sel)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;
    r.w.bx = paras;
    int386(0x31, &r, &r);
    if (r.w.cflag) {
        return 0;
    }
    *rm_seg = r.w.ax;
    *pm_sel = r.w.dx;
    return 1;
}

static void free_dos_buffer(unsigned pm_sel)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0101;
    r.w.dx = pm_sel;
    int386(0x31, &r, &r);
}

static int get_pm_vector(unsigned int_no, unsigned *sel, unsigned long *off)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0204;
    r.h.bl = (unsigned char)int_no;
    int386(0x31, &r, &r);
    if (r.w.cflag) {
        return 0;
    }
    *sel = r.w.cx;
    *off = r.x.edx;
    return 1;
}

static int set_pm_vector(unsigned int_no, unsigned sel, unsigned long off)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0205;
    r.h.bl = (unsigned char)int_no;
    r.w.cx = sel;
    r.x.edx = off;
    int386(0x31, &r, &r);
    return !r.w.cflag;
}

static unsigned get_master_pic_base(void)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0400;
    int386(0x31, &r, &r);
    return r.h.dh;
}

static unsigned enable_virtual_interrupts(void)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0901;
    int386(0x31, &r, &r);
    return r.h.al;
}

static void fill_sample(unsigned char __far *buf)
{
    unsigned i;
    for (i = 0; i < DMA_LEN; ++i) {
        buf[i] = (unsigned char)((i & 0x20) ? 0xd0 : 0x30);
    }
}

static int run_variant(const char *label, unsigned irq, unsigned auto_init, unsigned eoi_first)
{
    unsigned rm_seg = 0;
    unsigned pm_sel = 0;
    unsigned char __far *buf;
    unsigned alloc_size = DMA_LEN + 0x10000UL;
    unsigned dma_off = 0;
    unsigned long phys;

    printf("[PMIRQSB] CASE %s IRQ%u %s %s\n",
           label, irq, auto_init ? "AUTO" : "SINGLE",
           eoi_first ? "EOI_FIRST" : "ACK_FIRST");

    if (!alloc_dos_buffer((alloc_size + 15) / 16, &rm_seg, &pm_sel)) {
        mark("[PMIRQSB] DOSMEM FAIL");
        return 3;
    }

    phys = ((unsigned long)rm_seg) << 4;
    if (((phys & 0xffffUL) + DMA_LEN - 1) >= 0x10000UL) {
        dma_off = (unsigned)(0x10000UL - (phys & 0xffffUL));
        phys += dma_off;
    }
    buf = (unsigned char __far *)MK_FP(pm_sel, dma_off);
    fill_sample(buf);
    mark_hex16("DMA_PHYS_LO", (unsigned)(phys & 0xffff));
    mark_hex8("DMA_PAGE", (unsigned)((phys >> 16) & 0xff));
    mark("[PMIRQSB] DMA_BOUNDARY OK");

    timer_int_no = master_pic_base;
    sb_int_no = timer_int_no + irq;
    printf("[PMIRQSB] PMINT 0x%02X\n", sb_int_no);

    if (!get_pm_vector(timer_int_no, &old_timer_sel, &old_timer_off)) {
        mark("[PMIRQSB] TIMER GET FAIL");
        free_dos_buffer(pm_sel);
        return 4;
    }
    if (!set_pm_vector(timer_int_no, FP_SEG(timer_irq_handler), FP_OFF(timer_irq_handler))) {
        mark("[PMIRQSB] TIMER SET FAIL");
        free_dos_buffer(pm_sel);
        return 5;
    }

    if (!get_pm_vector(sb_int_no, &old_irq_sel, &old_irq_off)) {
        mark("[PMIRQSB] PMVEC GET FAIL");
        set_pm_vector(timer_int_no, old_timer_sel, old_timer_off);
        free_dos_buffer(pm_sel);
        return 6;
    }
    if (!set_pm_vector(sb_int_no, FP_SEG(sb_irq_handler), FP_OFF(sb_irq_handler))) {
        mark("[PMIRQSB] PMVEC SET FAIL");
        set_pm_vector(timer_int_no, old_timer_sel, old_timer_off);
        free_dos_buffer(pm_sel);
        return 7;
    }
    mark("[PMIRQSB] PMVEC OK");
    enable_virtual_interrupts();
    _enable();
    mark("[PMIRQSB] VIRQ ON");

    sb_mixer_setup();
    sb_mixer_irq(irq);
    unmask_irq(irq);
    mark_hex8("PIC_IMR_PRE", inp(0x21));
    mark_hex8("PIC_IRR_PRE", pic_read_irr());
    mark_hex8("PIC_ISR_PRE", pic_read_isr());
    irq_hits = 0;
    timer_hits = 0;
    ack_after_eoi = eoi_first;
    program_dma1(phys, DMA_LEN, auto_init);
    mark_hex16("DMA_COUNT_PRE", dma1_count_left());

    dsp_write(0xd1);               /* speaker on */
    dsp_write(0x40);
    dsp_write(0x83);               /* ~8 kHz time constant */
    if (auto_init) {
        dsp_write(0x48);
        dsp_write((unsigned char)((DMA_LEN - 1) & 0xff));
        dsp_write((unsigned char)(((DMA_LEN - 1) >> 8) & 0xff));
        dsp_write(0x1c);
    } else {
        dsp_write(0x14);
        dsp_write((unsigned char)((DMA_LEN - 1) & 0xff));
        dsp_write((unsigned char)(((DMA_LEN - 1) >> 8) & 0xff));
    }
    mark("[PMIRQSB] DMA START");

    while (timer_hits < (auto_init ? 18 : 36) && irq_hits == 0) {
    }

    if (auto_init) {
        dsp_write(0xda);           /* stop 8-bit auto-init DMA */
    }
    mark_hex8("DSP_RD_STATUS", inp(SB_BASE + 0x0e));
    mark_hex8("PIC_IMR_POST", inp(0x21));
    mark_hex8("PIC_IRR_POST", pic_read_irr());
    mark_hex8("PIC_ISR_POST", pic_read_isr());
    mark_hex16("DMA_COUNT_POST", dma1_count_left());

    set_pm_vector(sb_int_no, old_irq_sel, old_irq_off);
    set_pm_vector(timer_int_no, old_timer_sel, old_timer_off);
    dsp_write(0xd3);               /* speaker off */
    outp(0x0a, 0x05);              /* mask DMA1 */
    free_dos_buffer(pm_sel);

    if (timer_hits) {
        mark("[PMIRQSB] TIMER HIT");
    } else {
        mark("[PMIRQSB] TIMER MISS");
    }

    if (irq_hits) {
        mark("[PMIRQSB] IRQ HIT");
        return 0;
    }

    mark("[PMIRQSB] IRQ MISS");
    return 4;
}

static int start_single_dma(unsigned long phys)
{
    program_dma1(phys, DMA_LEN, 0);
    if (!dsp_write(0xd1)) {
        return 0;
    }
    if (!dsp_write(0x40)) {
        return 0;
    }
    if (!dsp_write(0x83)) {        /* ~8 kHz time constant */
        return 0;
    }
    if (!dsp_write(0x14)) {
        return 0;
    }
    if (!dsp_write((unsigned char)((DMA_LEN - 1) & 0xff))) {
        return 0;
    }
    if (!dsp_write((unsigned char)(((DMA_LEN - 1) >> 8) & 0xff))) {
        return 0;
    }
    return 1;
}

static int run_task_mode(void)
{
    unsigned rm_seg = 0;
    unsigned pm_sel = 0;
    unsigned char __far *buf;
    unsigned alloc_size = DMA_LEN + 0x10000UL;
    unsigned dma_off = 0;
    unsigned long phys;
    unsigned starts = 0;
    unsigned seen_irqs = 0;
    unsigned next_tick = 2;
    unsigned long spin = 0;

    mark("[PMIRQSB] TASK INSTALL");

    if (!alloc_dos_buffer((alloc_size + 15) / 16, &rm_seg, &pm_sel)) {
        mark("[PMIRQSB] DOSMEM FAIL");
        return 3;
    }

    phys = ((unsigned long)rm_seg) << 4;
    if (((phys & 0xffffUL) + DMA_LEN - 1) >= 0x10000UL) {
        dma_off = (unsigned)(0x10000UL - (phys & 0xffffUL));
        phys += dma_off;
    }
    buf = (unsigned char __far *)MK_FP(pm_sel, dma_off);
    fill_sample(buf);
    mark_hex16("DMA_PHYS_LO", (unsigned)(phys & 0xffff));
    mark_hex8("DMA_PAGE", (unsigned)((phys >> 16) & 0xff));
    mark("[PMIRQSB] DMA_BOUNDARY OK");

    timer_int_no = master_pic_base;
    sb_int_no = timer_int_no + 7;

    if (!get_pm_vector(timer_int_no, &old_timer_sel, &old_timer_off)) {
        mark("[PMIRQSB] TIMER GET FAIL");
        free_dos_buffer(pm_sel);
        return 4;
    }
    if (!set_pm_vector(timer_int_no, FP_SEG(timer_irq_handler), FP_OFF(timer_irq_handler))) {
        mark("[PMIRQSB] TIMER SET FAIL");
        free_dos_buffer(pm_sel);
        return 5;
    }
    if (!get_pm_vector(sb_int_no, &old_irq_sel, &old_irq_off)) {
        mark("[PMIRQSB] PMVEC GET FAIL");
        set_pm_vector(timer_int_no, old_timer_sel, old_timer_off);
        free_dos_buffer(pm_sel);
        return 6;
    }
    if (!set_pm_vector(sb_int_no, FP_SEG(sb_irq_handler), FP_OFF(sb_irq_handler))) {
        mark("[PMIRQSB] PMVEC SET FAIL");
        set_pm_vector(timer_int_no, old_timer_sel, old_timer_off);
        free_dos_buffer(pm_sel);
        return 7;
    }
    mark("[PMIRQSB] PMVEC OK");

    task_mode_enabled = 1;
    task_dma_busy = 0;
    task_dma_irqs = 0;
    irq_hits = 0;
    timer_hits = 0;
    ack_after_eoi = 0;

    enable_virtual_interrupts();
    _enable();
    mark("[PMIRQSB] VIRQ ON");

    sb_mixer_setup();
    sb_mixer_irq(7);
    unmask_irq(7);

    while (spin++ < 0x7fffffffUL && timer_hits < 180 && task_dma_irqs < 3) {
        if (!task_dma_busy && starts < 3 && timer_hits >= next_tick) {
            mark("[PMIRQSB] TASK HIT");
            mark("[PMIRQSB] FXDMA START");
            task_dma_busy = 1;
            ++starts;
            next_tick = timer_hits + 6;
            if (!start_single_dma(phys)) {
                mark("[PMIRQSB] FXDMA START FAIL");
                break;
            }
        }
        if (task_dma_irqs != seen_irqs) {
            seen_irqs = task_dma_irqs;
            mark("[PMIRQSB] FXDMA IRQ");
        }
    }
    if (task_dma_irqs != seen_irqs) {
        mark("[PMIRQSB] FXDMA IRQ");
    }

    task_mode_enabled = 0;
    set_pm_vector(sb_int_no, old_irq_sel, old_irq_off);
    set_pm_vector(timer_int_no, old_timer_sel, old_timer_off);
    dsp_write(0xd3);               /* speaker off */
    outp(0x0a, 0x05);              /* mask DMA1 */
    free_dos_buffer(pm_sel);

    if (timer_hits) {
        mark("[PMIRQSB] TIMER HIT");
    } else {
        mark("[PMIRQSB] TIMER MISS");
    }

    if (starts == 3 && task_dma_irqs == 3) {
        mark("[PMIRQSB] TASK PASS");
        return 0;
    }

    mark_hex16("TASK_STARTS", starts);
    mark_hex16("TASK_IRQS", task_dma_irqs);
    mark("[PMIRQSB] TASK FAIL");
    return 4;
}

int main(int argc, char **argv)
{
    unsigned failures = 0;
    unsigned run_irq5 = 0;
    unsigned prime_only = 0;
    unsigned task_mode = 0;

    mark("[PMIRQSB] BEGIN");

    if (!dsp_reset()) {
        mark("[PMIRQSB] DSP FAIL");
        return 2;
    }
    mark("[PMIRQSB] DSP OK");

    master_pic_base = get_master_pic_base();

    if (argc > 1 && (strstr(argv[1], "IRQ5") || strstr(argv[1], "irq5"))) {
        run_irq5 = 1;
    }
    if (argc > 1 && (strstr(argv[1], "PRIME") || strstr(argv[1], "prime"))) {
        prime_only = 1;
    }
    if (argc > 1 && (strstr(argv[1], "TASK") || strstr(argv[1], "task"))) {
        task_mode = 1;
    }

    if (task_mode) {
        failures += run_task_mode() ? 1 : 0;
    } else if (prime_only) {
        failures += run_variant("IRQ7_PRIME", 7, 0, 0) ? 1 : 0;
    } else if (run_irq5) {
        failures += run_variant("IRQ5_SINGLE_ACK", 5, 0, 0) ? 1 : 0;
        failures += run_variant("IRQ5_SINGLE_EOI", 5, 0, 1) ? 1 : 0;
        failures += run_variant("IRQ5_AUTO_ACK", 5, 1, 0) ? 1 : 0;
    } else {
        failures += run_variant("IRQ7_SINGLE_ACK", 7, 0, 0) ? 1 : 0;
        failures += run_variant("IRQ7_SINGLE_EOI", 7, 0, 1) ? 1 : 0;
        failures += run_variant("IRQ7_AUTO_ACK", 7, 1, 0) ? 1 : 0;
    }

    if (failures == 0) {
        mark("[PMIRQSB] PASS");
        return 0;
    }

    mark("[PMIRQSB] FAIL");
    return 4;
}
