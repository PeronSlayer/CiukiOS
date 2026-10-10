/* Native i8042, raw set-2 keyboard and standard three-byte PS/2 mouse.
 * Research/decision for f1-04 (2026-10-10), compared with registry.c,
 * sync.c and the F1 contracts before implementation:
 * IBM PS/2 Hardware Interface Technical Reference, controller pp. 4-12:
 * https://www.ardent-tool.com/docs/pdf/ibm_hitrc07.pdf
 * OBF/IBF, AUX bit 5, command-byte bits, D4 routing and the 7 us delay
 * after OBF. Preserve unrelated config bits; never write the output port
 * (A20/reset). Controller-generated replies require inhibited ports.
 * Linux v6.12 i8042.c, lock comments, controller_init/selftest:
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/input/serio/i8042.c
 * Linux v6.12 libps2.c, ps2_handle_ack/ps2_do_sendbyte:
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/input/serio/libps2.c
 * Serialize both ports; route unsolicited bytes while awaiting ACK/RESEND.
 * Linux v6.12 atkbd.c and psmouse-base.c (set-2 positions, 9-bit motion):
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/input/keyboard/atkbd.c
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/input/mouse/psmouse-base.c
 * No upstream code is copied. Limits are Ciuki policy: the directive's
 * 200 ms replies, 500 ms whole setup, two RESEND retries within
 * the original deadline. AA is one native activation self-test, followed
 * by reapplying config (some controllers reset it); no FF device reset,
 * output-port reset, retry-on-timeout or firmware/native fallback.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/arch.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/registry.h>
#include <ciuki/i8042.h>
#include <ciuki/fwinput.h>
#include <ciuki/biosvm.h>
#include <ciuki/init.h>

#define DATA_PORT 0x60u
#define STATUS_PORT 0x64u
#define OBF 0x01u
#define IBF 0x02u
#define AUX 0x20u
#define BAD_STATUS 0xC0u
#define DISABLED 0x30u
#define IRQ_BITS 0x03u
#define TRANSLATE 0x40u
#define POLL_LIMIT 65536u             /* stalled PIT escape, never a time estimate */

struct input_queue {
    struct input_event ring[INPUT_CAPACITY];
    struct input_stats stats;
    uint64_t sequence;
    unsigned head, tail;
    bool down[513], extended, breaking;
    uint8_t pause_pos, mouse_pos, packet[3];
};
struct controller {
    struct input_queue queue;
    struct kmutex mutex;
    const struct i8042_test_io *io;
    void *io_arg;
    struct i8042_stats stats;
    int handle[4];
    gen_t claim_gen[4];
    bool initialized, accepting, reply_any, reply_aux, reply_ready;
    uint8_t reply;
    int reply_error;
};
struct poll_deadline { uint64_t end; unsigned polls; };
static struct controller native;
static struct controller *fixture;

/* Set-2 wire positions -> the public set-1 positions. Checked against
 * Linux v6.12 atkbd_unxlate_table (source linked above), and SeaBIOS
 * rel-1.16.3 kbd.c's set-1 table. No backend-specific public key codes. */
