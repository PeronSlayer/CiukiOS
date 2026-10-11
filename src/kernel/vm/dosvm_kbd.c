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
static const char kbd_ascii[128] = {
    [1]=27, [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',
    [12]='-',[13]='=',[14]=8,[15]=9,[16]='q',[17]='w',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',
    [24]='o',[25]='p',[26]='[',[27]=']',[28]=13,[30]='a',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',
    [36]='j',[37]='k',[38]='l',[39]=';',[40]='\'',[41]='`',[43]='\\',[44]='z',[45]='x',[46]='c',
    [47]='v',[48]='b',[49]='n',[50]='m',[51]=',',[52]='.',[53]='/',[55]='*',[57]=' '
};
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
    else if (!up && scan == 0x3A) k->shift ^= 0x40;
    else if (!up && b < 0x80 && !(extended && scan == 0x2A)) {
        uint8_t ch = extended ? 0 : kbd_ascii[scan];
        if (ch >= 'a' && ch <= 'z') {
            if (k->shift & 4) ch -= 'a' - 1;
            else if (!!(k->shift & 3) != !!(k->shift & 0x40)) ch -= 'a' - 'A';
        } else if (k->shift & 3) {
            const char *normal = "1234567890-=[];'`,./\\", *shifted = "!@#$%^&*()_+{}:\"~<>?|";
            for (unsigned i = 0; normal[i]; i++) if (ch == (uint8_t)normal[i]) { ch = shifted[i]; break; }
        }
        if (k->shift & 8) ch = 0;
        dosvm_keyboard(v->info.key, (scan << 8) | ch);
    }
    return dosvm_write(v, 0x417, &k->shift, 1);
}
