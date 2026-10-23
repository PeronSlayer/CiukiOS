/* SPDX-License-Identifier: GPL-2.0-only
 * Ciuki adapter: bounded queries, old/new Synaptics and ALPS v1/v2 packets.
 * Algorithms/constants adapted from Linux v6.12 drivers/input/mouse/
 * synaptics.c, synaptics.h, alps.c and alps.h. Full origin/header manifest:
 * docs/licenses/linux-v6.12-touchpad-NOTICES.txt.
 *
 * Synaptics TouchPad PS/2 mouse driver
 *   2003 Dmitry Torokhov <dtor@mail.ru>
 *     Added support for pass-through port. Special thanks to Peter Berg Larsen
 *     for explaining various Synaptics quirks.
 *   2003 Peter Osterlund <petero2@telia.com>
 *     Ported to 2.5 input device infrastructure.
 *   Copyright (C) 2001 Stefan Gmeiner <riddlebox@freesurf.ch>
 *     start merging tpconfig and gpm code to a xfree-input module
 *     adding some changes and extensions (ex. 3rd and 4th button)
 *   Copyright (c) 1997 C. Scott Ananian <cananian@alumni.priceton.edu>
 *   Copyright (c) 1998-2000 Bruce Kalk <kall@compass.com>
 *     code for the special synaptics commands (from the tpconfig-source)
 * Trademarks are the property of their respective owners.
 *
 * ALPS touchpad PS/2 mouse driver
 * Copyright (c) 2003 Neil Brown <neilb@cse.unsw.edu.au>
 * Copyright (c) 2003-2005 Peter Osterlund <petero2@telia.com>
 * Copyright (c) 2004 Dmitry Torokhov <dtor@mail.ru>
 * Copyright (c) 2005 Vojtech Pavlik <vojtech@suse.cz>
 * Copyright (c) 2009 Sebastian Kapfer <sebastian_kapfer@gmx.net>
 * ALPS detection, tap switching and status querying info is taken from
 * tpconfig utility (by C. Scott Ananian and Bruce Kall).
 * alps.h: Copyright (c) 2003 Peter Osterlund <petero2@telia.com>
 *         Copyright (c) 2005 Vojtech Pavlik <vojtech@suse.cz>
 *
 * Modified for Ciuki: fixed storage, existing transport/leases, host gestures,
 * internal absolute samples and relative desktop/DOS projection. No Linux
 * serio/input core, DMI matching, device resets or dynamic allocation.
 */

#define TP_ALPS_DUAL 0x02u
#define TP_ALPS_PASS 0x04u
#define TP_ALPS_WHEEL 0x08u
#define TP_ALPS_BACK1 0x10u
#define TP_ALPS_BACK2 0x20u
#define TP_ALPS_FOUR 0x40u
#define TP_ALPS_INTERLEAVED 0x80u

/* Exact Linux signatures; later protocol versions are deliberately absent. */
static const struct {
    uint8_t signature[3], version, byte0, mask0;
    uint16_t flags;
} touchpad_alps_models[] = {
    {{0x20,0x02,0x0e},2,0xf8,0xf8,0x06},
    {{0x22,0x02,0x0a},2,0xf8,0xf8,0x06},
    {{0x22,0x02,0x14},2,0xff,0xff,0x06},
    {{0x32,0x02,0x14},2,0xf8,0xf8,0x06},
    {{0x33,0x02,0x0a},1,0x88,0xf8,0},
    {{0x52,0x01,0x14},2,0xff,0xff,0x86},
    {{0x53,0x02,0x0a},2,0xf8,0xf8,0},
    {{0x53,0x02,0x14},2,0xf8,0xf8,0},
    {{0x60,0x03,0xc8},2,0xf8,0xf8,0},
    {{0x62,0x02,0x14},2,0xcf,0xcf,0x86},
    {{0x63,0x02,0x0a},2,0xf8,0xf8,0},
    {{0x63,0x02,0x14},2,0xf8,0xf8,0},
    {{0x63,0x02,0x28},2,0xf8,0xf8,0x20},
    {{0x63,0x02,0x3c},2,0x8f,0x8f,0x08},
    {{0x63,0x02,0x50},2,0xef,0xef,0x10},
    {{0x63,0x02,0x64},2,0xf8,0xf8,0},
    {{0x63,0x03,0xc8},2,0xf8,0xf8,0x06},
    {{0x73,0x00,0x0a},2,0xf8,0xf8,0x02},
    {{0x73,0x02,0x0a},2,0xf8,0xf8,0},
    {{0x73,0x02,0x14},2,0xf8,0xf8,0x20},
    {{0x73,0x02,0x50},2,0xcf,0xcf,0x40},
};

