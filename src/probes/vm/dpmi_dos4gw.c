/* DPMI4GW.EXE: the pinned DOS/4GW client of directive f3-04, built by
 * scripts/build_dos_image.py with Open Watcom wcl386 (-bt=dos -l=dos4g)
 * against binw/dos4gw.exe. Under Ciuki VMM no HDPMI/VCPI exists, so the
 * extender can only run as a client of the kernel DPMI host. It reports on
 * the VM record port E9h (virtualized OUT from ring 3):
 *   DPMI4GW BEGIN / DPMI4GW HOST <ax> <bx> <cl> / DPMI4GW PASS | FAIL <what>
 * Tests: host version, mode report, extender heap and direct DPMI memory,
 * PIC port I/O, a protected-mode IRQ0 handler with chaining, virtual IF
 * services, selector services, DOS date/time and file I/O through the
 * extender's translation, then a normal exit (INT 21h/4Ch in protected
 * mode). 2026-10-25: DOS/4GW's own INT 21h 2Ah/2Ch handlers (the DOS/16M
 * kernel, AT-class machines) read the BIOS tick count through GDT selector
 * 40h; without that selector the first _dos_gettime stopped the program
 * with "DOS/4GW error (2001): exception 0Dh ... at C7:000014AF".
 * SPDX-License-Identifier: GPL-2.0-only */
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static char first[24];
static volatile unsigned long ticks;
static void (__interrupt __far *old_timer)(void);

static void put(const char *s)
{
    while (*s) outp(0xE9, *s++);
}
static void line(const char *s)
{
    put(s);
    outp(0xE9, '\n');
}
static void fail(const char *what)
{
    if (!failures) strncpy(first, what, sizeof(first) - 1);
    ++failures;
}
static void __interrupt __far timer(void)
{
    ++ticks;
    _chain_intr(old_timer);
}
static unsigned dpmi(unsigned ax, union REGS *r)
{
    r->w.ax = ax;
    int386(0x31, r, r);
    return r->w.cflag;
}

int main(void)
{
    union REGS r;
    struct dosdate_t date, again;
    struct dostime_t time;
    char text[48];
    unsigned long i, start, guard;
    unsigned char imr, *p;
    FILE *f;

    line("DPMI4GW BEGIN");
    memset(&r, 0, sizeof(r));
    if (dpmi(0x0400, &r)) fail("V0400");
    sprintf(text, "DPMI4GW HOST %04X %04X %02X", r.w.ax, r.w.bx, r.h.cl);
    line(text);
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x1686;
    int386(0x2F, &r, &r);
    if (r.w.ax != 0) fail("M1686");

    p = malloc(1024UL * 1024UL);
    if (!p) fail("MALLOC");
    else {
        for (i = 0; i < 1024UL * 1024UL; i++) p[i] = (unsigned char)(i * 7 + 3);
        for (i = 0; i < 1024UL * 1024UL; i++) if (p[i] != (unsigned char)(i * 7 + 3)) { fail("HEAP"); break; }
        free(p);
    }
    memset(&r, 0, sizeof(r));
    r.w.bx = 0;
    r.w.cx = 0x4000;
    if (dpmi(0x0501, &r)) fail("A0501");
    else {
        unsigned si = r.w.si, di = r.w.di;
        memset(&r, 0, sizeof(r));
        r.w.si = si;
        r.w.di = di;
        if (dpmi(0x0502, &r)) fail("F0502");
    }
    memset(&r, 0, sizeof(r));
    r.w.cx = 1;
    if (dpmi(0x0000, &r)) fail("S0000");
    else {
        unsigned sel = r.w.ax;
        memset(&r, 0, sizeof(r));
        r.w.bx = sel;
        if (dpmi(0x0001, &r)) fail("S0001");
    }

    imr = (unsigned char)inp(0x21);
    outp(0x21, imr);
    if ((unsigned char)inp(0x21) != imr) fail("PIC");

    old_timer = _dos_getvect(0x08);
    _dos_setvect(0x08, timer);
    _enable();
    start = ticks;
    for (guard = 0; ticks < start + 4 && guard < 400000000UL; guard++) { }
    if (ticks < start + 4) fail("IRQ0");
    _disable();
    memset(&r, 0, sizeof(r));
    dpmi(0x0902, &r);
    if (r.h.al != 0) fail("VIF0");
    _enable();
    memset(&r, 0, sizeof(r));
    dpmi(0x0902, &r);
    if (r.h.al != 1) fail("VIF1");
    _dos_setvect(0x08, old_timer);

    /* INT 21h 2Ah, 2Ch, then 2Ah again (the extender's cached-date path,
     * which compares the tick count's midnight bits through selector 40h). */
    _dos_getdate(&date);
    _dos_gettime(&time);
    _dos_getdate(&again);
    if (date.year < 1980 || date.month < 1 || date.month > 12 || date.day < 1 || date.day > 31) fail("DATE");
    if (again.year != date.year || again.month != date.month || again.day != date.day) fail("DATE2");
    if (time.hour > 23 || time.minute > 59 || time.second > 59 || time.hsecond > 99) fail("TIME");

    f = fopen("C:\\DPMI4GW.TMP", "wb");
    if (!f) fail("FOPEN");
    else {
        fputs("Ciuki VMM DPMI", f);
        fclose(f);
        f = fopen("C:\\DPMI4GW.TMP", "rb");
        if (!f || !fgets(text, sizeof(text), f) || strcmp(text, "Ciuki VMM DPMI")) fail("FREAD");
        if (f) fclose(f);
        remove("C:\\DPMI4GW.TMP");
    }

    if (failures) {
        sprintf(text, "DPMI4GW FAIL %s %d", first, failures);
        line(text);
        return 1;
    }
    line("DPMI4GW PASS");
    return 0;
}
