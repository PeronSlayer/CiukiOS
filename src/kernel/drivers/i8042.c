/* Native i8042, raw set-2 keyboard and PS/2 ID 0/3/4 mice.
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
 * f1-27 research/decision (T23 capture run=22222222 only contains EIO,
 * not the failing byte): SeaBIOS ps2port.c, __ps2_command/init_keyboard:
 * https://github.com/coreboot/seabios/blob/master/src/hw/ps2port.c
 * Inhibit/drain before controller replies, explicitly enable device ports.
 * Linux v6.12 i8042.c, controller_init/check_aux/toggle_aux:
 * stable CTR reads, internal AUX loopback can qualify a failing external
 * interface test; verify port bits rather than trusting a write. Preserve
 * strict nontranslation, device ACKs and quarantine, unlike Linux's x86
 * self-test fail-open policy. i8042-acpipnpio.h has IBM 2656 NOMUX and newer
 * ThinkPad reset quirks, no T23 quirk: do not infer a T23-specific workaround.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/input/serio/i8042-acpipnpio.h
 * HP 361834-002, tables 5-17/18, pp. 5-21..23: AA inhibits both ports;
 * AD/AE and A7/A8 set/clear disable bits, command-byte bit 6 translates.
 * https://h10032.www1.hp.com/ctg/Manual/c00283274.pdf
 * IBM T20..T23 HMM 62P9631, p. 46: TrackPoint can be disabled in setup;
 * absence is not permission to claim complete keyboard/mouse activation.
 * https://download.lenovo.com/pccbbs/mobiles_pdf/62p9631.pdf
 * Keep 200 ms ordinary replies/500 ms setup: SeaBIOS permits 500 ms
 * controller waits, but this capture supplies no evidence of a late reply.
 * Retain bounded raw traces, log after the timed transaction so serial I/O
 * cannot spend the setup budget. No new reset or timeout retry is qualified.
 * f1-29 research/decision: T23 boot1 has AA=55, CTR=77->34, AB=FA.
 * https://raw.githubusercontent.com/torvalds/linux/master/drivers/input/serio/i8042.c
 * controller_check only flushes; check_aux documents FA on notebooks and
 * qualifies external-test errors through loopback. AB/A9 are advisory here;
 * device ACKs/ID remain mandatory, with f1-27's tagged AUX loopback retained.
 * https://raw.githubusercontent.com/coreboot/seabios/master/src/hw/ps2port.c
 * Contrary to the directive's source summary, current SeaBIOS gates on AB
 * and Linux uses A9 as a loopback fallback. This change follows the directive,
 * not an assertion that both upstreams omit all interface tests.
 * A no-reply controller write gets a bounded 1 ms quiet interval after IBF
 * clears, within the existing deadlines. Drain one EC FA there, record it,
 * and complete that controller phase before arming a device ACK receiver.
 * f1-103 protocol research and recovery decisions:
 * docs/design/peripheral-driver-coverage.md, Input implementation contract.
 * Microsoft PS/2 tables specify signed byte/nibble wheel and Explorer buttons:
 * https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers#activating-buttons-4-5-and-wheel-on-ps2-mice
 * Linux v6.12 psmouse-base.c confirms the rate sequences and wheel sign.
 * No upstream code copied; keep command deadlines and retained quarantine.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/cpu.h>
#include <ciuki/arch.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/registry.h>
#include <ciuki/i8042.h>
#include <ciuki/fwinput.h>
#include <ciuki/biosvm.h>
#include <ciuki/init.h>
#include <ciuki/timing.h>
#include <ciuki/work.h>
#include <ciuki/touchpad.h>

extern void net_input_changed(void) __attribute__((weak));

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
#define MOUSE_PACKET_GAP_MS 500u      /* incomplete packet only, not device timeout */

struct input_queue {
    struct input_event ring[INPUT_CAPACITY];
    struct input_stats stats;
    uint64_t sequence;
    unsigned head, tail;
    bool down[513], native_down[513], extended, breaking;
    uint8_t usb_keys[513], usb_buttons[3], native_buttons;
    uint8_t pause_pos, mouse_pos, packet[9];
    uint64_t mouse_tick;
};
struct controller {
    struct input_queue queue;
    struct {
        struct touchpad_info info;
        uint64_t started;
        int x, y, start_x, start_y, scroll, rem_x, rem_y;
        uint8_t pad_buttons, stick_buttons, byte0, mask0;
        uint16_t flags;
        bool contact, tap, scrolling, synced, tap_blocked;
    } touchpad;
    struct kmutex mutex;
    const struct i8042_test_io *io;
    void *io_arg;
    struct i8042_stats stats;
    int handle[4];
    gen_t claim_gen[4];
    bool initialized, accepting, reply_any, reply_aux, reply_ready;
    uint16_t event_source;
    gen_t event_generation;
    uint8_t reply;
    uint8_t reply_data[3], reply_count, reply_wanted;
    bool reply_data_phase;
    int reply_error;
    struct i8042_init_record init_records[I8042_INIT_STEPS];
    uint64_t init_started;
    unsigned init_count;
    bool init_recording;
    uint8_t init_status;
};
struct poll_deadline { uint64_t end; unsigned polls; };
static struct controller native;
extern bool dosvm_fullscreen_input(uint16_t,uint16_t,int32_t) __attribute__((weak));
static unsigned usb_devices;
static unsigned repeat_delay = 1, repeat_speed = 31, lock_leds;
static unsigned programmed_repeat = ~0u, programmed_leds = ~0u;
static bool keyboard_policy_requested;
unsigned input_repeat_delay(void) { return (repeat_delay + 1) * 250; }
static unsigned typematic_interval(unsigned code)
{ return ((8+(code&7))<<((code>>3)&3))*1000/240; }
static unsigned typematic_rate(unsigned speed)
{
    /* SPI speed is linear in frequency (2.5..30 Hz); PS/2 F3 is not.
     * Choose the nearest hardware period, and give USB that same period. */
    unsigned wanted=12400/(31+11*speed),best=0,error=~0u;
    for(unsigned code=0;code<32;code++) {
        unsigned period=typematic_interval(code);
        unsigned difference=period>wanted ? period-wanted : wanted-period;
        if(difference<error){best=code;error=difference;}
    }
    return best;
}
unsigned input_repeat_interval(void) {return typematic_interval(typematic_rate(repeat_speed));}
unsigned input_lock_leds(void) { return lock_leds; }
static void keyboard_preferences_service(void);
void input_devices_snapshot(struct input_devices *out)
{
    uint32_t f = irq_save();
    bool active = native.stats.active && !native.stats.quarantined;
    *out = (struct input_devices){ .ps2 = active && !native.stats.firmware ? 2 : 0,
        .firmware = active && native.stats.firmware ? 2 : 0, .usb = usb_devices };
    irq_restore(f);
}
void input_devices_changed(enum input_source source, bool attached)
{
    uint32_t f = irq_save();
    if (source == INPUT_USB) {
        if (attached) usb_devices++;
        else if (usb_devices) usb_devices--;
    }
    struct input_devices state;
    input_devices_snapshot(&state);
    irq_restore(f);
    klog("[input] devices=%u source=%s event=%s ps2=%u usb=%u firmware=%u",
         state.ps2 + state.usb + state.firmware,
         source == INPUT_NATIVE ? "ps2" : source == INPUT_USB ? "usb" : "firmware",
         attached ? "attach" : "detach", state.ps2, state.usb, state.firmware);
}
/* One runtime-selected fixture, separate from the native controller and
 * its physical leases. Its queue exceeds kheap's 2040-byte payload limit;
 * reserve private storage instead of making an impossible heap request. */