static uint32_t touchpad_u24(const uint8_t p[3])
{ return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

static int touchpad_command(struct controller *c, uint8_t cmd, uint64_t end)
{ return init_device(c, true, cmd, end, 0); }

static int touchpad_reply(struct controller *c, uint8_t cmd, uint8_t p[3], uint64_t end)
{
    init_command(c, cmd);
    return device_exchange_data(c, true, cmd, end, p, 3);
}

static int touchpad_sliced(struct controller *c, uint8_t value, uint64_t end)
{
    int err = touchpad_command(c, 0xE6, end);
    for (int shift = 6; !err && shift >= 0; shift -= 2) {
        err = touchpad_command(c, 0xE8, end);
        if (!err) err = touchpad_command(c, (value >> shift) & 3, end);
    }
    return err;
}

static int touchpad_syn_query(struct controller *c, uint8_t query, uint8_t p[3], uint64_t end)
{
    int err = touchpad_sliced(c, query, end);
    return err ? err : touchpad_reply(c, 0xE9, p, end);
}

static int touchpad_syn_mode(struct controller *c, uint8_t mode, uint64_t end)
{
    int err = touchpad_sliced(c, mode, end);
    if (!err) err = touchpad_command(c, 0xF3, end);
    if (!err) err = touchpad_command(c, 20, end);
    return err;
}

static int touchpad_alps_report(struct controller *c, uint8_t repeat,
                                 uint8_t p[3], uint64_t end, bool resolution)
{
    int err = 0;
    if (resolution) {
        err = touchpad_command(c, 0xE8, end);
        if (!err) err = touchpad_command(c, 0, end);
    }
    for (unsigned i = 0; !err && i < 3; i++) err = touchpad_command(c, repeat, end);
    return err ? err : touchpad_reply(c, 0xE9, p, end);
}

static int touchpad_alps_pass(struct controller *c, bool enable, uint64_t end)
{
    int err = 0;
    for (unsigned i = 0; !err && i < 3; i++)
        err = touchpad_command(c, enable ? 0xE7 : 0xE6, end);
    if (!err) err = touchpad_command(c, 0xF5, end);
    /* The donor permits three extra bytes after this command. Keep them
     * outside the next ACK/data transaction, with a bounded quiet wait. */
    uint64_t quiet = c->io->now(c->io_arg) + 100;
    struct poll_deadline d = limit(c, end, 100);
    while (!err && c->io->now(c->io_arg) < quiet) {
        err = poll_check(c, &d);
        if (!err) { capture_one(c, true); poll_pause(c, &d); }
    }
    return err;
}

static int touchpad_probe(struct controller *c, uint64_t end)
{
    uint8_t p[3];
    int err = touchpad_syn_query(c, 0, p, end);
    if (err) return err;
    if (p[1] == 0x47) {
        c->touchpad.info.identity = touchpad_u24(p);
        unsigned major = p[2] & 15;
        if (major < 3 || major > 6 || (major == 3 && p[0] < 2)) return -ENOSYS;
        err = touchpad_syn_query(c, 3, p, end);
        if (err) return err;
        c->touchpad.info.model = touchpad_u24(p);
        /* Pen and simplified devices need a separate policy. */
        if (p[2] & 0x60) return -ENOSYS;
        err = touchpad_syn_query(c, 2, p, end);
        if (err) return err;
        if (p[1] != 0x47) return -I8042_EPROTO;
        uint32_t caps = touchpad_u24(p);
        if (!(caps & 0x800000)) caps = 0;
        c->touchpad.info.capabilities = caps;
        c->touchpad.info.pass_through = !!(caps & 0x80);
        c->touchpad.info.two_finger = !!(caps & 2);
        c->touchpad.info.palm_detect = !!(caps & 1);
        uint8_t mode = 0x80 | (major >= 4 ? 4 : 0) | (caps ? 1 : 0);
        err = touchpad_syn_mode(c, mode, end);
        if (err) return err;
        err = touchpad_syn_query(c, 1, p, end);
        if (err) return err;
        if (p[1] != 0x47 || p[2] != mode) return -I8042_EPROTO;
        c->touchpad.info.protocol = c->touchpad.info.model & 0x80 ?
            TOUCHPAD_SYNAPTICS_NEW : TOUCHPAD_SYNAPTICS_OLD;
        c->touchpad.info.x_min = 1472; c->touchpad.info.x_max = 5472;
        c->touchpad.info.y_min = 1408; c->touchpad.info.y_max = 4448;
        if (c->touchpad.info.pass_through) {
            /* Force a standard three-byte client, with defaults/taps intact.
             * Tunnel F6 and F4 through the pad; no physical-controller reset. */
            const uint8_t commands[] = {0xF6, 0xF4};
            for (unsigned i = 0; !err && i < ARRAY_SIZE(commands); i++) {
                err = touchpad_sliced(c, commands[i], end);
                if (!err) err = touchpad_command(c, 0xF3, end);
                if (!err) err = touchpad_command(c, 40, end);
            }
        }
        return err;
    }
    err = touchpad_alps_report(c, 0xE6, p, end, true);
    if (err) return err;
    if ((p[0] & 0xF8) || p[1] || (p[2] != 10 && p[2] != 100)) return 0;
    err = touchpad_alps_report(c, 0xE7, p, end, true);
    if (err) return err;
    c->touchpad.info.identity = touchpad_u24(p);
    unsigned index;
    for (index = 0; index < ARRAY_SIZE(touchpad_alps_models); index++)
        if (!memcmp(p, touchpad_alps_models[index].signature, 3)) break;
    if (index == ARRAY_SIZE(touchpad_alps_models))
        return p[0] == 0x73 ? -ENOSYS : 0;
    c->touchpad.flags = touchpad_alps_models[index].flags;
    c->touchpad.byte0 = touchpad_alps_models[index].byte0;
    c->touchpad.mask0 = touchpad_alps_models[index].mask0;
    c->touchpad.info.pass_through = !!(c->touchpad.flags & (TP_ALPS_PASS | TP_ALPS_DUAL));
    if (c->touchpad.flags & TP_ALPS_PASS) err = touchpad_alps_pass(c, true, end);
    /* Disable hardware gestures; one host policy avoids duplicate taps. */
    if (!err) err = touchpad_reply(c, 0xE9, p, end);
    if (!err) err = touchpad_command(c, 0xF5, end);
    if (!err) err = touchpad_command(c, 0xF5, end);
    if (!err) err = touchpad_command(c, 0xE8, end);
    if (!err) err = touchpad_command(c, 0, end);
    if (!err) err = touchpad_alps_report(c, 0xF5, p, end, false);
    for (unsigned i = 0; !err && i < 4; i++) err = touchpad_command(c, 0xF5, end);
    if (!err) err = touchpad_command(c, 0xF4, end);
    if (!err) err = touchpad_command(c, 0xF0, end);
    if (!err && (c->touchpad.flags & TP_ALPS_PASS)) err = touchpad_alps_pass(c, false, end);
    if (err) return err;
    c->touchpad.info.protocol = touchpad_alps_models[index].version == 1 ?
        TOUCHPAD_ALPS_V1 : TOUCHPAD_ALPS_V2;
    c->touchpad.info.x_max = 1023; c->touchpad.info.y_max = 767;
    return 0;
}

static int init_mouse_protocol(struct controller *c, uint64_t end)
{
    /* Never use the reserve for optional probing. Existing total/reply
     * budgets and lifecycle failures remain authoritative. */
    uint64_t now = c->io->now(c->io_arg);
    if (end - now <= 120) return init_mouse_relative(c, end);
    uint64_t probe_end = now + 340;
    if (probe_end > end - 120) probe_end = end - 120;
    int err = touchpad_probe(c, probe_end);
    if (!err && c->touchpad.info.protocol) {
        c->touchpad.synced = true; /* negotiated framing also admits an initial bare stick packet */
        return 0;
    }
    c->touchpad.info.fallback_error = err;
    /* Restore mode zero even for an unsupported Synaptics generation. */
    if ((c->touchpad.info.identity & 0xFF00) == 0x4700) {
        err = touchpad_syn_mode(c, 0, end);
        if (err) return err;
    }
    if (c->touchpad.flags & TP_ALPS_PASS) {
        err = touchpad_alps_pass(c, false, end);
        if (err) return err;
    }
    /* Defaults cancel partial query sequences. Do not assume ALPS has
     * left absolute mode: prove three-byte framing with a remote poll. */
    err = touchpad_command(c, 0xF6, end);
    if (!err) err = touchpad_command(c, 0xF5, end);
    if (!err) err = touchpad_command(c, 0xEA, end);
    uint8_t p[3];
    if (!err) err = touchpad_reply(c, 0xEB, p, end);
    if (!err && (p[0] & 0xC8) != 8) err = -I8042_EPROTO;
    if (err) return err;
    c->touchpad.info.protocol = TOUCHPAD_NONE;
    c->queue.mouse_pos = 0;
    return init_mouse_relative(c, end);
}

static const char *touchpad_name(const struct controller *c)
{
    static const char *const names[] = {0, "synaptics-old", "synaptics-new", "alps-v1", "alps-v2"};
    return names[c->touchpad.info.protocol];
}

void i8042_touchpad_snapshot(struct touchpad_info *out)
{
    if (!out) return;
    uint32_t f = irq_save();
    *out = native.touchpad.info;
    irq_restore(f);
}

static void touchpad_loss(struct controller *c)
{
    c->touchpad.contact = c->touchpad.tap = c->touchpad.scrolling = false;
    c->touchpad.rem_x = c->touchpad.rem_y = c->touchpad.scroll = 0;
    c->touchpad.synced = false;
    c->touchpad.tap_blocked = true;
}

static void touchpad_buttons(struct controller *c, uint64_t tick)
{
    struct input_queue *q = &c->queue;
    q->native_buttons = c->touchpad.pad_buttons | c->touchpad.stick_buttons;
    unsigned buttons = q->native_buttons;
    for (unsigned i = 0; i < 3; i++) if (q->usb_buttons[i]) buttons |= 1u << i;
    unsigned changed = q->stats.buttons ^ buttons;
    q->stats.buttons = buttons;
    for (unsigned i = 0; i < 5; i++)
        if (changed & (1u << i)) enqueue(c, INPUT_BTN, i, !!(buttons & (1u << i)), tick);
}

/* ALPS v1/v2 snapshots share the main buttons. As in alps_report_buttons,
 * keep the owner of a held button, but let either packet release it. */
static void touchpad_alps_buttons(struct controller *c, unsigned buttons, bool stick)
{
    uint8_t *own = stick ? &c->touchpad.stick_buttons : &c->touchpad.pad_buttons;
    uint8_t *other = stick ? &c->touchpad.pad_buttons : &c->touchpad.stick_buttons;
    for (unsigned i = 0; i < (stick ? 3u : 5u); i++) {
        unsigned mask = 1u << i;
        uint8_t *target = (*other & mask) ? other : own;
        *target = (uint8_t)((*target & ~mask) | (buttons & mask));
    }
}

static void touchpad_stick(struct controller *c, const uint8_t p[3], bool buttons, uint64_t tick)
{
    if ((p[0] & 0xC8) != 8) return; /* includes tunnel ACK/BAT, never motion */
    int x = p[1] ? (int)p[1] - ((p[0] & 0x10) ? 256 : 0) : 0;
    int y = p[2] ? (int)p[2] - ((p[0] & 0x20) ? 256 : 0) : 0;
    if (x) enqueue(c, INPUT_REL, INPUT_X, x, tick);
    if (y) enqueue(c, INPUT_REL, INPUT_Y, -y, tick);
    if (buttons) {
        if (c->touchpad.info.protocol >= TOUCHPAD_ALPS_V1) touchpad_alps_buttons(c, p[0] & 7, true);
        else c->touchpad.stick_buttons = p[0] & 7;
        touchpad_buttons(c, tick);
    }
    c->touchpad.tap = false;
}

static int touchpad_abs(int value) { return value < 0 ? -value : value; }

static void touchpad_sample(struct controller *c, int x, int y, int z, int w,
                              unsigned buttons, uint64_t tick)
{
    const struct touchpad_info *info = &c->touchpad.info;
    bool syn = info->protocol <= TOUCHPAD_SYNAPTICS_NEW;
    if (syn) { if (x > 8176) x -= 8192; if (y > 8176) y -= 8192; }
    /* Bounds are nominal for Synaptics; clamp edge/outside samples. */
    if (x < info->x_min) x = info->x_min;
    if (x > info->x_max) x = info->x_max;
    if (y < info->y_min) y = info->y_min;
    if (y > info->y_max) y = info->y_max;
    if (syn) y = info->y_min + info->y_max - y;
    bool palm = z >= (syn ? 200 : 120) || (info->palm_detect && w >= 12);
    bool contact = !palm && z >= (c->touchpad.contact ? 25 : 31);
    unsigned fingers = contact ? (info->two_finger && w <= 1 ? (unsigned)w + 2 : 1) : 0;
    int values[] = {x, y, z, (int)fingers, w};
    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) enqueue(c, INPUT_ABS, i, values[i], tick);
    if (syn) c->touchpad.pad_buttons = (uint8_t)buttons;
    else touchpad_alps_buttons(c, buttons, false);
    if (contact && !c->touchpad.contact) {
        c->touchpad.started = tick;
        c->touchpad.start_x = x; c->touchpad.start_y = y;
        c->touchpad.rem_x = c->touchpad.rem_y = c->touchpad.scroll = 0;
        c->touchpad.scrolling = fingers >= 2 || x >= info->x_max - (info->x_max - info->x_min) / 10;
        c->touchpad.tap = !c->touchpad.tap_blocked && !buttons && !c->touchpad.stick_buttons && !c->touchpad.scrolling;
    } else if (contact) {
        int dx = x - c->touchpad.x, dy = y - c->touchpad.y;
        /* Scale-independent thresholds: 2% tap travel, 1/30 pad per wheel
         * detent, 1/800 pad per pointer pixel. Preserve fractional motion. */
        if (touchpad_abs(x - c->touchpad.start_x) > (info->x_max - info->x_min) / 50 ||
            touchpad_abs(y - c->touchpad.start_y) > (info->y_max - info->y_min) / 50)
            c->touchpad.tap = false;
        if (fingers >= 2) { c->touchpad.scrolling = true; c->touchpad.tap = false; }
        if (c->touchpad.scrolling) {
            int step = (info->y_max - info->y_min) / 30;
            c->touchpad.scroll -= dy;
            int wheel = c->touchpad.scroll / step;
            c->touchpad.scroll %= step;
            if (wheel) enqueue(c, INPUT_REL, INPUT_WHEEL, wheel, tick);
        } else {
            int sx = (info->x_max - info->x_min) / 800;
            int sy = (info->y_max - info->y_min) / 800;
            if (!sx) sx = 1;
            if (!sy) sy = 1;
            c->touchpad.rem_x += dx; c->touchpad.rem_y += dy;
            if (c->touchpad.rem_x / sx) enqueue(c, INPUT_REL, INPUT_X, c->touchpad.rem_x / sx, tick);
            if (c->touchpad.rem_y / sy) enqueue(c, INPUT_REL, INPUT_Y, c->touchpad.rem_y / sy, tick);
            c->touchpad.rem_x %= sx; c->touchpad.rem_y %= sy;
        }
    } else if (c->touchpad.contact && !palm && !z && c->touchpad.tap &&
               !buttons && !c->touchpad.stick_buttons && tick - c->touchpad.started <= 180) {
        c->touchpad.pad_buttons |= 1;
        touchpad_buttons(c, tick);
        c->touchpad.pad_buttons &= ~1u;
    }
    if (buttons || palm || tick - c->touchpad.started > 180) c->touchpad.tap = false;
    if (!z) c->touchpad.tap_blocked = false;
    c->touchpad.x = x; c->touchpad.y = y; c->touchpad.contact = contact;
    touchpad_buttons(c, tick);
}

