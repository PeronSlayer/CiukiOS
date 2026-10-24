/* Formatted fatal-error screen with a restart choice (G13, ER-2).
 * Text follows the Windows 98 blue screen quoted in the Resource Kit
 * (Ch. 10, "A fatal exception 0E has occurred at 0028:C3C64C51 in VXD ...
 * The current application will be terminated", wrkc10_9.htm); the choice
 * is Ctrl+Alt+Del as there. Scancodes: IBM PS/2 Technical Reference, set 2
 * (make E0 71 = Delete, 14 = Ctrl, 11 = Alt, 5A = Enter, 76 = Esc) and its
 * set 1 translation (1D, 38, E0 53, 1C, 01).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/fatal.h>
#include <ciuki/kernel.h>
#ifndef FATAL_HOST
#include <ciuki/cpu.h>
#include <ciuki/reboot.h>
#endif

/* The kernel formatter has no precision: bounded copies stand in for %.Ns. */
static void clip(char *out, size_t cap, const char *s)
{
    size_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        out[i] = c >= 32 && c < 127 ? (char)c : '?';
    }
    out[i] = 0;
}

static const char *vector_name(uint32_t v)
{
    switch (v) {
    case 0: return "divide error";
    case 1: return "debug exception";
    case 2: return "non-maskable interrupt";
    case 3: return "breakpoint";
    case 4: return "overflow";
    case 5: return "bound range exceeded";
    case 6: return "invalid opcode";
    case 7: return "device not available";
    case 8: return "double fault";
    case 10: return "invalid TSS";
    case 11: return "segment not present";
    case 12: return "stack fault";
    case 13: return "general protection fault";
    case 14: return "page fault";
    case 16: return "floating-point error";
    case 17: return "alignment check";
    case 18: return "machine check";
    case 19: return "SIMD floating-point exception";
    default: return "exception";
    }
}

size_t fatal_format(const struct fatal_report *r, char *out, size_t cap)
{
    if (!cap) return 0;
    size_t n = 0;
    int k;
    #define PUT(...) do { k = ksnprintf(out + n, cap - n, __VA_ARGS__); \
        if (k < 0 || (size_t)k >= cap - n) { out[cap - 1] = 0; return cap - 1; } n += (size_t)k; } while (0)
    PUT("\n  CiukiOS\n\n");
    char task[25], reason[121];
    clip(task, sizeof(task), r->task);
    if (!task[0]) clip(task, sizeof(task), "boot");
    clip(reason, sizeof(reason), r->reason);
    if (!reason[0]) clip(reason, sizeof(reason), "unknown");
    if (r->frame) {
        PUT("  A fatal exception %02X has occurred at %04X:%08X in task \"%s\".\n",
            r->vector & 0xff, r->cs & 0xffff, r->eip, task);
        PUT("  %s", vector_name(r->vector));
        if (r->vector == 14)
            PUT(": %s of %s address %08X", r->error & 2 ? "write" : "read",
                r->error & 1 ? "protected" : "unmapped", r->cr2);
        else if (r->vector == 13 || r->vector == 10 || r->vector == 11 || r->vector == 12)
            PUT(": selector %04X", r->error & 0xffff);
        PUT(" (error code %08X).\n", r->error);
    } else {
        PUT("  A fatal error has occurred in task \"%s\":\n  %s\n", task, reason);
    }
    PUT("  The system cannot continue.\n\n");
    PUT("  *  Press ENTER or CTRL+ALT+DEL to restart your computer. You will\n");
    PUT("     lose any unsaved information in all programs.\n");
    PUT("  *  Press ESC to keep this screen and stop here.\n");
    #undef PUT
    return n;
}

/* Set 2 (default) and set 1 (translated) codes of the keys that matter. */
enum fatal_choice fatal_key_feed(struct fatal_keys *k, uint8_t b)
{
    if (b == 0xE0) { k->extended = true; return FATAL_NONE; }
    bool release;
    uint8_t code = b;
    if (k->translated) {
        release = b & 0x80;
        code = b & 0x7f;
    } else {
        if (b == 0xF0) { k->release = true; return FATAL_NONE; }
        release = k->release;
        k->release = false;
    }
    bool ext = k->extended;
    k->extended = false;
    uint8_t ctrl = k->translated ? 0x1d : 0x14, alt = k->translated ? 0x38 : 0x11;
    uint8_t del = k->translated ? 0x53 : 0x71, enter = k->translated ? 0x1c : 0x5a;
    uint8_t esc = k->translated ? 0x01 : 0x76;
    if (code == ctrl) { k->ctrl = !release; return FATAL_NONE; }
    if (code == alt) { k->alt = !release; return FATAL_NONE; }
    if (release) return FATAL_NONE;
    if (code == del && ext) return k->ctrl && k->alt ? FATAL_RESTART : FATAL_NONE;
    if (code == enter) return FATAL_RESTART;
    if (code == esc && !ext) return FATAL_STAY;
    return FATAL_NONE;
}

#ifndef FATAL_HOST
static bool wait_status(uint8_t mask, bool set)
{
    for (unsigned n = 0; n < 20000; n++) {
        uint8_t s = inb(0x64);
        if (s == 0xff) return false;            /* no controller */
        if (!!(s & mask) == set) return true;
        outb(0x80, 0);
    }
    return false;
}

/* Command byte bit 6: does the controller translate to set 1? Bounded; on any
 * doubt the kernel driver's untranslated set 2 is assumed. */
static bool controller_translates(void)
{
    while (inb(0x64) != 0xff && (inb(0x64) & 1)) (void)inb(0x60);
    if (!wait_status(2, false)) return false;
    outb(0x64, 0x20);
    if (!wait_status(1, true)) return false;
    return inb(0x60) & 0x40;
}

__attribute__((noreturn)) void fatal_screen(const struct fatal_report *r)
{
    char text[FATAL_SCREEN_BYTES];
    size_t n = fatal_format(r, text, sizeof(text));
    /* Scrolls the log off, then draws the screen once. */
    static const char clear[] =
        "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
        "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
        "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n";
    console_set_color(0xFFFFFF, 0x0000AA);
    console_write(clear, sizeof(clear) - 1);
    console_write(text, n);
    if (inb(0x64) == 0xff) for (;;) { cli(); hlt(); }
    struct fatal_keys keys = { .translated = controller_translates() };
    for (;;) {
        uint8_t status = inb(0x64);
        if (!(status & 1) || (status & 0x20)) {    /* nothing, or mouse data */
            if (status & 1) (void)inb(0x60);
            outb(0x80, 0);
            continue;
        }
        enum fatal_choice c = fatal_key_feed(&keys, inb(0x60));
        if (c == FATAL_RESTART) kernel_reboot();
        if (c == FATAL_STAY) for (;;) { cli(); hlt(); }
    }
}
#endif
