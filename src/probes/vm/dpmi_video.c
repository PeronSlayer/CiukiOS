/* Actual IOPL=0 guest instructions. Run only after packaged HDPMI32I -r.
 * No success can be obtained by directly invoking cvga_read/write_port here.
 * This is a bounded port/exception adapter probe, NOT DOS window acceptance. */
#include "../../vm/dpmi_video_io.h"
#include "../../vm/dpmi_video_fault.h"
#include <stdlib.h>
#include <conio.h>
#include <i86.h>
#include <stdio.h>
#include <string.h>

static cvga_state video;
static unsigned failures;

static unsigned io_in8(unsigned port, unsigned seed);
#pragma aux io_in8 = "in al,dx" parm [edx] [eax] value [eax] modify exact [eax]
static unsigned io_in16(unsigned port, unsigned seed);
#pragma aux io_in16 = "in ax,dx" parm [edx] [eax] value [eax] modify exact [eax]
static unsigned io_in32(unsigned port);
#pragma aux io_in32 = "in eax,dx" parm [edx] value [eax] modify exact [eax]
static void io_out8(unsigned port, unsigned value);
#pragma aux io_out8 = "out dx,al" parm [edx] [eax] modify exact []
static void io_out16(unsigned port, unsigned value);
#pragma aux io_out16 = "out dx,ax" parm [edx] [eax] modify exact []
static void io_out32(unsigned port, unsigned value);
#pragma aux io_out32 = "out dx,eax" parm [edx] [eax] modify exact []
static unsigned io_flags(unsigned port);
#pragma aux io_flags = \
    "std" "stc" "pushfd" "pop ecx" "in al,dx" \
    "pushfd" "pop eax" "cld" "xor eax,ecx" "and eax,0xed5h" \
    parm [edx] value [eax] modify exact [eax ecx]
static unsigned io_outs_rejected(unsigned port, const char *data, unsigned count);
#pragma aux io_outs_rejected = "rep outsb" \
    parm [edx] [esi] [ecx] value [ecx] modify exact [esi ecx]
static void mapped_store(unsigned selector, unsigned offset, unsigned value);
#pragma aux mapped_store = "push es" "mov es,cx" "mov es:[edi],eax" "pop es" \
    parm [ecx] [edi] [eax] modify exact []
static unsigned mapped_load(unsigned selector, unsigned offset);
#pragma aux mapped_load = "push es" "mov es,cx" "mov eax,es:[edi]" "pop es" \
    parm [ecx] [edi] value [eax] modify exact [eax]

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

static void check(unsigned got, unsigned expected, const char *name)
{
    char line[128];
    if (got != expected) {
        ++failures;
        sprintf(line, "[DPMIVGA] FAIL %s got=%08lx wanted=%08lx", name,
                (unsigned long)got, (unsigned long)expected);
        mark(line);
    }
}

static void mapped_aperture_probe(unsigned entry_segment, unsigned entry_offset)
{
    union REGS regs;
    unsigned selector, rc;
    char line[160];
    /* Opt-in ONLY after the parent armed CVSESSION and then loaded this fresh
     * HDPMI: its copied VGA PTEs are supervisor-only, so every access below
     * faults and is executed against the monitor's shared VGA model. */
    rc = cvpm_install((uint16_t)entry_segment, (uint16_t)entry_offset);
    if (rc) { check(rc, 0, "PM video fault handler install"); return; }
    memset(&regs, 0, sizeof(regs));
    regs.w.ax = 2; regs.w.bx = 0xa000;
    int386(0x31, &regs, &regs);
    if (regs.w.cflag) { check(1, 0, "DPMI A000 selector"); cvpm_remove(); return; }
    selector = regs.w.ax;
    /* The session is in text mode 03h: GC06 decodes B8000-BFFFF only, so an
     * A0000 store reaches no plane and a load returns FFh bytes (as on VGA). */
    mapped_store(selector, 0, 0x504d4443UL);
    check(mapped_load(selector, 0), 0xffffffffUL, "protected A000 outside text map");
    memset(&regs, 0, sizeof(regs));
    regs.w.ax = 2; regs.w.bx = 0xb800;
    int386(0x31, &regs, &regs);
    if (regs.w.cflag) { check(1, 0, "DPMI B800 selector"); cvpm_remove(); return; }
    selector = regs.w.ax;
    mapped_store(selector, 0, 0x31564d50UL);
    check(mapped_load(selector, 0), 0x31564d50UL, "protected B800 odd/even round trip");
    check(cvpm_stats_data.emulated, 4, "four trapped PM instructions");
    check(cvpm_stats_data.chained, 0, "no chained PM fault");
    check(cvpm_stats_data.bridge_failures, 0, "no bridge failure");
    sprintf(line, "[DPMIVGA] PM FAULTS=%lu EMULATED=%lu BRIDGE_CALLS=%lu APERTURE_BYTES=%lu",
            (unsigned long)cvpm_stats_data.faults, (unsigned long)cvpm_stats_data.emulated,
            (unsigned long)cvpm_stats_data.bridge_calls, (unsigned long)cvpm_stats_data.aperture_bytes);
    mark(line);
    check(cvpm_remove(), CVPM_OK, "PM video fault handler remove");
    /* DPMI0002 selectors are host-managed; do not free them with0001. */
    if (!failures) mark("[DPMIVGA] PM APERTURE CYCLES REACHED THE SHARED MODEL: A000=FFFFFFFF B800:0000=31564D50");
}