static void touchpad_packet(struct controller *c, uint64_t tick)
{
    const uint8_t *p = c->queue.packet;
    unsigned protocol = c->touchpad.info.protocol;
    int x, y, z, w = 5;
    unsigned buttons;
    if (protocol <= TOUCHPAD_SYNAPTICS_NEW) {
        if ((p[0] & 0xFC) == 0x84 && (p[3] & 0xCC) == 0xC4 &&
            c->touchpad.info.pass_through) {
            uint8_t stick[] = {p[1], p[4], p[5]};
            touchpad_stick(c, stick, true, tick);
            return;
        }
        buttons = p[0] & 3;
        if (protocol == TOUCHPAD_SYNAPTICS_NEW) {
            x = ((p[3] & 0x10) << 8) | ((p[1] & 15) << 8) | p[4];
            y = ((p[3] & 0x20) << 7) | ((p[1] & 0xF0) << 4) | p[5];
            z = p[2];
            w = ((p[0] & 0x30) >> 2) | ((p[0] & 4) >> 1) | ((p[3] & 4) >> 2);
            if ((c->touchpad.info.capabilities & 0x040000) && ((p[0] ^ p[3]) & 1)) buttons |= 4;
        } else {
            x = ((p[1] & 31) << 8) | p[2]; y = ((p[4] & 31) << 8) | p[5];
            z = ((p[0] & 0x30) << 2) | (p[3] & 63);
            w = ((p[1] & 0x80) >> 4) | ((p[0] & 4) >> 1);
        }
    } else {
        if (protocol == TOUCHPAD_ALPS_V1) {
            x = p[1] | ((p[0] & 7) << 7); y = p[4] | ((p[3] & 7) << 7);
            buttons = !!(p[2] & 0x10) | (!!(p[2] & 8) << 1);
        } else {
            x = p[1] | ((p[2] & 0x78) << 4); y = p[4] | ((p[3] & 0x70) << 3);
            buttons = p[3] & 7;
        }
        z = p[5];
        if ((c->touchpad.flags & TP_ALPS_DUAL) && z == 127) {
            x = x > 383 ? x - 768 : x; y = y > 255 ? y - 512 : y;
            if (x) enqueue(c, INPUT_REL, INPUT_X, x, tick);
            if (y) enqueue(c, INPUT_REL, INPUT_Y, -y, tick);
            touchpad_alps_buttons(c, buttons, true);
            c->touchpad.tap = false;
            touchpad_buttons(c, tick);
            return;
        }
        unsigned back = 0, forward = 0;
        if (c->touchpad.flags & TP_ALPS_BACK1) { back = p[0] & 0x10; forward = p[2] & 4; }
        if (c->touchpad.flags & TP_ALPS_BACK2) {
            back = p[3] & 4; forward = p[2] & 4;
            buttons &= ~4u;
            if (back && forward) { buttons |= 4; back = forward = 0; }
        }
        if (back) buttons |= 8;
        if (forward) buttons |= 16;
        if (c->touchpad.flags & TP_ALPS_WHEEL) {
            int wheel = ((p[2] << 1) & 8) - ((p[0] >> 4) & 7);
            if (wheel) enqueue(c, INPUT_REL, INPUT_WHEEL, wheel, tick);
        }
    }
    touchpad_sample(c, x, y, z, w, buttons, tick);
}

