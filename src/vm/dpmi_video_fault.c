#include "dpmi_video_fault.h"
#include "vga_x86.h"
#include <conio.h>
#include <i86.h>
#include <string.h>

/* Original CiukiOS implementation. DPMI 0.9 functions 0006h, 0100h, 0101h,
 * 0202h, 0203h and 0301h as specified in the DPMI 0.9 specification. */

#define QUEUE_MAX 64

/* Defined in dpmi_video_fault_thunk.asm (NASM OMF data referenced from
 * NASM code must live in the NASM object's DGROUP segment). */
extern void cvpm_handler(void);
extern uint32_t cvpm_previous_offset;
extern uint16_t cvpm_previous_selector;
extern uint16_t cvpm_data_selector;

static uint16_t entry_seg, entry_off, dos_seg, dos_sel, installed;
static uint8_t *packet;                 /* flat view of the DOS transfer block */
static unsigned queued;
static int dry_run, touched, bridge_error;

typedef struct rm_call {
    uint32_t edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    uint16_t flags, es, ds, fs, gs, ip, cs, sp, ss;
} rm_call;

static unsigned selector_d_bit(unsigned selector);
#pragma aux selector_d_bit = "lar eax,eax" "jz ok" "xor eax,eax" "ok:" "shr eax,22" "and eax,1" \
    parm [eax] value [eax] modify exact [eax]

static uint32_t segment_base(uint16_t selector)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 6;
    r.w.bx = selector;
    int386(0x31, &r, &r);
    if (r.w.cflag) return 0;
    return ((uint32_t)r.w.cx << 16) | r.w.dx;
}

static int aperture(uint32_t linear)
{
    return linear >= 0xa0000UL && linear < 0xc0000UL;
}

/* One ordered batch through the JLM entry (VM_OP_VIDEO_ACCESS = 2Ch). */
static int bridge(void)
{
    union REGS r;
    struct SREGS sr;
    static rm_call call;
    if (!queued) return 0;
    packet[0] = 'C'; packet[1] = 'V'; packet[2] = 'V'; packet[3] = 'A';
    packet[4] = (uint8_t)queued; packet[5] = packet[6] = packet[7] = 0;
    memset(&call, 0, sizeof(call));
    call.eax = 0x2c;
    call.es = dos_seg;
    call.edi = 0;
    call.cs = entry_seg;
    call.ip = entry_off;
    memset(&r, 0, sizeof(r));
    segread(&sr);
    r.w.ax = 0x0301;
    r.x.ecx = 0;
    r.x.edi = (unsigned)&call;
    sr.es = sr.ds;
    int386x(0x31, &r, &r, &sr);
    ++cvpm_stats_data.bridge_calls;
    cvpm_stats_data.aperture_bytes += queued;
    if (r.w.cflag || (call.flags & 1)) {
        ++cvpm_stats_data.bridge_failures;
        bridge_error = 1;
        return 1;
    }
    return 0;
}

static int queue(uint32_t linear, int write, uint8_t value)
{
    uint8_t *e;
    if (queued == QUEUE_MAX && bridge()) return 1;
    if (queued == QUEUE_MAX) queued = 0;
    e = packet + 8 + queued * 8;
    e[0] = (uint8_t)linear; e[1] = (uint8_t)(linear >> 8);
    e[2] = (uint8_t)(linear >> 16); e[3] = (uint8_t)(linear >> 24);
    e[4] = (uint8_t)write; e[5] = value; e[6] = e[7] = 0;
    ++queued;
    return 0;
}

static int CVGA_CALL bus_read(void *context, uint32_t linear, uint8_t *value)
{
    (void)context;
    if (aperture(linear)) {
        touched = 1;
        if (dry_run) { *value = 0; return 0; }
        /* The read joins any pending writes: one round trip, bus order kept. */
        if (queue(linear, 0, 0) || bridge()) return 1;
        *value = packet[8 + (queued - 1) * 8 + 5];
        queued = 0;
        return 0;
    }
    *value = dry_run ? 0 : *(volatile uint8_t *)linear;
    return 0;
}

static int CVGA_CALL bus_write(void *context, uint32_t linear, uint8_t value)
{
    (void)context;
    if (aperture(linear)) {
        touched = 1;
        return dry_run ? 0 : queue(linear, 1, value);
    }
    if (!dry_run) *(volatile uint8_t *)linear = value;
    return 0;
}

static int CVGA_CALL bus_fetch(void *context, uint32_t linear, uint8_t *value)
{
    (void)context;
    if (aperture(linear)) return 1;
    *value = *(volatile uint8_t *)linear;
    return 0;
}

/* Chaining ends the client under most extenders; leave the reason on COM1. */
static void report_chain(const char *reason, const cvpm_frame *f, const cvx_cpu *cpu, int status)
{
    static const char hex[] = "0123456789ABCDEF";
    const char *p;
    char line[96];
    unsigned i = 0, wait, shift;
    uint32_t values[5];
    for (p = "[DPMIVGA] PM CHAIN "; *p; ++p) line[i++] = *p;
    for (p = reason; *p && i < 40; ++p) line[i++] = *p;
    values[0] = f->cs; values[1] = f->eip; values[2] = cpu->code32;
    values[3] = (uint32_t)status; values[4] = cpu->seg_base[CVX_ES];
    for (wait = 0; wait < 5; ++wait) {
        line[i++] = ' ';
        for (shift = 28; shift < 32; shift -= 4) line[i++] = hex[(values[wait] >> shift) & 15];
    }
    line[i++] = '\r'; line[i++] = '\n';
    for (shift = 0; shift < i; ++shift) {
        wait = 65535;
        while (wait-- && !(inp(0x3fd) & 0x20)) { }
        outp(0x3f8, line[shift]);
    }
    {   /* raw frame dwords, for diagnosing host frame layouts */
        const uint32_t *raw = (const uint32_t *)f;
        unsigned k, d;
        for (k = 0; k < 20; ++k) {
            char word[10];
            for (d = 0; d < 8; ++d) word[d] = hex[(raw[k] >> (28 - 4 * d)) & 15];
            word[8] = k == 19 ? '\r' : ' ';
            word[9] = k == 19 ? '\n' : 0;
            for (d = 0; d < (k == 19 ? 10u : 9u); ++d) {
                wait = 65535;
                while (wait-- && !(inp(0x3fd) & 0x20)) { }
                outp(0x3f8, word[d]);
            }
        }
    }
}

