/* Research: SeaBIOS rel-1.16.3 kbd.c, dequeue_key / __process_key:
 * https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c
 * BDA: 417/418 flags, 41A/41C head/tail, 41E buffer, 480/482 bounds,
 * 496 enhanced flags, 497 LEDs. INT16 AH=11 sets ZF; AH=10 consumes a
 * translated word only. Ordinary releases are discarded by firmware.
 * Mouse protocol reference: mouse_15c2*, invoke_mouse_handler,
 * https://raw.githubusercontent.com/coreboot/seabios/master/src/mouse.c
 * https://raw.githubusercontent.com/coreboot/seabios/master/src/std/bda.h
 * (Pinned rel-1.16.3 mouse.c/bda.h could not be retrieved by the research
 * service; pinned-source confirmation and QEMU evidence remain lead gates.)
 * EBDA: far handler +22h, packet flags +26h/+27h, packet bytes +28h.
 * No SeaBIOS implementation is copied.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/sync.h>
#include <ciuki/biosvm.h>
#include <ciuki/fwinput.h>

static struct fwinput_decoder decoder;
static struct fwinput_backend_state backend;
static bool started;

void fwinput_decoder_loss(struct fwinput_decoder *d, unsigned lost)
{
    d->stats.loss += lost;
    d->stats.resyncs++;
    d->resync_pending = true;
    d->resync_tick = d->last_tick;
    d->count = d->head = d->mouse_count = d->pause = 0;
    d->e0 = false;
    d->buttons = 0;
    memset(d->held, 0, sizeof(d->held));
    memset(d->extended_held, 0, sizeof(d->extended_held));
}

static void event(struct fwinput_decoder *d, unsigned type, unsigned code, int32_t value, uint64_t tick)
{
    d->last_tick = tick;
    if (d->count == FWINPUT_CAPACITY)
        fwinput_decoder_loss(d, d->count);
    unsigned i = (d->head + d->count++) % FWINPUT_CAPACITY;
    d->events[i] = (struct fwinput_event){(uint16_t)type, (uint16_t)code, value, tick};
}

void fwinput_decode_scan(struct fwinput_decoder *d, uint8_t byte, uint64_t tick)
{
    d->last_tick = tick;
    /* This consumes an already-observed byte, never reads 60/64 itself.
     * The live INT16-only path cannot supply breaks; do not fabricate them. */
    static const uint8_t pause[] = {0x1D, 0x45, 0xE1, 0x9D, 0xC5};
    if (d->pause) {
        if (byte != pause[d->pause - 1]) {
            fwinput_decoder_loss(d, 1);
            return;
        }
        if (++d->pause == 6) {
            d->pause = 0;
            if (d->count > FWINPUT_CAPACITY - 2)
                fwinput_decoder_loss(d, d->count);
            event(d, FWINPUT_KEY, 0x145, 1, tick);
            event(d, FWINPUT_KEY, 0x145, 0, tick);
            d->stats.keys += 2;
        }
        return;
    }
    if (byte == 0xE1) {
        d->pause = 1;
        d->e0 = false;
        return;
    }
    if (byte == 0xE0) {
        d->e0 = true;
        return;
    }
    /* ACK/RESEND/error bytes are never key transitions. AA is also left
     * Shift's break, so BAT responses require command-context classification
     * by the qualified observer before this scan-only entry point. */
    if (byte == 0xFA || byte == 0xFE || byte == 0xFC || byte == 0xFD || byte == 0xFF || !byte) {
        d->e0 = false;
        return;
    }
    unsigned code = byte & 0x7F;
    bool e0 = d->e0;
    d->e0 = false;
    if (e0 && (code == 0x2A || code == 0x36))
        return; /* extended PrintScreen fake shift */
    bool *held = e0 ? &d->extended_held[code] : &d->held[code];
    bool down = !(byte & 0x80);
    if (*held == down)
        return; /* typematic is supplied as TEXT by INT16 */
    event(d, FWINPUT_KEY, code | (e0 ? 0x100 : 0), down, tick);
    *held = down; /* event overflow can have cleared the held-state arrays */
    d->stats.keys++;
}

void fwinput_decode_bios(struct fwinput_decoder *d, uint16_t ax, uint64_t tick)
{
    d->last_tick = tick;
    unsigned ascii = ax & 0xFF;
    /* AH is a BIOS-translated code, NOT a raw scan byte: e.g. F11=85h
     * is a make, not an 05h release. INT16 cannot establish transitions.
     * Keep text truthful until a qualified IRQ observer supplies scans. */
    if (ascii && ascii != 0xE0 && ascii != 0xF0) {
        event(d, FWINPUT_TEXT, 0, (int32_t)ascii, tick);
        d->stats.text++;
    }
}