static bool touchpad_prefix(struct controller *c, const uint8_t *p, unsigned length)
{
    unsigned protocol = c->touchpad.info.protocol;
    if (protocol == TOUCHPAD_SYNAPTICS_NEW) {
        return (p[0] & 0xC0) == 0x80 && (length < 4 || (p[3] & 0xC0) == 0xC0);
    }
    if (protocol == TOUCHPAD_SYNAPTICS_OLD) {
        return (p[0] & 0xC0) == 0xC0 && (length < 2 || !(p[1] & 0x60)) &&
            (length < 4 || (p[3] & 0xC0) == 0x80) && (length < 5 || !(p[4] & 0x60));
    }
    if ((p[0] & c->touchpad.mask0) != c->touchpad.byte0) return false;
    for (unsigned i = 1; i < length; i++) if (p[i] & 0x80) return false;
    return true;
}

static void touchpad_flush(struct controller *c, uint64_t tick)
{
    struct input_queue *q = &c->queue;
    if ((c->touchpad.flags & TP_ALPS_INTERLEAVED) && q->mouse_pos == 6 &&
        (q->packet[3] & 15) == 15 && tick - q->mouse_tick >= 20) {
        if (touchpad_prefix(c, q->packet, 6)) { touchpad_packet(c, q->mouse_tick); c->touchpad.synced = true; }
        else { q->stats.resync++; touchpad_loss(c); }
        q->mouse_pos = 0;
    }
}

