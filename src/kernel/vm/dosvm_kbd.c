/* Keyboard queue/focus machinery migrated from cvgp; BIOS stays synthetic.
 * Research: f3-acceptance.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

static void kbd_signal(struct dosvm *v)
{
    if (v->kbd.count && (v->kbd.command & 1)) dosvm_irq_raise(v->info.key, 1);
}
static bool kbd_queue(struct dosvm *v, const uint8_t *bytes, unsigned n)
{
    struct dosvm_kbd *k = &v->kbd;
    if (n > DOSVM_KBD_BYTES - k->count) { k->overflows++; return false; }
    for (unsigned i = 0; i < n; i++) k->bytes[(k->head + k->count++) % DOSVM_KBD_BYTES] = bytes[i];
    kbd_signal(v);
    return true;
}
static uint8_t kbd_pop(struct dosvm *v)
{
    struct dosvm_kbd *k = &v->kbd;
    if (!k->count) return 0xFF;
    uint8_t b = k->bytes[k->head];
    k->head = (k->head + 1) % DOSVM_KBD_BYTES; k->count--; k->last = b;
    k->reads++;
    v->monitor.pic[0].irr &= ~2u;
    kbd_signal(v);
    return b;
}
static bool kbd_reply(struct dosvm *v, uint8_t b) { return kbd_queue(v, &b, 1); }
void dosvm_kbd_init(struct dosvm *v)
{
    memset(&v->kbd, 0, sizeof(v->kbd));
    v->kbd.command = 0x41; v->kbd.output = 3;
    v->kbd.scanning = v->kbd.enabled = true; v->kbd.scan_set = 1;
}
static bool kbd_emit(struct dosvm *v, unsigned code, bool down)
{
    uint8_t b[6]; unsigned n = 0;
    if (code == 0x200) {
        if (!down) return true;
        const uint8_t pause[] = {0xE1,0x1D,0x45,0xE1,0x9D,0xC5};
        return kbd_queue(v, pause, sizeof(pause));
    }
    if (code == 0x137) {
        if (down) { b[n++] = 0xE0; b[n++] = 0x2A; b[n++] = 0xE0; b[n++] = 0x37; }
        else { b[n++] = 0xE0; b[n++] = 0xB7; b[n++] = 0xE0; b[n++] = 0xAA; }
    } else {
        if (code & 0x100) b[n++] = 0xE0;
        b[n++] = (code & 127) | (down ? 0 : 0x80);
    }
    return kbd_queue(v, b, n);
}
bool dosvm_kbd_event(struct dosvm *v, unsigned code, int value)
{
    struct dosvm_kbd *k = &v->kbd;
    if (value < 0 || value > 2 || (code != 0x200 &&
        (!code || code >= 0x180 || (code & 0x80) || !(code & 127)))) return false;
    if (code == 0x200) return value != 2 && kbd_emit(v, code, value == 1);
    unsigned bit = 1u << (code & 7);
    bool held = k->held[code >> 3] & bit;
    if ((value == 1 && held) || (!value && !held)) return true;
    if (value == 2 && !held) return false;
    if (k->enabled && k->scanning && !kbd_emit(v, code, value != 0)) return false;
    if (value) k->held[code >> 3] |= bit; else k->held[code >> 3] &= ~bit;
    return true;
}
void dosvm_kbd_release(struct dosvm *v)
{
    /* Whole sequences fit even with all valid ordinary/extended keys held. */
    v->kbd.head = v->kbd.count = 0;
    v->monitor.pic[0].irr &= ~2u;
    v->kbd.bios_prefix = 0;
    for (unsigned code = 1; code < 512; code++) if (v->kbd.held[code >> 3] & (1u << (code & 7)))
        kbd_emit(v, code, false);
    memset(v->kbd.held, 0, sizeof(v->kbd.held));
}
int dosvm_kbd_port(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{
    (void)arg;
    if (width != 1) return -V86_EPERM;
    struct dosvm_kbd *k = &v->kbd;
    uint8_t b = *value;
    if (!write) {
        *value = port == 0x64 ? (k->count ? 1 : 0) | 4 | (k->controller_pending ? 8 : 0) : kbd_pop(v);
        return 0;
    }
    if (port == 0x64) {
        k->controller_pending = 0;
        switch (b) {
        case 0x20: return kbd_reply(v, k->command) ? 0 : -ENOSPC;
        case 0x60: case 0xD1: case 0xD2: k->controller_pending = b; return 0;
        case 0xAA: return kbd_reply(v, 0x55) ? 0 : -ENOSPC;
        case 0xAB: return kbd_reply(v, 0) ? 0 : -ENOSPC;
        case 0xAD: k->enabled = false; k->command |= 0x10; return 0;
        case 0xAE: k->enabled = true; k->command &= ~0x10u; kbd_signal(v); return 0;
        case 0xA7: case 0xA8: return 0; /* AUX absent; INT33 is the mouse */
        case 0xD0: return kbd_reply(v, (k->output & ~2u) | (v->a20 ? 2 : 0)) ? 0 : -ENOSPC;
        case 0xFE: return dosvm_stop(v->info.key, DOSVM_RESET);
        default: return -V86_EPERM;
        }
    }
    if (k->controller_pending) {
        uint8_t cmd = k->controller_pending; k->controller_pending = 0;
        if (cmd == 0x60) { k->command = b; k->enabled = !(b & 0x10); kbd_signal(v); return 0; }
        if (cmd == 0xD2) return kbd_reply(v, b) ? 0 : -ENOSPC;
        k->output = b;
        if (!(b & 1)) return dosvm_stop(v->info.key, DOSVM_RESET);
        return dosvm_set_a20(v, b & 2);
    }
    if (k->keyboard_pending) {
        uint8_t cmd = k->keyboard_pending; k->keyboard_pending = 0;
        if (cmd == 0xF0 && b && b != 1) return kbd_reply(v, 0xFE) ? 0 : -ENOSPC;
        uint8_t reply[] = {0xFA, 1};
        if (cmd == 0xED) k->leds = b & 7; /* no physical LEDs */
        return kbd_queue(v, reply, cmd == 0xF0 && !b ? 2 : 1) ? 0 : -ENOSPC;
    }
    uint8_t reply[3] = {0xFA,0xAB,0x83}; unsigned n = 1;
    switch (b) {
    case 0xED: case 0xF0: case 0xF3: k->keyboard_pending = b; break;
    case 0xEE: reply[0] = 0xEE; break;
    case 0xF2: n = 3; break;
    case 0xF4: k->scanning = true; break;
    case 0xF5: dosvm_kbd_release(v); k->scanning = false; break;
    case 0xF6: k->scanning = true; k->leds = 0; break;
    case 0xFF: dosvm_kbd_release(v); k->scanning = true; reply[1] = 0xAA; n = 2; break;
    case 0xFE: reply[0] = k->last; break;
    default: reply[0] = 0xFE; break;
    }
    return kbd_queue(v, reply, n) ? 0 : -ENOSPC;
}
/* IBM PC AT INT 16h words for the synthetic INT 9 translation (f3-02,
 * from the window adapter it replaces): letters, Shift/Caps/Ctrl/Alt,
 * function keys with modifiers, F11/F12, Alt digits, Ctrl navigation and
 * punctuation, NumLock keypad, keypad Enter and slash. Grey keys use the 00h
 * form INT 16h AH=00h returns. https://stanislavs.org/helppc/scan_codes.html
 * s is the BIOS shift byte (0417h): 1/2 Shift, 4 Ctrl, 8 Alt, 20h Num, 40h Caps. */
static const char key_plain[0x3A] = {
    [0x01]=27, [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5', [0x07]='6',
    [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0', [0x0C]='-', [0x0D]='=', [0x0E]=8,
    [0x0F]=9, [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t', [0x15]='y',
    [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p', [0x1A]='[', [0x1B]=']', [0x1C]=13,
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g', [0x23]='h', [0x24]='j',
    [0x25]='k', [0x26]='l', [0x27]=';', [0x28]='\'', [0x29]='`', [0x2B]='\\', [0x2C]='z',
    [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b', [0x31]='n', [0x32]='m', [0x33]=',',
    [0x34]='.', [0x35]='/', [0x37]='*', [0x39]=' ',
};
static const char key_shift[0x3A] = {
    [0x01]=27, [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%', [0x07]='^',
    [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')', [0x0C]='_', [0x0D]='+', [0x0E]=8,
    [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T', [0x15]='Y', [0x16]='U',
    [0x17]='I', [0x18]='O', [0x19]='P', [0x1A]='{', [0x1B]='}', [0x1C]=13, [0x1E]='A',
    [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G', [0x23]='H', [0x24]='J', [0x25]='K',
    [0x26]='L', [0x27]=':', [0x28]='"', [0x29]='~', [0x2B]='|', [0x2C]='Z', [0x2D]='X',
    [0x2E]='C', [0x2F]='V', [0x30]='B', [0x31]='N', [0x32]='M', [0x33]='<', [0x34]='>',
    [0x35]='?', [0x37]='*', [0x39]=' ',
};
static uint16_t navigation(unsigned scan, bool ctrl)
{
    static const uint8_t ctrl_scan[13] = {0x77, 0x8D, 0x84, 0x8E, 0x73, 0x8F, 0x74, 0x90,
                                          0x75, 0x91, 0x76, 0x92, 0x93};
    if (scan < 0x47 || scan > 0x53) return 0;
    if (scan == 0x4A) return ctrl ? 0x8E00 : 0x4A2D;
    if (scan == 0x4E) return ctrl ? 0x9000 : 0x4E2B;
    return (uint16_t)((ctrl ? ctrl_scan[scan - 0x47] : scan) << 8);
}
static uint16_t bios_word(unsigned scan, bool extended, uint8_t s)
{
    bool shift = s & 3, ctrl = s & 4, alt = s & 8;
    if (extended) {
        if (scan == 0x1C) return ctrl ? 0x1C0A : 0x1C0D;        /* keypad Enter */
        if (scan == 0x35) return 0x352F;                        /* keypad / */
        return navigation(scan, ctrl);
    }
    if (scan >= 0x3B && scan <= 0x44) {                         /* F1-F10 */
        unsigned base = alt ? 0x68 : ctrl ? 0x5E : shift ? 0x54 : 0x3B;
        return (uint16_t)((base + scan - 0x3B) << 8);
    }
    if (scan == 0x57 || scan == 0x58) {                         /* F11, F12 */
        unsigned base = alt ? 0x8B : ctrl ? 0x89 : shift ? 0x87 : 0x85;
        return (uint16_t)((base + scan - 0x57) << 8);
    }
    if (scan >= 0x47 && scan <= 0x53 && scan != 0x4A && scan != 0x4E) {
        static const char digits[] = "789-456+1230.";
        bool number = ((s & 0x20) != 0) != shift;
        return number && !ctrl && !alt ? (uint16_t)(scan << 8 | (uint8_t)digits[scan - 0x47]) : navigation(scan, ctrl);
    }
    if (scan == 0x4A || scan == 0x4E) return navigation(scan, ctrl);
    if (scan >= sizeof(key_plain) || !key_plain[scan]) return 0;
    char c = key_plain[scan];
    if (alt) {
        if (scan >= 0x02 && scan <= 0x0D) return (uint16_t)((scan + 0x76) << 8); /* Alt 1..= */
        return (uint16_t)(scan << 8);
    }
    if (ctrl) {
        if (c >= 'a' && c <= 'z') return (uint16_t)(scan << 8 | (c - 'a' + 1));
        switch (scan) {
        case 0x1C: return 0x1C0A; case 0x0E: return 0x0E7F; case 0x03: return 0x0300;
        case 0x07: return 0x071E; case 0x0C: return 0x0C1F; case 0x1A: return 0x1A1B;
        case 0x2B: return 0x2B1C; case 0x1B: return 0x1B1D; default: return 0;
        }
    }
    if (scan == 0x0F) return shift ? 0x0F00 : 0x0F09;
    bool letter = c >= 'a' && c <= 'z';
    bool upper = letter ? shift != ((s & 0x40) != 0) : shift;
    return (uint16_t)(scan << 8 | (uint8_t)(upper ? key_shift[scan] : c));
}
/* f3-02: a key byte waits in the 8042 with IRQ1 requested, and the virtual
 * PIC would deliver an interrupt to an enabled guest now. The guest's INT 9
 * (the ROM stub below, or a DOS/TSR hook chained to it) is the only path from
 * the 8042 to the BIOS buffer, so a blocking INT 16h must let it run. */
bool dosvm_kbd_irq_ready(const struct dosvm *v)
{
    return v->kbd.count && (v->monitor.pic[0].irr & 2u) && v86_irq_pending(&v->monitor) >= 0;
}
int dosvm_kbd_bios(struct dosvm *v)
{
    struct dosvm_kbd *k = &v->kbd;
    if (!k->count) return 0;
    uint8_t b = kbd_pop(v);
    if (b == 0xE0) { k->bios_prefix = 1; return 0; }
    if (b == 0xE1) { k->bios_prefix = 6; return 0; }
    if (k->bios_prefix > 1) { if (--k->bios_prefix == 1) { k->bios_prefix = 0; dosvm_keyboard(v->info.key,0x4500); } return 0; }
    bool extended = k->bios_prefix; k->bios_prefix = 0;
    unsigned scan = b & 127;
    bool up = b & 0x80;
    uint8_t mask = !extended && scan == 0x2A ? 2 : !extended && scan == 0x36 ? 1 : scan == 0x1D ? 4 : scan == 0x38 ? 8 : 0;
    if (mask) { if (up) k->shift &= ~mask; else k->shift |= mask; }
    else if (!up && !extended && scan == 0x3A) k->shift ^= 0x40;   /* Caps Lock */
    else if (!up && !extended && scan == 0x45) k->shift ^= 0x20;   /* Num Lock */
    else if (!up && !extended && scan == 0x46) k->shift ^= 0x10;   /* Scroll Lock */
    else if (!up && b < 0x80 && !(extended && scan == 0x2A)) {
        uint16_t word = bios_word(scan, extended, k->shift);
        if (word) dosvm_keyboard(v->info.key, word);
    }
    return dosvm_write(v, 0x417, &k->shift, 1);
}