void fwinput_decode_mouse(struct fwinput_decoder *d, uint8_t byte, uint64_t tick)
{
    d->last_tick = tick;
    if (!d->mouse_count && !(byte & 8)) {
        d->stats.malformed++;
        d->stats.resyncs++;
        return;
    }
    d->mouse_bytes[d->mouse_count++] = byte;
    if (d->mouse_count != 3)
        return;
    d->mouse_count = 0;
    uint8_t flags = d->mouse_bytes[0];
    if (flags & 0xC0) {
        d->stats.malformed++;
        fwinput_decoder_loss(d, 1);
        return;
    }
    /* Reserve a whole packet's worst case (two axes, three buttons) so
     * overflow cannot split transitions around a state reset. */
    if (d->count > FWINPUT_CAPACITY - 5)
        fwinput_decoder_loss(d, d->count);
    /* PS/2 deltas have NINE signed bits. 0x10/0x20 supply bit 8,
     * independently of bit 7 in the data byte. Screen Y points down. */
    int32_t x = d->mouse_bytes[1] - ((flags & 0x10) ? 256 : 0);
    int32_t y = d->mouse_bytes[2] - ((flags & 0x20) ? 256 : 0);
    if (x)
        event(d, FWINPUT_REL, FWINPUT_X, x, tick);
    if (y)
        event(d, FWINPUT_REL, FWINPUT_Y, -y, tick);
    uint8_t buttons = flags & 7;
    for (unsigned n = 0; n < 3; n++)
        if ((buttons ^ d->buttons) & (1u << n))
            event(d, FWINPUT_BUTTON, n, !!(buttons & (1u << n)), tick);
    d->buttons = buttons;
    d->stats.packets++;
}

unsigned fwinput_decode_poll(struct fwinput_decoder *d, struct fwinput_event *out, unsigned max)
{
    if (!out || !max)
        return 0;
    unsigned n = 0;
    if (d->resync_pending) {
        out[n++] = (struct fwinput_event){FWINPUT_RESYNC, 0, 0, d->resync_tick};
        d->resync_pending = false;
    }
    while (n < max && d->count) {
        out[n++] = d->events[d->head];
        d->head = (d->head + 1) % FWINPUT_CAPACITY;
        d->count--;
    }
    return n;
}

static void service_input(void)
{
    /* Bounded drain: BIOS buffer is 16 words on SeaBIOS. Every status and
     * consume is a separate bounded V86 call; AH=10 requires nonempty BDA. */
    if (backend.keyboard) {
        for (unsigned n = 0; n < 16; n++) {
            struct biosvm_regs r = {.eax = 0x1100, .interrupt = 0x16};
            if (biosvm_call(&r, 100) || (r.flags & V86_ZF))
                break;
            r.eax = 0x1000;
            if (biosvm_call(&r, 100))
                break;
            fwinput_decode_bios(&decoder, (uint16_t)r.eax, deadline_after_ms(0));
        }
    }
    if (backend.mouse) {
        uint8_t packets[BIOSVM_MOUSE_CAP][3];
        unsigned lost = 0;
        unsigned n = biosvm_mouse_packets(packets, BIOSVM_MOUSE_CAP, &lost);
        if (lost)
            fwinput_decoder_loss(&decoder, lost);
        for (unsigned i = 0; i < n; i++)
            for (unsigned j = 0; j < 3; j++)
                fwinput_decode_mouse(&decoder, packets[i][j], deadline_after_ms(0));
    }
}

static int mouse_setup(uint8_t function, uint16_t bx, uint16_t es, uint32_t ms)
{
    struct biosvm_regs r = {.eax = 0xC200u | function, .ebx = bx, .es = es, .interrupt = 0x15};
    int rc = biosvm_call(&r, ms);
    if (rc)
        return rc;
    if ((r.flags & V86_CF) || (r.eax & 0xFF00))
        return -ENOSYS;
    decoder.stats.mouse_functions |= 1u << function;
    return 0;
}

int fwinput_init(void)
{
    if (started)
        return biosvm_backend_state() == BIOSVM_DISABLED_BACKEND ? -V86_EIO : 0;
    int rc = biosvm_init();
    if (rc)
        return rc;
    started = true;
    backend.keyboard = true;
    backend.key_releases = false; /* directive's INT16 release premise is false */
    /* C205/BH=3 initializes packet length AND performs the documented reset.
     * C201 alone leaves packet length unspecified. One 500ms setup budget,
     * not a fresh 500ms per step. Unsupported mouse service leaves keyboard. */
    static const struct { uint8_t fn; uint16_t bx, es; } setup[] = {
        {5, 0x0300, 0}, {2, 0x0500, 0}, {3, 0x0200, 0},
        {7, BIOSVM_MOUSE_OFFSET, BIOSVM_SCRATCH >> 4}, {0, 0x0100, 0},
    };
    uint64_t end = deadline_after_ms(500);
    for (unsigned i = 0; i < ARRAY_SIZE(setup); i++) {
        uint64_t now = deadline_after_ms(0);
        if (now - end < (1ull << 63)) {
            rc = -V86_ETIMEDOUT;
            break;
        }
        rc = mouse_setup(setup[i].fn, setup[i].bx, setup[i].es, (uint32_t)(end - now));
        if (rc)
            break;
    }
    backend.mouse = !rc;
    backend.setup_error = rc;
    biosvm_set_input_service(service_input);
    return biosvm_backend_state() == BIOSVM_DISABLED_BACKEND ? -V86_EIO : 0;
}

unsigned fwinput_poll(struct fwinput_event *out, unsigned max)
{
    /* The producer is the same non-preemptible kernel worker. It only
     * switches in V86, before returning and publishing decoded events. */
    if (!backend.disabled && biosvm_backend_state() == BIOSVM_DISABLED_BACKEND) {
        backend.disabled = true;
        decoder.last_tick = deadline_after_ms(0);
        fwinput_decoder_loss(&decoder, decoder.count);
    }
    return fwinput_decode_poll(&decoder, out, max);
}

void fwinput_stats(struct fwinput_stats *out) { *out = decoder.stats; }

void fwinput_backend_state(struct fwinput_backend_state *out)
{
    *out = backend;
    out->disabled = biosvm_backend_state() == BIOSVM_DISABLED_BACKEND;
    if (out->disabled)
        out->keyboard = out->mouse = false;
}
