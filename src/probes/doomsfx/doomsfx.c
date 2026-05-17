/*
 * DOOMSFX.LE - controlled DOOM WAD SB16 SFX lane for CiukiOS.
 *
 * This is not the original DOOM.EXE audio path. It is a small public-source
 * DOS/4GW harness that loads one DOOM sound lump and plays it through SB16
 * 8-bit DMA with protected-mode IRQ proof markers.
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
#define SB_IRQ    7
#define SB_DMA    1
#define DMA_MAX   32768U
#define DEFAULT_LUMP "DSPISTOL"

typedef struct {
    char id[4];
    uint32_t num_lumps;
    uint32_t dir_offset;
} wad_header_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
    char name[8];
} wad_dir_t;

static volatile unsigned irq_hits;
static volatile unsigned timer_hits;
static unsigned old_irq_sel;
static unsigned long old_irq_off;
static unsigned old_timer_sel;
static unsigned long old_timer_off;
static unsigned master_pic_base;
static unsigned sb_int_no;
static unsigned timer_int_no;

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

static void serial_put_dec(unsigned value)
{
    char buf[6];
    int i = 0;
    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i--) {
        serial_putc(buf[i]);
    }
}

static void mark_u16(const char *name, unsigned value)
{
    printf("[DOOMSFX] %s=%u\n", name, value);
    serial_puts("[DOOMSFX] ");
    serial_puts(name);
    serial_puts("=");
    serial_put_dec(value);
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

static void unmask_irq(unsigned irq)
{
    outp(0x21, inp(0x21) & (unsigned char)~(1U << irq));
}

static void program_dma1(unsigned long phys, unsigned count)
{
    unsigned len = count - 1;
    outp(0x0a, 0x05);
    outp(0x0c, 0x00);
    outp(0x0b, 0x49);
    outp(0x02, (unsigned char)(phys & 0xff));
    outp(0x02, (unsigned char)((phys >> 8) & 0xff));
    outp(0x83, (unsigned char)((phys >> 16) & 0xff));
    outp(0x03, (unsigned char)(len & 0xff));
    outp(0x03, (unsigned char)((len >> 8) & 0xff));
    outp(0x0a, 0x01);
}

static void __interrupt __far sb_irq_handler(void)
{
    ++irq_hits;
    inp(SB_BASE + 0x0e);
    outp(0x20, 0x20);
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

static void enable_virtual_interrupts(void)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0901;
    int386(0x31, &r, &r);
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int lump_name_eq(const char *name, const char *want)
{
    char tmp[9];
    unsigned i;
    for (i = 0; i < 8; ++i) {
        tmp[i] = name[i];
    }
    tmp[8] = 0;
    for (i = 0; i < 8 && tmp[i]; ++i) {
        if (tmp[i] >= 'a' && tmp[i] <= 'z') {
            tmp[i] = (char)(tmp[i] - 32);
        }
    }
    return strncmp(tmp, want, 8) == 0;
}

static void copy_lump_arg(char *dst, const char *src)
{
    unsigned i;
    for (i = 0; i < 8 && src[i]; ++i) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        dst[i] = c;
    }
    dst[i] = 0;
}

static int supported_lump(const char *name)
{
    return strcmp(name, "DSPISTOL") == 0 ||
           strcmp(name, "DSDOROPN") == 0 ||
           strcmp(name, "DSITEMUP") == 0;
}

static void mark_lump(const char *name)
{
    printf("[DOOMSFX] LUMP %s\n", name);
    serial_puts("[DOOMSFX] LUMP ");
    serial_puts(name);
    serial_puts("\n");
}

static int load_lump(const char *path, const char *lump, unsigned char *dst,
                     unsigned *out_len, unsigned *out_rate)
{
    FILE *fp;
    wad_header_t hdr;
    wad_dir_t ent;
    unsigned i;
    unsigned char head[8];
    uint32_t raw_len;

    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        fclose(fp);
        return 0;
    }
    if (memcmp(hdr.id, "IWAD", 4) != 0 && memcmp(hdr.id, "PWAD", 4) != 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, hdr.dir_offset, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    for (i = 0; i < hdr.num_lumps; ++i) {
        if (fread(&ent, 1, sizeof(ent), fp) != sizeof(ent)) {
            fclose(fp);
            return 0;
        }
        if (!lump_name_eq(ent.name, lump)) {
            continue;
        }
        if (ent.size <= 8 || fseek(fp, ent.offset, SEEK_SET) != 0) {
            fclose(fp);
            return 0;
        }
        if (fread(head, 1, sizeof(head), fp) != sizeof(head)) {
            fclose(fp);
            return 0;
        }
        *out_rate = rd16(head + 2);
        raw_len = rd32(head + 4);
        if (raw_len > ent.size - 8) {
            raw_len = ent.size - 8;
        }
        if (raw_len > DMA_MAX) {
            raw_len = DMA_MAX;
        }
        if (fread(dst, 1, raw_len, fp) != raw_len) {
            fclose(fp);
            return 0;
        }
        *out_len = (unsigned)raw_len;
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

static int play_dma(unsigned long phys, unsigned len, unsigned rate)
{
    unsigned tc;
    if (rate < 4000 || rate > 44100) {
        rate = 11025;
    }
    tc = 256 - (1000000UL / rate);
    if (tc > 255) {
        tc = 165;
    }
    irq_hits = 0;
    timer_hits = 0;
    program_dma1(phys, len);
    if (!dsp_write(0xd1)) return 0;
    if (!dsp_write(0x40)) return 0;
    if (!dsp_write((unsigned char)tc)) return 0;
    if (!dsp_write(0x14)) return 0;
    if (!dsp_write((unsigned char)((len - 1) & 0xff))) return 0;
    if (!dsp_write((unsigned char)(((len - 1) >> 8) & 0xff))) return 0;
    mark("[DOOMSFX] DMA START");
    while (timer_hits < 72 && irq_hits == 0) {
    }
    return irq_hits != 0;
}

int main(int argc, char **argv)
{
    unsigned rm_seg = 0;
    unsigned pm_sel = 0;
    unsigned char __far *buf;
    unsigned char *tmp;
    unsigned alloc_size = DMA_MAX + 0x10000UL;
    unsigned dma_off = 0;
    unsigned long phys;
    unsigned len = 0;
    unsigned rate = 0;
    int ok = 0;
    char lump[9] = DEFAULT_LUMP;

    mark("[DOOMSFX] BEGIN");
    mark("[DOOMSFX] CANDIDATE doom-vanille later; harness now");
    if (argc > 1 && argv[1] && argv[1][0]) {
        copy_lump_arg(lump, argv[1]);
    }
    if (!supported_lump(lump)) {
        mark("[DOOMSFX] BAD LUMP");
        mark("[DOOMSFX] SUPPORTED DSPISTOL DSDOROPN DSITEMUP");
        return 1;
    }

    if (!dsp_reset()) {
        mark("[DOOMSFX] DSP FAIL");
        return 2;
    }
    mark("[DOOMSFX] DSP OK");

    if (!alloc_dos_buffer((alloc_size + 15) / 16, &rm_seg, &pm_sel)) {
        mark("[DOOMSFX] DOSMEM FAIL");
        return 3;
    }
    phys = ((unsigned long)rm_seg) << 4;
    if (((phys & 0xffffUL) + DMA_MAX - 1) >= 0x10000UL) {
        dma_off = (unsigned)(0x10000UL - (phys & 0xffffUL));
        phys += dma_off;
    }
    buf = (unsigned char __far *)MK_FP(pm_sel, dma_off);

    tmp = malloc(DMA_MAX);
    if (!tmp) {
        mark("[DOOMSFX] HEAP FAIL");
        free_dos_buffer(pm_sel);
        return 4;
    }

    if (!load_lump("\\APPS\\DOOM\\DOOM.WAD", lump, tmp, &len, &rate)) {
        mark("[DOOMSFX] WAD/LUMP FAIL");
        free(tmp);
        free_dos_buffer(pm_sel);
        return 4;
    }
    {
        unsigned i;
        for (i = 0; i < len; ++i) {
            buf[i] = tmp[i];
        }
    }
    free(tmp);
    mark("[DOOMSFX] WAD OK");
    mark_lump(lump);
    mark_u16("RATE", rate);
    mark_u16("LEN", len);

    master_pic_base = get_master_pic_base();
    timer_int_no = master_pic_base;
    sb_int_no = master_pic_base + SB_IRQ;
    if (!get_pm_vector(timer_int_no, &old_timer_sel, &old_timer_off) ||
        !get_pm_vector(sb_int_no, &old_irq_sel, &old_irq_off)) {
        mark("[DOOMSFX] PMVEC GET FAIL");
        free_dos_buffer(pm_sel);
        return 5;
    }
    if (!set_pm_vector(timer_int_no, FP_SEG(timer_irq_handler), FP_OFF(timer_irq_handler)) ||
        !set_pm_vector(sb_int_no, FP_SEG(sb_irq_handler), FP_OFF(sb_irq_handler))) {
        mark("[DOOMSFX] PMVEC SET FAIL");
        free_dos_buffer(pm_sel);
        return 6;
    }
    mark("[DOOMSFX] PMVEC OK");

    sb_mixer_setup();
    unmask_irq(SB_IRQ);
    enable_virtual_interrupts();
    _enable();
    mark("[DOOMSFX] VIRQ ON");

    ok = play_dma(phys, len, rate);

    set_pm_vector(sb_int_no, old_irq_sel, old_irq_off);
    set_pm_vector(timer_int_no, old_timer_sel, old_timer_off);
    dsp_write(0xd3);
    outp(0x0a, 0x05);
    free_dos_buffer(pm_sel);

    if (timer_hits) {
        mark("[DOOMSFX] TIMER HIT");
    }
    if (ok) {
        mark("[DOOMSFX] IRQ HIT");
        mark("[DOOMSFX] PASS");
        return 0;
    }
    mark("[DOOMSFX] IRQ MISS");
    mark("[DOOMSFX] FAIL");
    return 7;
}