static void touchpad_byte(struct controller *c, uint8_t b, uint64_t tick)
{
    struct input_queue *q = &c->queue;
    if (q->mouse_pos && tick - q->mouse_tick > MOUSE_PACKET_GAP_MS) {
        q->mouse_pos = 0; q->stats.resync++; touchpad_loss(c);
    }
    touchpad_flush(c, tick);
    q->mouse_tick = tick;
    q->packet[q->mouse_pos++] = b;
    bool alps = c->touchpad.info.protocol >= TOUCHPAD_ALPS_V1;
    if (alps && c->touchpad.synced && (q->packet[0] & 0xC8) == 8) {
        if (q->mouse_pos == 3) { touchpad_stick(c, q->packet, true, tick); q->mouse_pos = 0; }
        return;
    }
    if (alps && (c->touchpad.flags & TP_ALPS_INTERLEAVED) &&
        q->mouse_pos >= 4 && (q->packet[3] & 15) == 15) {
        if (q->mouse_pos < 7) return;
        if (b & 0x80) {
            if (touchpad_prefix(c, q->packet, 6)) { touchpad_packet(c, tick); c->touchpad.synced = true; }
            else { q->stats.resync++; touchpad_loss(c); }
            q->mouse_pos = 0;
            touchpad_byte(c, b, tick);
        } else {
            touchpad_stick(c, q->packet + 3, false, tick);
            q->packet[3] = b & 0xF7;
            q->mouse_pos = 4;
        }
        return;
    }
    if (!touchpad_prefix(c, q->packet, q->mouse_pos)) {
        q->stats.resync++; touchpad_loss(c);
        unsigned skip = 1;
        while (skip < q->mouse_pos && !touchpad_prefix(c, q->packet + skip, q->mouse_pos - skip)) skip++;
        q->mouse_pos -= skip;
        memmove(q->packet, q->packet + skip, q->mouse_pos);
        return;
    }
    if (q->mouse_pos == 6) { touchpad_packet(c, tick); c->touchpad.synced = true; q->mouse_pos = 0; }
}