static const uint8_t set1[0x84] = {
    [0x01]=0x43, [0x03]=0x3F, [0x04]=0x3D, [0x05]=0x3B, [0x06]=0x3C,
    [0x07]=0x58, [0x09]=0x44, [0x0A]=0x42, [0x0B]=0x40, [0x0C]=0x3E,
    [0x0D]=0x0F, [0x0E]=0x29, [0x11]=0x38, [0x12]=0x2A, [0x14]=0x1D,
    [0x15]=0x10, [0x16]=0x02, [0x1A]=0x2C, [0x1B]=0x1F, [0x1C]=0x1E,
    [0x1D]=0x11, [0x1E]=0x03, [0x21]=0x2E, [0x22]=0x2D, [0x23]=0x20,
    [0x24]=0x12, [0x25]=0x05, [0x26]=0x04, [0x29]=0x39, [0x2A]=0x2F,
    [0x2B]=0x21, [0x2C]=0x14, [0x2D]=0x13, [0x2E]=0x06, [0x31]=0x31,
    [0x32]=0x30, [0x33]=0x23, [0x34]=0x22, [0x35]=0x15, [0x36]=0x07,
    [0x3A]=0x32, [0x3B]=0x24, [0x3C]=0x16, [0x3D]=0x08, [0x3E]=0x09,
    [0x41]=0x33, [0x43]=0x17, [0x42]=0x25, [0x44]=0x18, [0x45]=0x0B,
    [0x46]=0x0A, [0x49]=0x34, [0x4A]=0x35, [0x4B]=0x26, [0x4C]=0x27,
    [0x4D]=0x19, [0x4E]=0x0C, [0x52]=0x28, [0x54]=0x1A, [0x55]=0x0D,
    [0x58]=0x3A, [0x59]=0x36, [0x5A]=0x1C, [0x5B]=0x1B, [0x5D]=0x2B,
    [0x61]=0x56, [0x66]=0x0E, [0x69]=0x4F, [0x6B]=0x4B, [0x6C]=0x47,
    [0x70]=0x52, [0x71]=0x53, [0x72]=0x50, [0x73]=0x4C, [0x74]=0x4D,
    [0x75]=0x48, [0x76]=0x01, [0x77]=0x45, [0x78]=0x57, [0x79]=0x4E,
    [0x7A]=0x51, [0x7B]=0x4A, [0x7C]=0x37, [0x7D]=0x49, [0x7E]=0x46,
    [0x83]=0x41,
};
static const uint8_t set1_e0[0x7E] = {
    [0x11]=0x38, [0x14]=0x1D, [0x1F]=0x5B, [0x27]=0x5C, [0x2F]=0x5D,
    [0x4A]=0x35, [0x5A]=0x1C, [0x69]=0x4F, [0x6B]=0x4B, [0x6C]=0x47,
    [0x70]=0x52, [0x71]=0x53, [0x72]=0x50, [0x74]=0x4D, [0x75]=0x48,
    [0x7A]=0x51, [0x7C]=0x37, [0x7D]=0x49,
};
/* Shared US unshifted text/digest helper, indexed only by public set 1. */
static const char us[0x59] = {
    [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5', [0x07]='6',
    [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0', [0x0C]='-', [0x0D]='=',
    [0x0E]='\b', [0x0F]='\t', [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r',
    [0x14]='t', [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1A]='[', [0x1B]=']', [0x1C]='\r', [0x1E]='a', [0x1F]='s', [0x20]='d',
    [0x21]='f', [0x22]='g', [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
    [0x27]=';', [0x28]=39, [0x29]='`', [0x2B]=92, [0x2C]='z', [0x2D]='x',
    [0x2E]='c', [0x2F]='v', [0x30]='b', [0x31]='n', [0x32]='m', [0x33]=',',
    [0x34]='.', [0x35]='/', [0x39]=' ',
};
char input_unshifted(uint16_t code) { return code < sizeof(us) ? us[code] : 0; }

/* All decoder/queue helpers below run under the caller's short irq_save.
 * One byte has constant work (at most five events), no allocation/logging,
 * port polling, sleeps or deferred callbacks requiring shutdown fences. */
static void enqueue(struct controller *c, uint16_t type, uint16_t code, int32_t value, uint64_t tick)
{
    struct input_queue *q = &c->queue;
    uint64_t seq = ++q->sequence;
    if (q->stats.pending == INPUT_CAPACITY) {
        q->stats.overflow++;
        q->stats.state_lost = true;
        return;
    }
    q->ring[q->tail] = (struct input_event){ type, code, value, tick, seq,
        c->stats.generation, c->stats.firmware ? INPUT_FIRMWARE : INPUT_NATIVE, q->stats.state_lost ? INPUT_F_RESYNC : 0 };
    q->tail = (q->tail + 1) % INPUT_CAPACITY;
    q->stats.pending++;
}

static void key_transition(struct controller *c, uint16_t code, bool down, uint64_t tick)
{
    struct input_queue *q = &c->queue;
    if (q->down[code] == down) {
        if (down) q->stats.repeats++;
        else q->stats.duplicates++;
        return;
    }
    q->down[code] = down;
    if (down) q->stats.keys_down++;
    else q->stats.keys_down--;
    enqueue(c, INPUT_KEY, code, down, tick);
    if (down && !c->stats.firmware) {
        char ch = input_unshifted(code);
        if (ch) enqueue(c, INPUT_TEXT, (uint8_t)ch, 0, tick);
    }
}

static void keyboard_byte(struct controller *c, uint8_t b, uint64_t tick)
{
    struct input_queue *q = &c->queue;
    static const uint8_t pause_tail[] = { 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77 };
    if (q->pause_pos) {
        if (b == pause_tail[q->pause_pos - 1]) {
            if (++q->pause_pos == sizeof(pause_tail) + 1) {
                q->pause_pos = 0;
                key_transition(c, INPUT_KEY_PAUSE, true, tick);
                key_transition(c, INPUT_KEY_PAUSE, false, tick);
            }
            return;
        }
        q->pause_pos = 0;
        q->stats.resync++;
    }
    if (b == 0xE1) {
        q->extended = q->breaking = false;
        q->pause_pos = 1;
        return;
    }
    if (b == 0xE0) { q->extended = true; return; }
    if (b == 0xF0) { q->breaking = true; return; }
    /* Print Screen's E0 12 is a fake shift, not a key transition. */
    uint16_t code = q->extended ? (b < sizeof(set1_e0) ? set1_e0[b] : 0) :
                                  (b < sizeof(set1) ? set1[b] : 0);
    if (code)
        key_transition(c, code | (q->extended ? 0x100u : 0), !q->breaking, tick);
    else if (!(q->extended && b == 0x12))
        q->stats.resync++;
    q->extended = q->breaking = false;
}

static void mouse_byte(struct controller *c, uint8_t b, uint64_t tick)
{
    struct input_queue *q = &c->queue;
    if (!q->mouse_pos && (!(b & 8) || (b & 0xC0))) {
        q->stats.resync++;              /* count every rejected candidate header */
        return;
    }
    q->packet[q->mouse_pos++] = b;
    if (q->mouse_pos != 3)
        return;
    q->mouse_pos = 0;
    uint8_t h = q->packet[0];
    /* PS/2 axes are signed NINE-bit values, not int8_t. Screen y grows
     * down, the device's y grows up. Zero payload with a sign bit means
     * zero (Linux standard-packet convention), not an artificial -256. */
    int x = q->packet[1] ? (int)q->packet[1] - ((h & 0x10) ? 256 : 0) : 0;
    int y = q->packet[2] ? (int)q->packet[2] - ((h & 0x20) ? 256 : 0) : 0;
    if (x) enqueue(c, INPUT_REL, INPUT_X, x, tick);
    if (y) enqueue(c, INPUT_REL, INPUT_Y, -y, tick);
    unsigned buttons = h & 7, changed = buttons ^ q->stats.buttons;
    for (unsigned i = 0; i < 3; i++)
        if (changed & (1u << i))
            enqueue(c, INPUT_BTN, (uint16_t)i, !!(buttons & (1u << i)), tick);
    q->stats.buttons = buttons;
}

static bool dequeue(struct controller *c, struct input_event *out)
{
    if (!out)
        return false;
    uint32_t f = irq_save();
    struct input_queue *q = &c->queue;
    bool have = q->stats.pending != 0;
    if (have) {
        *out = q->ring[q->head];
        q->head = (q->head + 1) % INPUT_CAPACITY;
        q->stats.pending--;
    }
    irq_restore(f);
    return have;
}

bool input_read(struct input_event *out) { return dequeue(&native, out); }
void input_snapshot(struct input_stats *out)
{
    if (!out) return;
    uint32_t f = irq_save();
    *out = native.queue.stats;
    irq_restore(f);
}

static uint32_t hash_byte(uint32_t h, uint8_t b) { return (h ^ b) * 16777619u; }
void input_digest_init(struct input_digest *d)
{
    memset(d, 0, sizeof(*d));
    d->hash = d->text_hash = 2166136261u;
}
void input_digest_add(struct input_digest *d, const struct input_event *e)
{
    d->events++;
    /* Explicit little-endian fields, excluding padding/timestamps/generation
     * so the replay digest is reproducible on the host and the kernel. */
    uint32_t fields[] = { e->type, e->code, (uint32_t)e->value };
    for (unsigned i = 0; i < ARRAY_SIZE(fields); i++)
        for (unsigned j = 0; j < 4; j++)
            d->hash = hash_byte(d->hash, (uint8_t)(fields[i] >> (8 * j)));
    if (e->type == INPUT_KEY) {
        d->key_transitions++;
    } else if (e->type == INPUT_TEXT) {
        d->characters++;
        d->text_hash = hash_byte(d->text_hash, (uint8_t)e->code);
    } else if (e->type == INPUT_BTN) {
        d->button_transitions++;
    } else if (e->type == INPUT_REL) {
        if (e->code == INPUT_X) d->x += e->value;
        if (e->code == INPUT_Y) d->y += e->value;
    }
}

static uint8_t physical_read(void *arg, uint16_t port)
{
    (void)arg;
    if (port == DATA_PORT)
        (void)udelay(7);                /* IBM Type 1 OBF-to-data delay */
    return inb(port);
}
static void physical_write(void *arg, uint16_t port, uint8_t b) { (void)arg; outb(port, b); }
static uint64_t physical_now(void *arg) { (void)arg; return deadline_after_ms(0); }
static void physical_pause(void *arg)
{
    (void)arg;
    (void)udelay(10);                    /* IF=1; engine yields at most every 32 polls */
}
static const struct i8042_test_io physical_io = {
    physical_read, physical_write, physical_now, physical_pause,
};

static uint8_t read_port(struct controller *c, uint16_t port)
{
    c->stats.reads++;
    return c->io->read(c->io_arg, port);
}
static void write_port(struct controller *c, uint16_t port, uint8_t b)
{
    c->stats.writes++;
    c->io->write(c->io_arg, port, b);
}
static struct poll_deadline limit(struct controller *c, uint64_t outer, uint32_t ms)
{
    uint64_t now = c->io->now(c->io_arg);
    uint64_t remaining = outer - now;
    if (remaining >= (1ull << 63)) remaining = 0;
    return (struct poll_deadline){ now + (remaining < ms ? remaining : ms), 0 };
}
static int poll_check(struct controller *c, struct poll_deadline *d)
{
    if (c->io->now(c->io_arg) - d->end < (1ull << 63)) {
        c->stats.timeouts++;
        return -I8042_ETIMEDOUT;
    }
    if (++d->polls > POLL_LIMIT) {
        c->stats.stalled++;
        return -EFAULT;
    }
    return 0;
}
static void poll_pause(struct controller *c, struct poll_deadline *d)
{
    c->io->pause(c->io_arg);
    if (!(d->polls % 32)) task_yield();
}

static void capture(struct controller *c, uint8_t status, uint8_t b)
{
    struct input_queue *q = &c->queue;
    q->stats.bytes++;
    bool aux = (status & AUX) != 0;
    if (status & BAD_STATUS) {
        q->stats.errors++;
        q->stats.state_lost = true;
        if (aux) q->mouse_pos = 0;
        else { q->extended = q->breaking = false; q->pause_pos = 0; }
        if (c->stats.pending_command && aux == c->reply_aux) {
            c->reply_error = -I8042_EIO;
            c->reply_ready = true;
        }
        return;
    }
    if (c->stats.pending_command && !c->reply_ready && aux == c->reply_aux &&
        (c->reply_any || b == 0xFA || b == 0xFE || b == 0xFC || b == 0xFD)) {
        c->reply = b;
        c->reply_ready = true;
        return;
    }
    uint64_t tick = c->io->now(c->io_arg);
    if (aux) mouse_byte(c, b, tick);
    else keyboard_byte(c, b, tick);
}

static bool capture_one(struct controller *c, bool discard)
{
    uint32_t f = irq_save();
    uint8_t status = read_port(c, STATUS_PORT);
    bool have = status & OBF;
    if (have) {
        uint8_t b = read_port(c, DATA_PORT);
        if (discard) c->stats.drained++;
        else capture(c, status, b);
    }
    irq_restore(f);
    return have;
}

static int drain(struct controller *c, struct poll_deadline *d, bool discard)
{
    for (;;) {
        int err = poll_check(c, d);
        if (err) return err;
        if (!capture_one(c, discard)) return 0;
        poll_pause(c, d);
    }
}

static int write_when_ready(struct controller *c, struct poll_deadline *d, uint16_t port, uint8_t b)
{
    for (;;) {
        int err = poll_check(c, d);
        if (err) return err;
        uint32_t f = irq_save();
        uint8_t status = read_port(c, STATUS_PORT);
        if (status & OBF) {
            capture(c, status, read_port(c, DATA_PORT));
        } else if (!(status & IBF)) {
            write_port(c, port, b);
            irq_restore(f);
            return 0;
        }
        irq_restore(f);
        poll_pause(c, d);
    }
}

static void expect_reply(struct controller *c, bool aux, bool any)
{
    uint32_t f = irq_save();
    c->reply_aux = aux;
    c->reply_any = any;
    c->reply_ready = false;
    c->reply_error = 0;
    c->stats.pending_command = true;
    irq_restore(f);
}
static int wait_reply(struct controller *c, struct poll_deadline *d, uint8_t *reply)
{
    int err;
    for (;;) {
        err = poll_check(c, d);
        if (err) break;
        uint32_t f = irq_save();
        bool ready = c->reply_ready;
        if (ready) {
            *reply = c->reply;
            err = c->reply_error;
        }
        irq_restore(f);
        if (ready) break;
        if (!capture_one(c, false)) poll_pause(c, d);
        else if (!(d->polls % 32)) task_yield();
    }
    uint32_t f = irq_save();
    c->stats.pending_command = false;
    irq_restore(f);
    return err;
}

static int device_exchange(struct controller *c, bool aux, uint8_t b, uint64_t outer, uint8_t *data)
{
    if (c->stats.quarantined) return -I8042_EIO;
    /* Reset is not qualified, even for a selected fault fixture. */
    if (b == 0xFF) return -EINVAL;
    uint64_t start = c->io->now(c->io_arg);
    struct poll_deadline d = limit(c, outer, I8042_REPLY_MS);
    c->stats.commands++;
    int err = drain(c, &d, false);
    for (unsigned attempt = 0; !err; attempt++) {
        if (aux) err = write_when_ready(c, &d, STATUS_PORT, 0xD4);
        if (err) break;
        expect_reply(c, aux, false);
        err = write_when_ready(c, &d, DATA_PORT, b);
        if (err) break;
        uint8_t reply = 0;
        err = wait_reply(c, &d, &reply);
        if (err) break;
        if (reply == 0xFA) {
            if (data) {
                expect_reply(c, aux, true);
                err = wait_reply(c, &d, data);
            }
            break;
        }
        if (reply != 0xFE || attempt == I8042_RESENDS) {
            err = -I8042_EPROTO;
            break;
        }
        c->stats.resends++;
    }
    uint32_t f = irq_save();
    c->stats.pending_command = false;
    c->stats.last_elapsed_ms = (uint32_t)(c->io->now(c->io_arg) - start);
    c->stats.last_error = err;
    irq_restore(f);
    return err;
}

static int device_command(struct controller *c, bool aux, uint8_t b, uint64_t outer)
{
    return device_exchange(c, aux, b, outer, 0);
}

/* Ports MUST be inhibited and drained before these untagged controller
 * replies. Native initialization/shutdown holds the command mutex. */
static int controller_reply(struct controller *c, uint8_t cmd, uint8_t *out, uint64_t outer)
{
    struct poll_deadline d = limit(c, outer, I8042_REPLY_MS);
    int err = drain(c, &d, true);
    if (!err) {
        expect_reply(c, false, true);
        err = write_when_ready(c, &d, STATUS_PORT, cmd);
        if (!err) err = wait_reply(c, &d, out);
    }
    c->stats.pending_command = false;
    return err;
}
static int controller_write(struct controller *c, uint8_t cmd, uint64_t outer)
{
    struct poll_deadline d = limit(c, outer, I8042_REPLY_MS);
    return write_when_ready(c, &d, STATUS_PORT, cmd);
}
static int config_write(struct controller *c, uint8_t cfg, uint64_t outer)
{
    struct poll_deadline d = limit(c, outer, I8042_REPLY_MS);
    int err = write_when_ready(c, &d, STATUS_PORT, 0x60);
    if (!err) err = write_when_ready(c, &d, DATA_PORT, cfg);
    if (!err) c->stats.config = cfg;
    return err;
}

static void quarantine(struct controller *c, int err)
{
    c->accepting = c->stats.active = false;
    c->stats.pending_command = false;
    c->stats.last_error = err;
    c->stats.quarantined = true;
    if (c != &native) return;           /* fixtures own no physical resources */
    pic_mask(1);
    pic_mask(12);
    irq_set_handler(1, 0);
    irq_set_handler(12, 0);
    /* Uncertain commands may still be in flight: no more controller
     * writes, no release or automatic restart. Masked claims stay pinned. */
    for (unsigned i = 0; i < ARRAY_SIZE(c->handle); i++)
        if (c->handle[i] >= 0)
            registry_quarantine(c->handle[i], c->claim_gen[i]);
}

static bool claims_active(struct controller *c)
{
    for (unsigned i = 0; i < ARRAY_SIZE(c->handle); i++) {
        const struct resource *r = c->handle[i] < 0 ? 0 : registry_get((unsigned)c->handle[i]);
        if (!r || !gen_matches(r->generation, c->claim_gen[i]) || r->state != RS_ACTIVE)
            return false;
    }
    return true;
}

static void input_irq(struct trap_frame *tf)
{
    (void)tf;
    if (native.accepting && !native.stats.quarantined && claims_active(&native))
        capture_one(&native, false);    /* exactly one byte; dispatcher owns EOI */
}

static bool no_hardware(int handle, gen_t generation)
{
    (void)handle; (void)generation;
    return true;                       /* only pre-I/O rollback */
}
static void rollback_claims(struct controller *c)
{
    for (unsigned i = 0; i < ARRAY_SIZE(c->handle); i++) {
        if (c->handle[i] < 0) continue;
        if (registry_activate(c->handle[i], c->claim_gen[i]) == 0 &&
            registry_quiesce(c->handle[i], c->claim_gen[i], no_hardware) == 0)
            registry_release(c->handle[i], c->claim_gen[i]);
        c->handle[i] = -1;
    }
}
static int acquire(struct controller *c)
{
    static const enum res_type types[] = { RES_PORT, RES_PORT, RES_IRQ, RES_IRQ };
    static const uint32_t starts[] = { DATA_PORT, STATUS_PORT, 1, 12 };
    int reserved[4] = { -1, -1, -1, -1 };
    /* Preflight the complete transaction before any transfer or device I/O.
     * The UP kernel cannot interleave a thread here: these calls never yield. */
    for (unsigned j = 0; j < 4; j++) {
        for (unsigned i = 0; i < registry_count(); i++) {
            const struct resource *r = registry_get(i);
            if (r->type != types[j] || r->state == RS_RELEASED ||
                r->start >= starts[j] + 1 || r->end <= starts[j]) continue;
            if (r->start != starts[j] || r->end != starts[j] + 1 ||
                r->state != RS_FIRMWARE || strncmp(r->owner, "input", 6)) {
                klog("[i8042] conflict: i8042 wants %u:%x-%x held by %s reason=lease",
                     types[j], starts[j], starts[j] + 1, r->owner);
                return -EINVAL;
            }
            reserved[j] = (int)i;
        }
    }
    /* Allocate unreserved claims first. Failure rolls back without having
     * transferred a firmware lease. Gen exhaustion during transfer is
     * fail-closed: retain/quarantine, never pretend the lease was restored. */
    for (unsigned j = 0; j < 4; j++) {
        if (reserved[j] >= 0) continue;
        int h = registry_claim(types[j], starts[j], starts[j] + 1, "i8042", false);
        if (h < 0) { rollback_claims(c); return h; }
        c->handle[j] = h;
        c->claim_gen[j] = registry_get((unsigned)h)->generation;
    }
    for (unsigned j = 0; j < 4; j++) {
        if (reserved[j] < 0) continue;
        int h = reserved[j];
        int err = registry_claim_reserved(h, registry_get((unsigned)h)->generation, "i8042");
        if (err) {
            for (unsigned i = 0; i < 4; i++)
                if (c->handle[i] >= 0) registry_quarantine(c->handle[i], c->claim_gen[i]);
            c->stats.quarantined = true;
            return err;
        }
        c->handle[j] = h;
        c->claim_gen[j] = registry_get((unsigned)h)->generation;
    }
    return 0;
}

static void controller_init(struct controller *c, const struct i8042_test_io *io, void *arg)
{
    memset(c, 0, sizeof(*c));
    for (unsigned i = 0; i < ARRAY_SIZE(c->handle); i++) c->handle[i] = -1;
    c->io = io;
    c->io_arg = arg;
    kmutex_init(&c->mutex);
    c->initialized = true;
}

int i8042_init(void)
{
    if (!(read_eflags() & 0x200) || !g_current) return -EINVAL;
    /* Real E500 policy is input_policy; INPUT_FORCED is the validated QEMU
     * override. Check both BEFORE even changing PIC masks or claiming. */
    if (g_boot.input_policy == CBI_INPUT_FIRMWARE || (g_boot.flags & CBI_F_INPUT_FORCED)) {
        native.stats.firmware = true;
        if (native.stats.quarantined || biosvm_backend_state() == BIOSVM_DISABLED_BACKEND)
            return -I8042_EIO;
        return native.stats.active ? 0 : -ENOSYS;
    }
    if (native.stats.quarantined) return -I8042_EIO;
    if (native.stats.active) return claims_active(&native) ? 0 : -EINVAL;
    if (!native.initialized) controller_init(&native, &physical_io, 0);
    kmutex_lock(&native.mutex);
    /* Recheck after a contended mutex: another activation may have finished
     * or failed while this caller was asleep. */
    if (native.stats.active || native.stats.quarantined) {
        int result = native.stats.quarantined ? -I8042_EIO : (claims_active(&native) ? 0 : -EINVAL);
        kmutex_unlock(&native.mutex);
        return result;
    }
    int err = acquire(&native);
    if (err) { kmutex_unlock(&native.mutex); return err; }
    native.stats.generation = gen_alloc();
    if (!native.stats.generation) {
        quarantine(&native, -ENOSPC);
        kmutex_unlock(&native.mutex);
        return -ENOSPC;
    }
    uint64_t start = physical_now(0), end = start + I8042_SETUP_MS;
    pic_mask(1);
    pic_mask(12);
    uint8_t cfg = 0, reply = 0;
    err = controller_write(&native, 0xAD, end);
    if (!err) err = controller_write(&native, 0xA7, end);
    struct poll_deadline d = limit(&native, end, I8042_REPLY_MS);
    if (!err) err = drain(&native, &d, true);
    if (!err) err = controller_reply(&native, 0x20, &cfg, end);
    native.stats.initial_config = cfg;
    cfg = (cfg | DISABLED) & ~(IRQ_BITS | TRANSLATE);
    if (!err) err = config_write(&native, cfg, end);
    if (!err) err = controller_reply(&native, 0xAA, &reply, end);
    if (!err && reply != 0x55) err = -I8042_EIO;
    /* AA may reset the command byte; inhibit again before any config reply. */
    if (!err) err = controller_write(&native, 0xAD, end);
    if (!err) err = controller_write(&native, 0xA7, end);
    if (!err) err = config_write(&native, cfg, end);
    if (!err) err = controller_reply(&native, 0xAB, &reply, end);
    if (!err && reply) err = -I8042_EIO;
    if (!err) err = controller_reply(&native, 0xA9, &reply, end);
    if (!err && reply) err = -I8042_EIO;
    if (!err) err = controller_write(&native, 0xAE, end);
    if (!err) err = controller_write(&native, 0xA8, end);
    cfg &= ~DISABLED;
    if (!err) err = config_write(&native, cfg, end);
    if (!err) err = device_command(&native, false, 0xF5, end); /* disable scanning */
    if (!err) err = device_command(&native, false, 0xF0, end);
    if (!err) err = device_command(&native, false, 0x02, end); /* raw scan set 2 */
    /* F6 restores defaults without a device reset/BAT. It DOES NOT force
     * an already negotiated wheel protocol back to ID 0 (QEMU v9.2.0:
     * https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/input/ps2.c
     * AUX_SET_DEFAULT vs AUX_RESET). Query ID and refuse extended packets;
     * no IntelliMouse sample-rate detection or blind reset is performed. */
    if (!err) err = device_command(&native, true, 0xF5, end);
    if (!err) err = device_command(&native, true, 0xF6, end);
    if (!err) err = device_exchange(&native, true, 0xF2, end, &reply);
    if (!err && reply) err = -I8042_EPROTO;
    if (!err) err = device_command(&native, true, 0xEA, end); /* stream mode */
    if (!err) err = device_command(&native, true, 0xE6, end); /* 1:1 scaling */
    if (!err) err = device_command(&native, false, 0xF4, end);
    if (!err) err = device_command(&native, true, 0xF4, end);
    /* Verify nontranslation before activating registry claims/IRQs. */
    if (!err) err = controller_write(&native, 0xAD, end);
    if (!err) err = controller_write(&native, 0xA7, end);
    if (!err) err = config_write(&native, cfg | DISABLED, end);
    if (!err) err = controller_reply(&native, 0x20, &reply, end);
    if (!err && (reply & (TRANSLATE | DISABLED | IRQ_BITS)) != DISABLED) err = -I8042_EIO;
    for (unsigned i = 0; !err && i < ARRAY_SIZE(native.handle); i++)
        err = registry_activate(native.handle[i], native.claim_gen[i]);
    if (!err) {
        uint32_t f = irq_save();
        native.accepting = true;
        irq_set_handler(1, input_irq);
        irq_set_handler(12, input_irq);
        irq_restore(f);
        err = config_write(&native, cfg | IRQ_BITS, end);
    }
    if (err) quarantine(&native, err);
    else {
        native.stats.active = true;
        pic_unmask(1);
        pic_unmask(12);                 /* PIC service also enables cascade */
    }
    native.stats.last_elapsed_ms = (uint32_t)(physical_now(0) - start);
    native.stats.last_error = err;
    kmutex_unlock(&native.mutex);
    return err;
}

static bool idle_proof(int handle, gen_t generation)
{
    (void)handle; (void)generation;
    return !native.accepting && !native.stats.pending_command;
}
int i8042_stop(gen_t generation)
{
    if (native.stats.firmware) return -ENOSYS; /* persistent BIOS lease */
    if (!native.stats.active || !gen_matches(native.stats.generation, generation)) return -EINVAL;
    if (!(read_eflags() & 0x200) || !g_current) return -EINVAL;
    kmutex_lock(&native.mutex);
    if (!native.stats.active || !gen_matches(native.stats.generation, generation) || !claims_active(&native)) {
        kmutex_unlock(&native.mutex);
        return -EINVAL;
    }
    native.accepting = false;          /* reject submissions before stopping sources */
    pic_mask(1);
    pic_mask(12);
    irq_set_handler(1, 0);
    irq_set_handler(12, 0);
    uint64_t end = physical_now(0) + I8042_REPLY_MS;
    int err = controller_write(&native, 0xAD, end);
    if (!err) err = controller_write(&native, 0xA7, end);
    if (!err) err = config_write(&native, (native.stats.config | DISABLED) & ~IRQ_BITS, end);
    struct poll_deadline d = limit(&native, end, I8042_REPLY_MS);
    if (!err) err = drain(&native, &d, true);
    uint8_t cfg = 0;
    if (!err) err = controller_reply(&native, 0x20, &cfg, end);
    if (!err && (cfg & (DISABLED | IRQ_BITS)) != DISABLED) err = -I8042_EIO;
    d = limit(&native, end, I8042_REPLY_MS);
    if (!err) err = drain(&native, &d, true);
    if (!err) {
        uint32_t f = irq_save();
        if (read_port(&native, STATUS_PORT) & (IBF | OBF)) err = -I8042_EIO;
        irq_restore(f);
    }
    for (unsigned i = 0; !err && i < ARRAY_SIZE(native.handle); i++)
        err = registry_quiesce(native.handle[i], native.claim_gen[i], idle_proof);
    if (err) quarantine(&native, err);
    else {
        for (unsigned i = 0; i < ARRAY_SIZE(native.handle); i++)
            registry_release(native.handle[i], native.claim_gen[i]);
        native.stats.active = false;
        native.initialized = false;    /* fresh generation/state on next activation */
    }
    kmutex_unlock(&native.mutex);
    return err;
}

const char *i8042_backend(void) { return native.stats.firmware ? "firmware" : "native"; }
void i8042_snapshot(struct i8042_stats *out)
{
    if (!out) return;
    uint32_t f = irq_save();
    *out = native.stats;
    irq_restore(f);
}

/* Firmware bridge: one producer thread, same bounded queue, no controller
 * access. Generation/source are immutable for the persistent BIOS lease. */
bool input_firmware_begin(gen_t generation)
{
    if (!generation || (g_boot.input_policy != CBI_INPUT_FIRMWARE &&
                        !(g_boot.flags & CBI_F_INPUT_FORCED))) return false;
    uint32_t f = irq_save();
    if (native.stats.quarantined || (native.stats.active && !native.stats.firmware)) {
        irq_restore(f);
        return false;
    }
    memset(&native.queue, 0, sizeof(native.queue));
    native.stats.firmware = native.stats.active = true;
    native.stats.generation = generation;
    irq_restore(f);
    return true;
}

void input_firmware_loss(uint64_t lost)
{
    uint32_t f = irq_save();
    native.queue.stats.overflow += lost;
    irq_restore(f);
}

void input_firmware_disable(gen_t generation)
{
    uint32_t f = irq_save();
    if (native.stats.firmware && gen_matches(native.stats.generation, generation)) {
        native.stats.quarantined = true;
        native.stats.active = false;
        native.stats.last_error = -I8042_EIO;
    }
    irq_restore(f);
}

void input_firmware_event(const struct fwinput_event *e, gen_t generation)
{
    if (!e) return;
    uint32_t f = irq_save();
    struct input_queue *q = &native.queue;
    if (!native.stats.firmware || !native.stats.active ||
        !gen_matches(native.stats.generation, generation)) {
        irq_restore(f);
        return;
    }
    switch (e->type) {
    case FWINPUT_KEY:
        if (e->code < ARRAY_SIZE(q->down) && (e->value == 0 || e->value == 1))
            key_transition(&native, e->code, e->value != 0, e->tick);
        else q->stats.errors++;
        break;
    case FWINPUT_TEXT:
        if (e->value > 0 && e->value <= UINT16_MAX)
            enqueue(&native, INPUT_TEXT, (uint16_t)e->value, 0, e->tick);
        else q->stats.errors++;
        break;
    case FWINPUT_REL:
        if (e->code <= FWINPUT_Y)
            enqueue(&native, INPUT_REL, e->code == FWINPUT_X ? INPUT_X : INPUT_Y, e->value, e->tick);
        else q->stats.errors++;
        break;
    case FWINPUT_BUTTON:
        if (e->code < 3 && (e->value == 0 || e->value == 1)) {
            unsigned mask = 1u << e->code;
            if (!!(q->stats.buttons & mask) == !!e->value) q->stats.duplicates++;
            else {
                q->stats.buttons ^= mask;
                enqueue(&native, INPUT_BTN, e->code, e->value, e->tick);
            }
        } else q->stats.errors++;
        break;
    case FWINPUT_RESYNC:
        q->stats.resync++;
        q->stats.state_lost = true;
        q->stats.keys_down = q->stats.buttons = 0;
        memset(q->down, 0, sizeof(q->down));
        enqueue(&native, INPUT_RESYNC, 0, 0, e->tick);
        break;
    default: q->stats.errors++; break;
    }
    irq_restore(f);
}

static bool fault_selected(void)
{
    if (!(g_boot.flags & CBI_F_TEST_REQUEST) || g_boot.test_request_len > CIUKI_TEST_REQ_MAX)
        return false;
    const char *s = g_boot.test_request;
    static const char *prefixes[] = { "f1:input-fault run=", "f1:core run=", "f1:all run=" };
    unsigned n = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(prefixes); i++) {
        unsigned length = (unsigned)strlen(prefixes[i]);
        if (g_boot.test_request_len >= length + 8 && !strncmp(s, prefixes[i], length)) {
            n = length;
            break;
        }
    }
    if (!n) return false;
    for (unsigned i = n; i < n + 8; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
              (s[i] >= 'A' && s[i] <= 'F'))) return false;
    n += 8;
    if (g_boot.test_request_len >= n + 14 && !strncmp(s + n, " platform=e500", 14)) {
        if (!(g_boot.flags & CBI_F_INPUT_FORCED)) return false;
        n += 14;
    }
    if (g_boot.test_request_len >= n + 7 && !strncmp(s + n, " safe=1", 7)) n += 7;
    return n == g_boot.test_request_len;
}

int i8042_fault_begin(const struct i8042_test_io *io, void *arg)
{
    if (!fault_selected()) return -ENOSYS;
    if (!(read_eflags() & 0x200) || !g_current || fixture || !io || !io->read ||
        !io->write || !io->now || !io->pause) return -EINVAL;
    fixture = kzalloc(sizeof(*fixture));
    if (!fixture) return -ENOMEM;
    controller_init(fixture, io, arg);
    fixture->stats.generation = gen_alloc();
    if (!fixture->stats.generation) { i8042_fault_end(); return -ENOSPC; }
    fixture->accepting = fixture->stats.active = true;
    return 0;
}
int i8042_fault_command(bool aux, uint8_t b)
{
    if (!fixture) return -ENOSYS;
    kmutex_lock(&fixture->mutex);
    int err = device_command(fixture, aux, b, fixture->io->now(fixture->io_arg) + I8042_REPLY_MS);
    if (err && err != -EINVAL) quarantine(fixture, err);
    kmutex_unlock(&fixture->mutex);
    return err;
}
int i8042_fault_capture(uint8_t status, uint8_t b)
{
    if (!fixture) return -ENOSYS;
    if (!fixture->accepting || !(status & OBF)) return -EINVAL;
    uint32_t f = irq_save();
    capture(fixture, status, b);
    irq_restore(f);
    return 0;
}
bool i8042_fault_read(struct input_event *out) { return fixture && dequeue(fixture, out); }
void i8042_fault_snapshot(struct i8042_stats *driver, struct input_stats *queue)
{
    if (!fixture) return;
    uint32_t f = irq_save();
    if (driver) *driver = fixture->stats;
    if (queue) *queue = fixture->queue.stats;
    irq_restore(f);
}
void i8042_fault_end(void)
{
    if (!fixture) return;
    /* No callback survives the synchronous command. Fixtures never publish
     * IRQ/work producers and cannot alter/release a physical claim. */
    kfree(fixture);
    fixture = 0;
}