int cvpm_fault(void)
{
    static const cvx_bus bus = {bus_read, bus_write, bus_fetch, 0};
    cvpm_frame *f = &cvpm_frame_data;
    cvx_cpu cpu, probe;
    cvx_result result;
    int status, i;
    ++cvpm_stats_data.faults;
    cpu.gpr[CVX_EAX] = f->eax; cpu.gpr[CVX_ECX] = f->ecx;
    cpu.gpr[CVX_EDX] = f->edx; cpu.gpr[CVX_EBX] = f->ebx;
    cpu.gpr[CVX_ESP] = f->esp; cpu.gpr[CVX_EBP] = f->ebp;
    cpu.gpr[CVX_ESI] = f->esi; cpu.gpr[CVX_EDI] = f->edi;
    cpu.eip = f->eip;
    cpu.eflags = f->eflags;
    cpu.sreg[CVX_ES] = (uint16_t)f->es; cpu.sreg[CVX_CS] = (uint16_t)f->cs;
    cpu.sreg[CVX_SS] = (uint16_t)f->ss; cpu.sreg[CVX_DS] = (uint16_t)f->ds;
    cpu.sreg[CVX_FS] = (uint16_t)f->fs; cpu.sreg[CVX_GS] = (uint16_t)f->gs;
    for (i = 0; i < 6; ++i) cpu.seg_base[i] = cpu.sreg[i] ? segment_base(cpu.sreg[i]) : 0;
    cpu.code32 = (uint8_t)selector_d_bit(f->cs);
    /* Dry run: decode and route without any memory side effect. Only an
     * instruction that addresses the aperture is ours. */
    probe = cpu;
    dry_run = 1;
    touched = 0;
    status = cvx_execute(&probe, &bus, 1, &result);
    dry_run = 0;
    if (!touched || status == CVX_UNSUPPORTED || status == CVX_TOO_LONG) {
        if (touched) ++cvpm_stats_data.unsupported;
        ++cvpm_stats_data.chained;
        report_chain(touched ? "unsupported" : "not-aperture", f, &cpu, status);
        return 1;
    }
    queued = 0;
    bridge_error = 0;
    status = cvx_execute(&cpu, &bus, 4096, &result);
    if ((status == CVX_DONE || status == CVX_PARTIAL) && !bridge() && !bridge_error) {
        f->eax = cpu.gpr[CVX_EAX]; f->ecx = cpu.gpr[CVX_ECX];
        f->edx = cpu.gpr[CVX_EDX]; f->ebx = cpu.gpr[CVX_EBX];
        f->ebp = cpu.gpr[CVX_EBP]; f->esi = cpu.gpr[CVX_ESI];
        f->edi = cpu.gpr[CVX_EDI];
        f->eip = cpu.eip;
        f->eflags = (f->eflags & ~0x8d5UL) | (cpu.eflags & 0x8d5UL);
        ++cvpm_stats_data.emulated;
        return 0;
    }
    ++cvpm_stats_data.unsupported;
    ++cvpm_stats_data.chained;
    report_chain(bridge_error ? "bridge" : "execute", f, &cpu, status);
    return 1;
}

int cvpm_install(uint16_t entry_segment, uint16_t entry_offset)
{
    union REGS r;
    if (installed) return CVPM_INSTALLED;
    memset(&cvpm_stats_data, 0, sizeof(cvpm_stats_data));
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;                     /* DOS memory for the access packet */
    r.w.bx = (8 + QUEUE_MAX * 8 + 15) / 16;
    int386(0x31, &r, &r);
    if (r.w.cflag) return CVPM_DOS_MEMORY;
    dos_seg = r.w.ax;
    dos_sel = r.w.dx;
    packet = (uint8_t *)((uint32_t)dos_seg << 4);   /* flat model: linear == near */
    entry_seg = entry_segment;
    entry_off = entry_offset;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0202;
    r.h.bl = 0x0e;
    int386(0x31, &r, &r);
    if (r.w.cflag) return CVPM_HANDLER;
    cvpm_previous_selector = r.w.cx;
    cvpm_previous_offset = r.x.edx;
    {
        struct SREGS sr;
        segread(&sr);
        cvpm_data_selector = sr.ds;
        memset(&r, 0, sizeof(r));
        r.w.ax = 0x0203;
        r.h.bl = 0x0e;
        r.w.cx = sr.cs;
        r.x.edx = (unsigned)cvpm_handler;
        int386(0x31, &r, &r);
    }
    if (r.w.cflag) return CVPM_HANDLER;
    installed = 1;
    return CVPM_OK;
}

int cvpm_remove(void)
{
    union REGS r;
    if (!installed) return CVPM_OK;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0203;
    r.h.bl = 0x0e;
    r.w.cx = cvpm_previous_selector;
    r.x.edx = cvpm_previous_offset;
    int386(0x31, &r, &r);
    if (r.w.cflag) return CVPM_HANDLER;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0101;
    r.w.dx = dos_sel;
    int386(0x31, &r, &r);
    installed = 0;
    return CVPM_OK;
}