static struct controller fixture_storage;
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

/* Decoder producers never yield. Native IRQs only append raw bytes, so the
 * thread consumer may decode with IF enabled; USB/firmware producers retain
 * their short queue exclusion. One byte produces at most twelve events plus
 * RESYNC, without allocation, logging, polling or sleeping. */
static void queue_event(struct controller *c, uint16_t type, uint16_t code, int32_t value,
                        uint64_t tick, uint64_t lost)
{
    struct input_queue *q = &c->queue;
    q->ring[q->tail] = (struct input_event){ .type = type, .code = code, .value = value,
        .tick = tick, .sequence = ++q->sequence, .generation = c->event_generation ? c->event_generation : c->stats.generation,
        .source = c->event_source ? c->event_source : c->stats.firmware ? INPUT_FIRMWARE : INPUT_NATIVE,
        .flags = type == INPUT_RESYNC ? INPUT_F_RESYNC : 0, .lost_count = lost };
    q->tail = (q->tail + 1) % INPUT_CAPACITY;
    q->stats.pending++;
    if (c == &native && net_input_changed) net_input_changed();
}
static void queue_resync(struct controller *c, uint64_t tick)
{
    struct input_queue *q = &c->queue;
    q->stats.overflow += q->stats.pending;
    q->stats.resync++;
    q->stats.state_lost = true;
    q->head = q->tail = q->stats.pending = 0;
    c->touchpad.tap = false;
    if (c==&native && dosvm_fullscreen_input) dosvm_fullscreen_input(INPUT_RESYNC,0,0);
    queue_event(c, INPUT_RESYNC, 0, (int32_t)q->stats.buttons, tick, q->stats.overflow);
}
/* --- sec-k07 (begin): trusted secure attention and input domains ---
 * Microsoft Winlogon registers Ctrl+Alt+Delete as the secure attention
 * sequence and switches the physical keyboard/screen to its own desktop
 * (learn.microsoft.com/windows/win32/secauthn/initializing-winlogon). Here
 * the merged native queue is the only producer of physical keys: PS/2 decode,
 * USB HID and the firmware adapter all reach key_emit; no syscall injects
 * into it. Once the kernel LOGON role exists (security_attention_armed) the
 * Delete press with a physically held Ctrl and Alt is consumed here, before
 * the DOS full-screen hook and the desktop queue, and its repeats/release are
 * swallowed too. The queue then feeds only the trusted domain: pending user
 * events are discarded, consumers get RESYNC, LOGON_UI an ATTENTION event.
 * Without LOGON (the interim desktop) nothing changes: Ctrl+Alt+Del stays an
 * ordinary key sequence and opens Task Manager as before. */
extern bool security_attention_armed(void) __attribute__((weak));
extern void security_attention_physical(void) __attribute__((weak));
static bool secure_trusted;
static uint16_t secure_swallow;
static uint64_t secure_discarded, secure_attentions;
bool input_secure_trusted(void) { return secure_trusted; }
void input_secure_stats(uint64_t *discarded, uint64_t *attentions)
{
    uint32_t f = irq_save();
    if (discarded) *discarded = secure_discarded;
    if (attentions) *attentions = secure_attentions;
    irq_restore(f);
}
/* Domain switch: nonblocking, IRQ-exclusive. Events queued for the previous
 * owner never reach the next one (typed passwords stay trusted, user keys
 * never reach the logon UI); both sides restart from one RESYNC. */
void input_secure_domain(bool trusted, uint32_t attention)
{
    uint32_t f = irq_save();
    struct input_queue *q = &native.queue;
    if (secure_trusted != trusted) {
        secure_trusted = trusted;
        if (dosvm_fullscreen_input) dosvm_fullscreen_input(INPUT_RESYNC,0,0);
        secure_discarded += q->stats.pending;
        q->head = q->tail = q->stats.pending = 0;
        q->stats.resync++;
        queue_event(&native, INPUT_RESYNC, 0, (int32_t)q->stats.buttons, g_ticks, q->stats.overflow);
    }
    if (attention && trusted) {
        if (q->stats.pending == INPUT_CAPACITY) queue_resync(&native, g_ticks);
        queue_event(&native, INPUT_ATTENTION, (uint16_t)attention, 0, g_ticks, 0);
        secure_attentions++;
    }
    irq_restore(f);
}
static bool secure_attention(struct controller *c, uint16_t code, int32_t value)
{
    if (secure_swallow && code == secure_swallow) {
        if (!value) secure_swallow = 0;
        return true;
    }
    if (value != 1 || (code != 0x153 && code != 0x53)) return false;
    const struct input_queue *q = &c->queue;
    if (!(q->down[0x1D] || q->down[0x11D]) || !(q->down[0x38] || q->down[0x138])) return false;
    if (!security_attention_armed || !security_attention_armed()) return false;
    secure_swallow = code;
    input_secure_domain(true, CIUKI_SECURITY_SECURE_ATTENTION);
    if (security_attention_physical) security_attention_physical();
    return true;
}
/* --- sec-k07 (end) --- */
/* --- sec-user-06 (begin): the self-test's secure attention ---
 * security.lock_latency (core/selftest_checks.c) measures the trusted gate
 * on an unattended T boot. It enters the same transition as the physical
 * filter above, from here, so that this module stays the only raiser of
 * ATTENTION (census: tests/host/security/test_secure_gate.py). Refused
 * outside a self-test boot and before the LOGON role exists; it touches no
 * key state, and no syscall or SDK wrapper reaches it. */
extern bool selftest_active(void) __attribute__((weak));
int input_selftest_attention(void)
{
    if (!selftest_active || !selftest_active()) return -EPERM;
    if (!security_attention_armed || !security_attention_armed() || !security_attention_physical) return -ENOENT;
    klog("[input] self-test secure attention");
    input_secure_domain(true, CIUKI_SECURITY_SECURE_ATTENTION);
    security_attention_physical();
    return 0;
}
/* --- sec-user-06 (end) --- */
static void enqueue(struct controller *c, uint16_t type, uint16_t code, int32_t value, uint64_t tick)
{
    /* sec-k07: a trusted console never feeds a DOS full-screen guest. */
    if (c==&native && dosvm_fullscreen_input && !secure_trusted && dosvm_fullscreen_input(type,code,value)) return;
    if (c->queue.stats.pending == INPUT_CAPACITY) queue_resync(c, tick);
    queue_event(c, type, code, value, tick, 0);
}