int main(int argc, char **argv)
{
    static const unsigned ports[] = {0x3cc,0x3c4,0x3c5,0x3c6,0x3c7,0x3c8};
    unsigned host_before[6], i, rc, handle, before_reads, before_writes;
    char line[128];
    const char rejected[] = {1,2,3};
    mark("[DPMIVGA] BEGIN original CPU IN/OUT, packaged HDPMI 3.24 required");
    if (argc > 2 && !strcmp(argv[1], "-map"))
        mapped_aperture_probe((unsigned)strtoul(argv[2], 0, 16),
                              (unsigned)strtoul(strchr(argv[2], ':') ? strchr(argv[2], ':') + 1 : "0", 0, 16));
    for (i = 0; i < 6; ++i) host_before[i] = io_in8(ports[i], 0);
    cvga_init(&video);
    check(cvio_init(&video), CVIO_OK, "init");
    rc = cvio_install();
    if (rc) {
        sprintf(line, "[DPMIVGA] INSTALL FAIL %u (2=not HDPMI,3=overlap,9=not flat)", rc);
        mark(line);
        return 2;
    }
    mark("[DPMIVGA] INSTALLED");
    handle = cvio_stats_data.handle;
    check(handle != 0, 1, "owned handle");
    check(cvio_install(), CVIO_ALREADY_INSTALLED, "duplicate install rejected");
    check(cvio_stats_data.handle, handle, "duplicate preserves handle");
    io_out8(0x3c6, 0x5a);
    check(io_in8(0x3c6, 0x12345600), 0x1234565a, "IN AL preserves EAX high24");
    io_out16(0x3c4, 0x0b02);
    check(io_in16(0x3c4, 0xa5a50000), 0xa5a50b02, "IN AX and OUT AX");
    io_out32(0x3c4, 0x22120f02);
    check(io_in32(0x3c4), 0x03120f02, "IN EAX and OUT EAX");
    check(video.seq[2], 15, "dword seq mask");
    check(video.dac_mask, 0x12, "dword DAC mask");
    io_out8(0x3c8, 255);
    io_out8(0x3c9, 63); io_out8(0x3c9, 17); io_out8(0x3c9, 32);
    check(io_in8(0x3c8, 0), 0, "DAC write index wrap");
    io_out8(0x3c7, 255);
    check(io_in8(0x3c9, 0), 63, "DAC red");
    check(io_in8(0x3c9, 0), 17, "DAC green");
    check(io_in8(0x3c9, 0), 32, "DAC blue");
    cvio_set_status1(9);
    for (i = 0; i < 200; ++i) check(io_in8(0x3da, 0), 9, "retrace explicit");
    cvio_set_status1(0);
    check(io_in8(0x3da, 0), 0, "retrace clear");
    check(io_flags(0x3c6), 0, "EFLAGS incl DF CF IF unchanged");
    check(cvio_stats_data.fatal, 0, "no fatal for supported instructions");
    check(cvio_stats_data.traps, 217, "real trap count");
    if (!failures) mark("[DPMIVGA] BYTE WORD DWORD DAC FLAGS PASS");
    before_reads = cvio_stats_data.reads;
    before_writes = cvio_stats_data.writes;
    check(io_outs_rejected(0x3c9, rejected, 3), 3, "unsupported REP count retained");
    check(cvio_stats_data.fatal, CVIO_STRING_UNSUPPORTED, "string explicit fatal");
    check(cvio_stats_data.unsupported, 1, "one rejected string");
    check(cvio_stats_data.reads, before_reads, "rejected string no read");
    check(cvio_stats_data.writes, before_writes, "rejected string no write");
    if (!failures) mark("[DPMIVGA] STRING REJECTED WITHOUT HARDWARE PASS");
    rc = cvio_remove();
    check(rc, CVIO_OK, "remove");
    if (rc) {
        mark("[DPMIVGA] FATAL: ownership retained; stop this test VM");
        /* Never return to DOS with a possibly dangling global host callback. */
        for (;;) { }
    }
    check(cvio_stats_data.handle, 0, "handle cleared");
    check(cvio_remove(), CVIO_OK, "empty remove never calls handle0");
    for (i = 0; i < 6; ++i) check(io_in8(ports[i], 0), host_before[i], "physical VGA unchanged");
    sprintf(line, "[DPMIVGA] COUNTS traps=%lu in=%lu out=%lu rejected=%lu",
            (unsigned long)cvio_stats_data.traps, (unsigned long)cvio_stats_data.reads,
            (unsigned long)cvio_stats_data.writes, (unsigned long)cvio_stats_data.unsupported);
    mark(line);
    if (!failures) mark("[DPMIVGA] PASS ports isolated, register/frame restoration, exact remove");
    else mark("[DPMIVGA] FAIL");
    return failures ? 1 : 0;
}