static void key_emit(struct controller *c, uint16_t code, bool down, uint64_t tick)
{
    struct input_queue *q = &c->queue;
    int32_t value = down;
    if (q->down[code] == down) {
        if (!down) { q->stats.duplicates++; return; }
        q->stats.repeats++;
        value = 2;
    } else {
        q->down[code] = down;
        if (c == &native && down) {
            if (code == 0x3a) lock_leds ^= 4;
            else if (code == 0x45) lock_leds ^= 2;
            else if (code == 0x46) lock_leds ^= 1;
        }
        if (down) q->stats.keys_down++;
        else q->stats.keys_down--;
    }
    if (c == &native && secure_attention(c, code, value)) return; /* sec-k07 */
    enqueue(c, INPUT_KEY, code, value, tick);
    if (down && !c->stats.firmware) {
        char ch = input_unshifted(code);
        if (ch) enqueue(c, INPUT_TEXT, (uint8_t)ch, 0, tick);
    }
}

static void key_transition(struct controller *c, uint16_t code, bool down, uint64_t tick)
{
    bool before = c->queue.native_down[code];
    c->queue.native_down[code] = down;
    if (!down && c->queue.usb_keys[code]) return;
    if (down && !before && c->queue.usb_keys[code]) return;
    key_emit(c, code, down, tick);
}

static void usb_event_drop(const char *reason, uint16_t type, uint16_t code,
                           int32_t value, gen_t generation)
{
    if (g_boot.flags & (CBI_F_VERBOSE | CBI_F_TEST_REQUEST))
        klog("[input] usb drop reason=%s type=%u set1=%03x value=%d generation=%u",
             reason, type, code, value, generation);
}

void input_hid_event(uint16_t type, uint16_t code, int32_t value, uint32_t device,
                     gen_t generation, uint64_t tick)
{
    if (!generation || !device || device>CIUKI_JOYSTICK_LAST ||
        type<INPUT_USAGE || type>INPUT_DEVICE || drivers_input_firmware()) return;
    uint32_t f=irq_save();
    native.event_source=device>=CIUKI_GAMEPORT_FIRST ? INPUT_NATIVE : INPUT_USB; native.event_generation=generation;
    /* HID extension has its own logical identity; DOS projection is separate. */
    if (native.queue.stats.pending==INPUT_CAPACITY) queue_resync(&native,tick);
    queue_event(&native,type,code,value,tick,0);
    struct input_event *e=&native.queue.ring[(native.queue.tail+INPUT_CAPACITY-1)%INPUT_CAPACITY];
    e->device=device;
    native.event_source=0; native.event_generation=0;
    irq_restore(f);
}
static void input_external_event(uint16_t type, uint16_t code, int32_t value, gen_t generation, uint64_t tick, bool serial)
{
    const char *drop = !generation ? "generation" :
        !serial && drivers_input_firmware() ? "firmware_policy" : 0;
    if (drop) { usb_event_drop(drop,type,code,value,generation); return; }
    uint32_t f = irq_save();
    struct input_queue *q = &native.queue;
    native.event_source = serial ? INPUT_NATIVE : INPUT_USB; native.event_generation = generation;
    if (type == INPUT_KEY && code < ARRAY_SIZE(q->down) && code) {
        uint8_t *refs = &q->usb_keys[code];
        if (value == 1 && *refs < 255) {
            bool held = q->down[code]; (*refs)++;
            if (!held) key_emit(&native, code, true, tick);
        } else if (value == 0 && *refs) {
            (*refs)--;
            if (!*refs && !q->native_down[code]) key_emit(&native, code, false, tick);
        } else if (value == 2 && *refs) key_emit(&native, code, true, tick);
        else drop = value == 1 ? "key_refcount" : value == 0 ? "unbalanced_release" :
                    value == 2 ? "repeat_without_press" : "key_value";
    } else if (type == INPUT_KEY) {
        drop = "key_code";
    } else if (type == INPUT_BTN && code < 3 && (value == 0 || value == 1)) {
        uint8_t *refs = &q->usb_buttons[code];
        unsigned before = q->stats.buttons;
        if (value && *refs < 255) (*refs)++; else if (!value && *refs) (*refs)--;
        unsigned mask = 1u << code;
        if (*refs || (q->native_buttons & mask)) q->stats.buttons |= mask;
        else q->stats.buttons &= ~mask;
        if ((before ^ q->stats.buttons) & mask) enqueue(&native, INPUT_BTN, code, !!(q->stats.buttons & mask), tick);
    } else if (type == INPUT_REL && code <= INPUT_WHEEL && value)
        enqueue(&native, INPUT_REL, code, value, tick);
    native.event_source = 0; native.event_generation = 0;
    irq_restore(f);
    /* Diagnostics must not hold the queue's IRQ exclusion across serial I/O. */
    if (drop) usb_event_drop(drop,type,code,value,generation);
}
void input_usb_event(uint16_t type, uint16_t code, int32_t value, gen_t generation, uint64_t tick)
{ input_external_event(type,code,value,generation,tick,false); }
void input_serial_event(uint16_t type, uint16_t code, int32_t value, gen_t generation, uint64_t tick)
{ input_external_event(type,code,value,generation,tick,true); }

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

static void touchpad_byte(struct controller *c, uint8_t b, uint64_t tick);
static void touchpad_loss(struct controller *c);
static void touchpad_flush(struct controller *c, uint64_t tick);

static void mouse_byte(struct controller *c, uint8_t b, uint64_t tick)
{
    if (c->touchpad.info.protocol) { touchpad_byte(c, b, tick); return; }
    struct input_queue *q = &c->queue;
    if (q->mouse_pos && tick - q->mouse_tick > MOUSE_PACKET_GAP_MS) {
        q->mouse_pos = 0;
        q->stats.resync++;
    }
    q->mouse_tick = tick;
    if (!q->mouse_pos && (!(b & 8) || (b & 0xC0))) {
        q->stats.resync++;              /* count every rejected candidate header */
        return;
    }
    q->packet[q->mouse_pos++] = b;
    if (q->mouse_pos != (c->stats.aux_id ? 4 : 3))
        return;
    if (c->stats.aux_id == 4 && (q->packet[3] & 0xC0)) {
        /* Unsupported extensions/corruption: retain a possible suffix header.
         * No packet has been published, including its buttons or motion. */
        q->stats.resync++;
        unsigned skip = 1;
        while (skip < 4 && (!(q->packet[skip] & 8) || (q->packet[skip] & 0xC0))) skip++;
        q->mouse_pos = (uint8_t)(4 - skip);
        memmove(q->packet, q->packet + skip, q->mouse_pos);
        return;
    }
    q->mouse_pos = 0;
    uint8_t h = q->packet[0];
    /* PS/2 axes are signed NINE-bit values, not int8_t. Screen y grows
     * down, the device's y grows up. Zero payload with a sign bit means
     * zero (Linux standard-packet convention), not an artificial -256. */
    int x = q->packet[1] ? (int)q->packet[1] - ((h & 0x10) ? 256 : 0) : 0;
    int y = q->packet[2] ? (int)q->packet[2] - ((h & 0x20) ? 256 : 0) : 0;
    q->native_buttons = h & 7;
    int wheel = 0;
    if (c->stats.aux_id == 3)
        wheel = q->packet[3] - ((q->packet[3] & 0x80) ? 256 : 0);
    else if (c->stats.aux_id == 4) {
        wheel = (q->packet[3] & 15) - ((q->packet[3] & 8) ? 16 : 0);
        q->native_buttons |= (q->packet[3] & 0x30) >> 1;
    }
    unsigned buttons = q->native_buttons;
    for (unsigned i = 0; i < 3; i++) if (q->usb_buttons[i]) buttons |= 1u << i;
    unsigned changed = buttons ^ q->stats.buttons;
    q->stats.buttons = buttons;        /* RESYNC snapshots this complete packet */
    if (x) enqueue(c, INPUT_REL, INPUT_X, x, tick);
    if (y) enqueue(c, INPUT_REL, INPUT_Y, -y, tick);
    if (wheel) enqueue(c, INPUT_REL, INPUT_WHEEL, -wheel, tick);
    for (unsigned i = 0; i < 5; i++)
        if (changed & (1u << i))
            enqueue(c, INPUT_BTN, (uint16_t)i, !!(buttons & (1u << i)), tick);
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

static void input_retry(void);
bool input_read(struct input_event *out)
{
    input_retry();
    keyboard_preferences_service();
    uint32_t f = irq_save();
    touchpad_flush(&native, g_ticks);
    irq_restore(f);
    return dequeue(&native, out);
}
/* sec-k07: dequeue only while the queue feeds the caller's domain, in the
 * same IRQ-exclusive interval as the domain test (input_read may yield). */
bool input_read_domain(struct input_event *out, bool trusted)
{
    if (!out) return false;
    input_retry();
    keyboard_preferences_service();
    uint32_t f = irq_save();
    touchpad_flush(&native, g_ticks);
    bool have = secure_trusted == trusted && dequeue(&native, out);
    irq_restore(f);
    return have;
}
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
        if (e->value != 2) d->key_transitions++;
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
    if (port == DATA_PORT) {
        /* IBM OBF-to-data pacing, never a status wait. The generic udelay
         * escape permits a million clock reads. Do not inherit it into an
         * IRQ: cap this sampling loop and report an unverified delay once.
         * The calibrated path finishes after 7 us. A sample above 100 us
         * or exhaustion of the clock-sampling bound gets a record. */
        uint64_t target=(g_tsc_per_ms*7+999)/1000;
        uint64_t bound=(g_tsc_per_ms+9)/10;
        uint64_t start=ktime_cycles(), elapsed=0;
        unsigned polls=0;
        if (target) for (;polls<256;polls++) {
            elapsed=ktime_cycles()-start;
            if (elapsed>=target) break;
        }
        if (!target || polls==256 || elapsed>bound) {
            static bool reported;
            if (!reported) {
                reported=true;
                klog("[i8042] pacing reason=clock_bound port=60 bound_us=100 poll_limit=256");
            }
        }
    }
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
    uint8_t byte = c->io->read(c->io_arg, port);
    if (c->init_recording && port == STATUS_PORT) c->init_status = byte;
    else if (c->init_recording && port == DATA_PORT) {
        struct i8042_init_record *r = &c->init_records[c->init_count];
        if (!r->bytes) r->first = byte;
        r->reply = byte;                /* includes discarded/error-status bytes */
        r->status_reply = c->init_status;
        r->bytes++;
    }
    return byte;
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

static void capture_at(struct controller *c, uint8_t status, uint8_t b, uint64_t tick)
{
    bool aux = (status & AUX) != 0;
    /* Boot traffic predates the configured raw set-2/negotiated mouse protocol.
     * It must not poison decoder state while draining for initialization. */
    if (c->init_recording && (!c->stats.pending_command || aux != c->reply_aux)) {
        c->stats.drained++;
        return;
    }
    struct input_queue *q = &c->queue;
    q->stats.bytes++;
    if (status & BAD_STATUS) {
        q->stats.errors++;
        q->stats.state_lost = true;
        if (aux) { q->mouse_pos = 0; touchpad_loss(c); }
        else { q->extended = q->breaking = false; q->pause_pos = 0; }
        if (c->stats.pending_command && aux == c->reply_aux) {
            c->reply_error = -I8042_EIO;
            c->reply_ready = true;
        }
        return;
    }
    if (c->stats.pending_command && !c->reply_ready && aux == c->reply_aux &&
        (c->reply_any || b == 0xFA || b == 0xFE || b == 0xFC || b == 0xFD)) {
        if (c->reply_data_phase) {
            c->reply_data[c->reply_count++] = b;
            c->reply_ready = c->reply_count == c->reply_wanted;
        } else {
            c->reply = b;
            if (!c->reply_any && b == 0xFA && c->reply_wanted) {
                c->reply_any = c->reply_data_phase = true;
            } else c->reply_ready = true;
        }
        return;
    }
    if (c->init_recording) { c->stats.drained++; return; }
    if (aux) mouse_byte(c, b, tick);
    else keyboard_byte(c, b, tick);
}

static void capture(struct controller *c, uint8_t status, uint8_t b)
{ capture_at(c, status, b, c->io->now(c->io_arg)); }

/* fx-3 (T23 trial of 9a60215f: `irqoff_section=i8042_sample
 * irqoff_max_us=2156`): the command engine samples the controller with
 * interrupts enabled. Each poll used to run status read, data read and the
 * decoder with IF clear; a setup or reset wait spends most of its time in
 * that section, so a firmware SMI (2.1 ms on the T23) landing there became a
 * 2 ms interrupt-off section of the kernel. IF=0 bought no exclusion:
 * - while the native IRQ path is live (handlers installed, accepting),
 *   input_irq is the only reader of the data port; a poll reads the
 *   non-destructive status register only and every byte, replies included,
 *   reaches capture_at through input_work (event-driven, Linux libps2
 *   ps2_handle_ack/ps2_handle_response model);
 * - otherwise (setup, stop, the fault fixture) input_irq is not installed or
 *   returns before any port access, input_work drops bytes while
 *   !accepting, and the command mutex holder is the only port user.
 * IBM PS/2 Hardware Interface Technical Reference (keyboard/auxiliary
 * controller, status register): reading 64h has no side effect; reading 60h
 * clears OBF. Only the data read must have one owner. */
static bool irq_path_live(const struct controller *c)
{
    return c == &native && c->accepting;
}

static bool capture_one(struct controller *c, bool discard)
{
    if (irq_path_live(c)) return false;     /* input_irq owns port 60h */
    uint8_t status = read_port(c, STATUS_PORT);
    bool have = status & OBF;
    if (have) {
        uint8_t b = read_port(c, DATA_PORT);
        if (discard) c->stats.drained++;
        else capture(c, status, b);
    }
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
        /* fx-3: IF stays set. Pending output is consumed first: by this
         * poll when the IRQ path is not live, by input_irq when it is. */
        uint8_t status = read_port(c, STATUS_PORT);
        if (status & OBF) {
            if (!irq_path_live(c)) capture(c, status, read_port(c, DATA_PORT));
        } else if (!(status & IBF)) {
            write_port(c, port, b);
            return 0;
        }
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
    c->reply_count = c->reply_wanted = 0;
    c->reply_data_phase = false;
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

/* No response is specified for these controller writes. During setup, finish
 * their IBF/EC response phase before expecting a device reply. Never run this
 * filter inside a device ACK wait, nor for AB/A9's diagnostic result byte. */
static int controller_ack_drain(struct controller *c, struct poll_deadline *d)
{
    if (!c->init_recording) return 0;
    bool seen = false, idle = false;
    uint64_t quiet_end = 0;
    for (;;) {
        int err = poll_check(c, d);
        if (err) return err;
        /* Setup only (init_recording): the IRQ path is not live; IF set. */
        uint8_t status = read_port(c, STATUS_PORT);
        if (status & OBF) {
            uint8_t byte = read_port(c, DATA_PORT);
            c->stats.drained++;
            if (!(status & (AUX | BAD_STATUS)) && byte == 0xFA) {
                c->init_records[c->init_count].stray_ack = 1;
                if (seen) err = -I8042_EPROTO;
                seen = true;
            }
        }
        if (err) return err;
        if (!(status & IBF)) {
            if (!idle) { quiet_end = c->io->now(c->io_arg) + 1; idle = true; }
            if (!(status & OBF) && c->io->now(c->io_arg) - quiet_end < (1ull << 63))
                return 0;
        } else idle = false;
        poll_pause(c, d);
    }
}

static int device_exchange_data(struct controller *c, bool aux, uint8_t b,
                                uint64_t outer, uint8_t *data, unsigned count)
{
    if (count > sizeof(c->reply_data) || (count && !data)) return -EINVAL;
    if (c->stats.quarantined) return -I8042_EIO;
    /* Reset is not qualified, even for a selected fault fixture. */
    if (b == 0xFF) return -EINVAL;
    uint64_t start = c->io->now(c->io_arg);
    struct poll_deadline d = limit(c, outer, I8042_REPLY_MS);
    c->stats.commands++;
    int err = drain(c, &d, false);
    for (unsigned attempt = 0; !err; attempt++) {
        if (aux) {
            err = write_when_ready(c, &d, STATUS_PORT, 0xD4);
            if (!err) err = controller_ack_drain(c, &d);
        }
        if (err) break;
        expect_reply(c, aux, false);
        c->reply_wanted = (uint8_t)count;
        err = write_when_ready(c, &d, DATA_PORT, b);
        if (err) break;
        uint8_t reply = 0;
        err = wait_reply(c, &d, &reply);
        if (err) break;
        if (reply == 0xFA) {
            if (count) memcpy(data, c->reply_data, count);
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

static int device_exchange(struct controller *c, bool aux, uint8_t b, uint64_t outer, uint8_t *data)
{
    return device_exchange_data(c, aux, b, outer, data, data ? 1 : 0);
}

static int device_command(struct controller *c, bool aux, uint8_t b, uint64_t outer)
{
    return device_exchange(c, aux, b, outer, 0);
}

/* Thread consumer only. The command engine needs the input worker to drain
 * IRQ replies, so never execute transactions from that same worker or IRQ. */
static void quarantine(struct controller *, int);
static int keyboard_program(unsigned repeat, unsigned leds)
{
    if (!native.stats.active || native.stats.firmware || native.stats.quarantined)
        return 0; /* preference remains usable by USB and a later activation */
    if (!(read_eflags() & 0x200) || !g_current) return -EINVAL;
    kmutex_lock(&native.mutex);
    int err = 0;
    if (native.stats.active && !native.stats.quarantined) {
        uint64_t end = native.io->now(native.io_arg) + I8042_REPLY_MS * 4;
        if (repeat != programmed_repeat) {
            err = device_command(&native, false, 0xf3, end);
            if (!err) err = device_command(&native, false, (uint8_t)repeat, end);
            if (!err) programmed_repeat = repeat;
        }
        if (!err && leds != programmed_leds) {
            err = device_command(&native, false, 0xed, end);
            if (!err) err = device_command(&native, false, (uint8_t)leds, end);
            if (!err) programmed_leds = leds;
        }
    }
    if(err)quarantine(&native,err);
    kmutex_unlock(&native.mutex);
    return err;
}
int input_keyboard_settings(const struct ciuki_keyboard_settings *s)
{
    if (!s || s->version != 1 || s->size != sizeof(*s) || s->delay > 3 || s->speed > 31)
        return -EINVAL;
    keyboard_policy_requested=true;repeat_delay=s->delay;repeat_speed=s->speed;
    return keyboard_program((s->delay << 5) | typematic_rate(s->speed),lock_leds);
}
static void keyboard_preferences_service(void)
{
    /* One attempt per changed state; a missing keyboard must not stall every
     * read. A subsequent preference change or activation permits another. */
    static unsigned attempted_leds = ~0u;static gen_t attempted_generation;
    /* Desktop's first explicit policy request initializes the default LEDs.
     * An ordinary read with no lock change must not start a transaction. */
    if ((!keyboard_policy_requested && !lock_leds && programmed_leds==~0u) ||
        (attempted_leds==lock_leds && attempted_generation==native.stats.generation) ||
        !native.stats.active || native.stats.firmware || native.stats.quarantined) return;
    attempted_leds=lock_leds;attempted_generation=native.stats.generation;
    (void)keyboard_program((repeat_delay << 5) | typematic_rate(repeat_speed), lock_leds);
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
    int err = write_when_ready(c, &d, STATUS_PORT, cmd);
    return err ? err : controller_ack_drain(c, &d);
}
static int config_write(struct controller *c, uint8_t cfg, uint64_t outer)
{
    struct poll_deadline d = limit(c, outer, I8042_REPLY_MS);
    int err = write_when_ready(c, &d, STATUS_PORT, 0x60);
    if (!err) err = controller_ack_drain(c, &d);
    if (!err) err = write_when_ready(c, &d, DATA_PORT, cfg);
    if (!err) err = controller_ack_drain(c, &d);
    if (!err) c->stats.config = cfg;
    return err;
}

const char *i8042_init_step_name(enum i8042_init_step step)
{
    static const char *const names[] = {
        "self_test", "iface_kbd", "iface_aux", "config_read", "config_write",
        "flush", "enable", "reset_kbd", "reset_aux",
    };
    return (unsigned)step < ARRAY_SIZE(names) ? names[step] : "none";
}

void i8042_init_format(char out[I8042_INIT_LINE], unsigned index,
                      const struct i8042_init_record *r)
{
    char reply[5] = "none", first[5] = "none";
    if (r->bytes) {
        ksnprintf(reply, sizeof(reply), "%02x", r->reply);
        ksnprintf(first, sizeof(first), "%02x", r->first);
    }
    ksnprintf(out, I8042_INIT_LINE,
              "step=%s index=%u command=%02x reply=%s status_before=%02x status_after=%02x elapsed_ms=%u result=%d first=%s bytes=%u status_reply=%02x stray_ack=%u",
              i8042_init_step_name(r->step), index + 1, r->command, reply,
              r->status_before, r->status_after, r->elapsed_ms, r->result, first, r->bytes, r->status_reply, r->stray_ack);
}

bool i8042_init_record(unsigned index, struct i8042_init_record *out)
{
    if (!out) return false;
    uint32_t f = irq_save();
    bool have = !native.stats.firmware && index < native.init_count;
    if (have) *out = native.init_records[index];
    irq_restore(f);
    return have;
}

static void init_begin(struct controller *c, enum i8042_init_step step, uint8_t command)
{
    /* The one fixed sequence below has fifteen stages, no retry loop. */
    if (c->init_count >= I8042_INIT_STEPS) panic("i8042 init trace capacity");
    struct i8042_init_record *r = &c->init_records[c->init_count];
    *r = (struct i8042_init_record){ .step = step, .command = command };
    c->init_started = c->io->now(c->io_arg);
    r->status_before = read_port(c, STATUS_PORT);
    c->init_recording = true;
}

static void init_finish(struct controller *c, int result)
{
    struct i8042_init_record *r = &c->init_records[c->init_count];
    r->status_after = read_port(c, STATUS_PORT);
    r->elapsed_ms = (uint32_t)(c->io->now(c->io_arg) - c->init_started);
    r->result = result;
    c->init_recording = false;
    c->init_count++;
}

/* Grouped stages retain the command and raw replies for their last attempted
 * operation, so an ACK to an earlier command cannot disguise a missing ACK. */
static void init_command(struct controller *c, uint8_t command)
{
    struct i8042_init_record *r = &c->init_records[c->init_count];
    r->command = command;
    r->bytes = 0;
    r->first = r->reply = 0;
    r->status_reply = 0;
}

static int init_ctl_write(struct controller *c, uint8_t command, uint64_t end)
{
    init_command(c, command);
    return controller_write(c, command, end);
}

static int init_device(struct controller *c, bool aux, uint8_t command,
                       uint64_t end, uint8_t *reply)
{
    init_command(c, command);
    return device_exchange(c, aux, command, end, reply);
}

static bool mouse_id_supported(uint8_t id)
{
    return id == 0 || id == 3 || id == 4;
}

/* Reporting is disabled by F5. Only a fresh ID proves the stream format;
 * a failed transaction cannot prove any fallback and follows quarantine. */
static int init_mouse_relative(struct controller *c, uint64_t end)
{
    uint8_t id = 0;
    int err = init_device(c, true, 0xF2, end, &id);
    if (!err && !mouse_id_supported(id)) err = -I8042_EPROTO;
    if (err) return err;
    c->stats.aux_id = id;
    static const uint8_t rates[2][3] = { { 200, 100, 80 }, { 200, 200, 80 } };
    for (unsigned stage = 0; stage < ARRAY_SIZE(rates) && id != 4; stage++) {
        for (unsigned i = 0; !err && i < ARRAY_SIZE(rates[stage]); i++) {
            err = init_device(c, true, 0xF3, end, 0);
            if (!err) err = init_device(c, true, rates[stage][i], end, 0);
        }
        if (!err) err = init_device(c, true, 0xF2, end, &id);
        if (!err && !mouse_id_supported(id)) err = -I8042_EPROTO;
        if (err) return err;
        c->stats.aux_id = id;
        if (!id) break;                /* proven standard three-byte fallback */
    }
    c->queue.mouse_pos = 0;
    return 0;
}

#include "i8042_touchpad.inc"

/* A write is not complete while IBF remains asserted. OBF alone is input to
 * drain, not proof that the controller is unusable (Linux controller_check). */
static int init_idle(struct controller *c, uint64_t end)
{
    struct poll_deadline d = limit(c, end, I8042_REPLY_MS);
    for (;;) {
        int err = poll_check(c, &d);
        if (err) return err;
        uint8_t status = read_port(c, STATUS_PORT);
        if (status & OBF) (void)capture_one(c, true);
        else if (!(status & IBF)) return 0;
        poll_pause(c, &d);
    }
}

static int init_config_read(struct controller *c, uint8_t *cfg, uint64_t end)
{
    uint8_t previous = 0;
    for (unsigned i = 0; i < 10; i++) {
        if (i) c->io->pause(c->io_arg);
        int err = controller_reply(c, 0x20, cfg, end);
        if (err) return err;
        if (i && *cfg == previous) return 0;
        previous = *cfg;
    }
    return -I8042_EIO;
}

static int init_config_write(struct controller *c, uint8_t cfg, uint64_t end, bool verify)
{
    init_command(c, 0x60);
    int err = config_write(c, cfg, end);
    if (!err) err = init_idle(c, end);
    if (!err && verify) {
        uint8_t reply;
        /* Ports are inhibited, or both devices acknowledged F5 and the
         * buffers were drained: no scan/packet may impersonate CTR. */
        err = init_config_read(c, &reply, end);
        if (!err && (reply & (TRANSLATE | DISABLED | IRQ_BITS)) !=
                    (cfg & (TRANSLATE | DISABLED | IRQ_BITS))) err = -I8042_EIO;
    }
    return err;
}

static int init_iface_test(struct controller *c, uint8_t command, uint8_t *reply, uint64_t end)
{
    struct poll_deadline d = limit(c, end, I8042_REPLY_MS);
    int err = drain(c, &d, true);
    if (!err) {
        expect_reply(c, false, true);
        err = write_when_ready(c, &d, STATUS_PORT, command);
        if (!err) {
            err = wait_reply(c, &d, reply);
            /* Only a missing diagnostic reply is advisory. Failure to issue
             * the command, bad status or a stalled clock remains an error. */
            if (err == -I8042_ETIMEDOUT) err = 0;
        }
    }
    c->stats.pending_command = false;
    return err;
}

static int init_aux_test(struct controller *c, uint64_t end)
{
    uint8_t reply = 0xFF;
    int err = init_iface_test(c, 0xA9, &reply, end);
    if (err || !reply) return err;
    /* External line tests can fail on a functioning notebook AUX port.
     * Only a correctly tagged internal loopback qualifies that result.
     * Keep A9 as first raw byte and D3 echo as last, in the same record.
     * Real device presence still requires every subsequent device ACK/ID. */
    struct poll_deadline d = limit(c, end, I8042_REPLY_MS);
    err = write_when_ready(c, &d, STATUS_PORT, 0xD3);
    if (!err) err = controller_ack_drain(c, &d);
    if (!err) {
        expect_reply(c, true, true);
        err = write_when_ready(c, &d, DATA_PORT, 0x5A);
        if (!err) err = wait_reply(c, &d, &reply);
    }
    c->stats.pending_command = false;
    return err ? err : (reply == 0x5A ? 0 : -I8042_EIO);
}

static void quarantine(struct controller *c, int err)
{
    bool active = c == &native && c->stats.active;
    c->accepting = c->stats.active = false;
    c->stats.pending_command = false;
    c->stats.last_error = err;
    c->stats.quarantined = true;
    if (active) input_devices_changed(c->stats.firmware ? INPUT_FIRMWARE : INPUT_NATIVE, false);
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

/* IRQ only captures a byte. Decoding, resync and guest delivery are thread
 * work, one byte per turn. Storage is permanent; generation rejects stop /
 * restart completions. Overflow is retained and reported by the consumer. */
static struct { uint8_t status, byte; gen_t generation; uint64_t tick; } raw_input[64];
static unsigned raw_read, raw_count, raw_lost;
static gen_t raw_generation;
static void input_work(void *arg)
{
    (void)arg;
    uint32_t f = irq_save();
    if (raw_lost) {
        if (native.accepting && raw_generation == native.stats.generation) {
            native.queue.mouse_pos = 0;
            touchpad_loss(&native);
            native.queue.stats.overflow += raw_lost;
            native.queue.stats.bytes += raw_lost;
            native.queue.stats.state_lost = true;
        }
        raw_lost = 0;
    }
    bool have = raw_count != 0;
    uint8_t status = 0, byte = 0; gen_t generation = 0; uint64_t tick = 0;
    if (have) {
        unsigned at = raw_read;
        status = raw_input[at].status; byte = raw_input[at].byte;
        generation = raw_input[at].generation; tick = raw_input[at].tick;
        raw_read = (raw_read + 1) % ARRAY_SIZE(raw_input); raw_count--;
    }
    bool more = raw_count != 0;
    irq_restore(f);
    if (have && native.accepting && generation == native.stats.generation)
        capture_at(&native, status, byte, tick);
    if (more) kwork_queue(input_work, 0);
}

static void input_retry(void)
{
    uint32_t f = irq_save(); bool pending = raw_count || raw_lost; irq_restore(f);
    if (pending) kwork_queue(input_work, 0);
}
static void input_irq(struct trap_frame *tf)
{
    (void)tf;
    if (crit_label) crit_label("i8042_irq");
    if (!native.accepting || native.stats.quarantined || !claims_active(&native)) return;
    uint8_t status = read_port(&native, STATUS_PORT);
    if (!(status & OBF)) return;
    uint8_t byte = read_port(&native, DATA_PORT);
    if (raw_generation != native.stats.generation) {
        raw_generation = native.stats.generation; raw_read = raw_count = raw_lost = 0;
    }
    if (raw_count == ARRAY_SIZE(raw_input)) { if (raw_lost != UINT32_MAX) raw_lost++; }
    else {
        unsigned at = (raw_read + raw_count) % ARRAY_SIZE(raw_input);
        raw_input[at].status = status; raw_input[at].byte = byte;
        raw_input[at].generation = native.stats.generation;
        raw_input[at].tick = native.io->now(native.io_arg); raw_count++;
    }
    kwork_queue(input_work, 0); /* coalesced; dispatcher owns EOI */
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
    /* The boot activation owner resolves provenance and firmware failure
     * before native claims. Hardware sequencing below is unchanged. */
    if (drivers_input_firmware()) {
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
    /* IBM controller command-byte bits 0/1 gate IRQs; AD/A7 inhibit the
     * interfaces. Keep PIC lines open across clean stop/reactivation. */
    uint8_t cfg = 0, reply = 0;
    init_begin(&native, I8042_INIT_FLUSH, 0xAD);
    err = init_ctl_write(&native, 0xAD, end);
    if (!err) err = init_ctl_write(&native, 0xA7, end);
    if (!err) err = init_idle(&native, end);
    init_finish(&native, err);
    if (!err) {
        init_begin(&native, I8042_INIT_CONFIG_READ, 0x20);
        err = init_config_read(&native, &cfg, end);
        native.stats.initial_config = cfg;
        init_finish(&native, err);
    }
    cfg = (cfg | DISABLED) & ~(IRQ_BITS | TRANSLATE);
    if (!err) {
        init_begin(&native, I8042_INIT_CONFIG_WRITE, 0x60);
        err = init_config_write(&native, cfg, end, false);
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_SELF_TEST, 0xAA);
        err = controller_reply(&native, 0xAA, &reply, end);
        if (!err && reply != 0x55) err = -I8042_EIO;
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_CONFIG_WRITE, 0x60);
        /* AA can reset CTR and re-enable ports. Inhibit again before reading
         * the configuration; verify its actual nontranslated/IRQ-off state. */
        err = init_ctl_write(&native, 0xAD, end);
        if (!err) err = init_ctl_write(&native, 0xA7, end);
        if (!err) err = init_config_write(&native, cfg, end, true);
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_IFACE_KBD, 0xAB);
        err = init_iface_test(&native, 0xAB, &reply, end);
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_IFACE_AUX, 0xA9);
        err = init_aux_test(&native, end);
        init_finish(&native, err);
    }
    cfg &= ~DISABLED;
    if (!err) {
        init_begin(&native, I8042_INIT_ENABLE, 0x60);
        err = init_config_write(&native, cfg, end, false);
        if (!err) err = init_ctl_write(&native, 0xAE, end);
        if (!err) err = init_ctl_write(&native, 0xA8, end);
        if (!err) err = init_idle(&native, end);
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_RESET_KBD, 0xF5);
        err = init_device(&native, false, 0xF5, end, 0); /* disable scanning */
        if (!err) err = init_device(&native, false, 0xF0, end, 0);
        if (!err) err = init_device(&native, false, 0x02, end, 0); /* raw set 2 */
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_RESET_AUX, 0xF5);
        /* F6 defaults do not prove ID 0. Negotiate with reporting disabled;
         * no FF/BAT reset or unproven three-byte fallback. */
        err = init_device(&native, true, 0xF5, end, 0);
        if (!err) err = init_device(&native, true, 0xF6, end, 0);
        if (!err) err = init_mouse_protocol(&native, end);
        if (!err) err = init_device(&native, true, 0xEA, end, 0); /* stream */
        if (!err && !native.touchpad.info.protocol)
            err = init_device(&native, true, 0xE6, end, 0); /* 1:1 */
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_CONFIG_WRITE, 0x60);
        err = init_ctl_write(&native, 0xAD, end);
        if (!err) err = init_ctl_write(&native, 0xA7, end);
        if (!err) err = init_config_write(&native, cfg | DISABLED, end, true);
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_ENABLE, 0x60);
        /* Both devices acknowledged F5. Enable/configure, then read back CTR
         * BEFORE F4 allows untagged scans/packets to race that reply. The old
         * last write was unverified and could leave an EC port inhibited. */
        err = init_config_write(&native, cfg | IRQ_BITS, end, false);
        if (!err) err = init_ctl_write(&native, 0xAE, end);
        if (!err) err = init_ctl_write(&native, 0xA8, end);
        if (!err) err = init_idle(&native, end);
        if (!err) {
            init_command(&native, 0x20);
            err = init_config_read(&native, &reply, end);
            if (!err && (reply & (TRANSLATE | DISABLED | IRQ_BITS)) != IRQ_BITS)
                err = -I8042_EIO;
        }
        for (unsigned i = 0; !err && i < ARRAY_SIZE(native.handle); i++)
            err = registry_activate(native.handle[i], native.claim_gen[i]);
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_RESET_KBD, 0xF4);
        err = init_device(&native, false, 0xF4, end, 0);
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_RESET_AUX, 0xF4);
        err = init_device(&native, true, 0xF4, end, 0);
        init_finish(&native, err);
    }
    if (!err) {
        init_begin(&native, I8042_INIT_FLUSH, 0x00); /* status/data only */
        err = init_idle(&native, end);
        init_finish(&native, err);
    }
    if (err) quarantine(&native, err);
    else {
        uint32_t f = irq_save();
        native.accepting = native.stats.active = true;
        irq_set_handler(1, input_irq);
        irq_set_handler(12, input_irq);
        irq_restore(f);
        pic_unmask(1);
        pic_unmask(12);                 /* PIC service also enables cascade */
    }
    native.stats.last_elapsed_ms = (uint32_t)(physical_now(0) - start);
    native.stats.last_error = err;
    /* Console/serial latency is outside the hardware transaction deadline. */
    if (!err) input_devices_changed(INPUT_NATIVE, true);
    for (unsigned i = 0; i < native.init_count; i++) {
        char line[I8042_INIT_LINE];
        i8042_init_format(line, i, &native.init_records[i]);
        klog("[i8042] %s", line);
    }
    if (!err && native.touchpad.info.fallback_error)
        klog("[i8042] touchpad fallback=%d identity=%x", native.touchpad.info.fallback_error,
             native.touchpad.info.identity);
    if (!err) klog("[i8042] aux id=%u protocol=%s", native.stats.aux_id,
                   touchpad_name(&native) ? touchpad_name(&native) :
                   native.stats.aux_id == 4 ? "explorer" :
                   native.stats.aux_id == 3 ? "intellimouse" : "ps2");
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
    programmed_repeat = programmed_leds = ~0u;
    native.accepting = false;          /* reject submissions before stopping sources */
    /* Command-byte IRQ_BITS are cleared and verified below. A latched
     * edge cannot consume controller replies after accepting is clear. */
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
    if (!err && (read_port(&native, STATUS_PORT) & (IBF | OBF))) err = -I8042_EIO;
    for (unsigned i = 0; !err && i < ARRAY_SIZE(native.handle); i++)
        err = registry_quiesce(native.handle[i], native.claim_gen[i], idle_proof);
    if (err) quarantine(&native, err);
    else {
        for (unsigned i = 0; i < ARRAY_SIZE(native.handle); i++)
            registry_release(native.handle[i], native.claim_gen[i]);
        native.stats.active = false;
        native.initialized = false;    /* fresh generation/state on next activation */
        input_devices_changed(INPUT_NATIVE, false);
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
    if (!generation || !drivers_input_firmware()) return false;
    uint32_t f = irq_save();
    if (native.stats.quarantined || (native.stats.active && !native.stats.firmware)) {
        irq_restore(f);
        return false;
    }
    bool attached = !native.stats.active;
    memset(&native.queue, 0, sizeof(native.queue));
    native.stats.firmware = native.stats.active = true;
    native.stats.generation = generation;
    irq_restore(f);
    if (attached) input_devices_changed(INPUT_FIRMWARE, true);
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
    bool detached = native.stats.active && native.stats.firmware && gen_matches(native.stats.generation, generation);
    if (native.stats.firmware && gen_matches(native.stats.generation, generation)) {
        native.stats.quarantined = true;
        native.stats.active = false;
        native.stats.last_error = -I8042_EIO;
    }
    irq_restore(f);
    if (detached) input_devices_changed(INPUT_FIRMWARE, false);
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
        q->stats.keys_down = 0;
        memset(q->down, 0, sizeof(q->down));
        memset(q->native_down, 0, sizeof(q->native_down));
        queue_resync(&native, e->tick);
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

static int fault_refused(const char *reason, int err)
{
    klog("[i8042] fault_begin refused reason=%s error=%d", reason, err);
    return err;
}

int i8042_fault_begin(const struct i8042_test_io *io, void *arg)
{
    if (!fault_selected()) {
        /* The loader supplies a byte count, not a terminated string. Keep
         * diagnostics bounded even when the selector itself is invalid. */
        char text[CIUKI_TEST_REQ_MAX + 1];
        unsigned n = g_boot.test_request_len;
        if (n > CIUKI_TEST_REQ_MAX) n = CIUKI_TEST_REQ_MAX;
        memcpy(text, g_boot.test_request, n);
        text[n] = 0;
        klog("[i8042] fault_begin refused reason=selector error=%d selector='%s' length=%u flags=%08x",
             -ENOSYS, text, g_boot.test_request_len, g_boot.flags);
        return -ENOSYS;
    }
    if (!(read_eflags() & 0x200)) return fault_refused("interrupts_disabled", -EINVAL);
    if (!g_current) return fault_refused("no_task", -EINVAL);
    if (fixture) return fault_refused("fixture_present", -EINVAL);
    if (!io || !io->read || !io->write || !io->now || !io->pause)
        return fault_refused("callbacks", -EINVAL);
    fixture = &fixture_storage;
    controller_init(fixture, io, arg);
    fixture->stats.generation = gen_alloc();
    if (!fixture->stats.generation) {
        i8042_fault_end();
        return fault_refused("generation", -ENOSPC);
    }
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
    memset(fixture, 0, sizeof(*fixture));
    fixture = 0;
}
