/* CiukiOS monitored-session guest devices inside CVSESSION (ring 0).
 * See session_devices.h. Freestanding OpenWatcom C: no libc, no allocator.
 */
#include "session_devices.h"
#include "guest_peripherals.h"

/* Services from session_devices.inc and session_video.inc (cdecl). */
extern uint32_t CVDEV_CALL cvdev_in(uint32_t port, uint32_t size);
extern void CVDEV_CALL cvdev_out(uint32_t port, uint32_t value, uint32_t size);
extern int CVDEV_CALL cvdev_raise_irq(uint32_t vm, uint32_t irq);
/* Linear address of the session VM's guest byte (session_vmm.inc); 0 if none. */
extern uint8_t *CVDEV_CALL cvdev_guest_linear(uint32_t vm, uint32_t physical);
extern uint32_t CVDEV_CALL cvdev_pm_state(void);
extern void CVDEV_CALL cvdev_fpu_save(void *area);
extern void CVDEV_CALL cvdev_fpu_restore(void *area);
extern void CVDEV_CALL cvvid_rdtsc(uint32_t *low, uint32_t *high);
/* DBOPL adapter compiled by clang for i686 COFF (session_opl.cpp). */
extern void *CVDEV_CALL cvop_create(unsigned rate);
extern void CVDEV_CALL cvop_write(void *chip, unsigned reg, unsigned value);
extern void CVDEV_CALL cvop_generate(void *chip, short *stereo, unsigned frames);

#define AUDIO_RATE      44100u
#define AUDIO_LEAD      8u        /* buffers queued ahead of the DMA engine */
#define KBC_DATA        0x60u
#define KBC_STATUS      0x64u

/* Everything that belongs to one session's device model: several VMs can
 * each own one. CVSESSION selects the instance of the VM it acts for
 * (cvdev_select); without a session the empty instance is inactive. The
 * report comes first, the model right after it: the report's 'CVDV' header
 * (set at DEV_BEGIN) also locates the model in a physical memory dump. */
struct cvdev_instance {
    cvdev_report stats;
    cvgp_state devices;
    uint32_t generation, active, caps, focused;
    uint32_t pull_keyboard, pull_mouse;
    uint32_t hotkey_enabled, ctrl_down, esc_break_pending;
    uint32_t tsc_per_us, last_low, last_high;
    void *opl_chip;
    uint8_t a20_data_pending, a20_output_pending;
    uint32_t audio_elapsed_us;
    uint32_t vm, wants_audio;             /* its VM; it streams to the AC'97 */
    uint32_t muted_us, muted_frac;        /* time not yet played muted */
    uint16_t trap_ports[96];
};

static struct cvdev_instance instance_none;
static struct cvdev_instance *I = &instance_none;
/* The session whose SB16/OPL output the one AC'97 stream plays (0: none),
 * and every session that wants to play (the others play muted). */
static struct cvdev_instance *A;
#define CVDEV_PLAYERS 8u
static struct cvdev_instance *players[CVDEV_PLAYERS];
/* Ctrl+Esc gave the keyboard back to the desktop's VM (several VMs only). */
uint32_t cvdev_release_request;
static uint8_t fpu_area[112];

/* AC'97 PCM-out streaming state. */
static uint32_t nam, nabm, audio_running, audio_rate;
static uint32_t *bdl;
static uint32_t buffer_linear[CVDEV_AUDIO_BUFFERS];
static uint32_t next_fill;
static uint16_t saved_master, saved_pcm, saved_ext, saved_rate;
static uint32_t saved_bdbar;

static void player_set(struct cvdev_instance *instance, int playing)
{
    unsigned i, slot = CVDEV_PLAYERS;
    for (i = 0; i != CVDEV_PLAYERS; ++i) {
        if (players[i] == instance) {
            if (!playing) players[i] = 0;
            return;
        }
        if (!players[i] && slot == CVDEV_PLAYERS) slot = i;
    }
    if (playing && slot != CVDEV_PLAYERS) players[slot] = instance;
}
static uint8_t saved_lvi;
static int16_t resample[CVDEV_AUDIO_FRAMES * 2 * 2];
static int16_t discard[CVDEV_AUDIO_FRAMES * 2];


static void zero(void *at, uint32_t count)
{
    uint8_t *p = (uint8_t *)at;
    while (count--) *p++ = 0;
}

/* ---- Model backend ---- */

static uint8_t CVGP_CALL memory_read8(void *opaque, uint32_t physical, int *valid)
{
    volatile uint8_t *p;
    const struct cvdev_instance *owner = opaque ? (const struct cvdev_instance *)opaque : I;
    /* ISA DMA reaches conventional memory only (the VGA window above is the
     * virtual adapter, never a DMA buffer). It is the memory of the session's
     * VM, which need not be the VM running now. */
    if (physical >= 0xa0000UL) { *valid = 0; return 0; }
    p = cvdev_guest_linear(owner->vm, physical);
    if (!p) { *valid = 0; return 0; }
    *valid = 1;
    return *p;
}

static void CVGP_CALL opl_write(void *opaque, uint16_t reg, uint8_t value)
{
    (void)opaque;
    ++I->stats.opl_writes;
    if (I->opl_chip) cvop_write(I->opl_chip, reg, value);
}

static int CVGP_CALL opl_generate(void *opaque, int16_t *stereo, unsigned frames)
{
    (void)opaque;
    if (!I->opl_chip) return 0;
    cvdev_fpu_save(fpu_area);
    cvop_generate(I->opl_chip, stereo, frames);
    cvdev_fpu_restore(fpu_area);
    return 1;
}

static void CVGP_CALL opl_reset(void *opaque, unsigned rate)
{
    (void)opaque;
    cvdev_fpu_save(fpu_area);
    I->opl_chip = cvop_create(rate);
    cvdev_fpu_restore(fpu_area);
}

/* ---- Virtual IRQs: the model's PIC only aggregates device requests (auto
 * EOI); the single virtual PIC the guest programs is Jemm's profile. ---- */

static void pump(void)
{
    uint8_t vector;
    unsigned irq, guard = 32;
    while (guard-- && cvgp_irq_acknowledge(&I->devices, I->generation, 1, &vector)) {
        irq = vector >= 0x70 ? vector - 0x70u + 8u : vector - 0x08u;
        if (irq == 0 || irq == 2 || irq > 15) continue;  /* timer: physical */
        if (cvdev_raise_irq(I->vm, irq)) ++I->stats.irqs_raised;
        else ++I->stats.irq_failures;
        if (irq == I->devices.sb.irq) ++I->stats.sb_blocks;
    }
    if (I->devices.last_error) {
        I->stats.last_error = I->devices.last_error;
        I->stats.last_port = I->devices.last_port;
        ++I->stats.model_errors;
        I->devices.last_error = 0;
    }
}

/* ---- Physical 8042 bytes: captured once, routed by focus. ---- */

/* Ctrl+Esc while this session's VM has the keyboard: neither byte reaches
 * the guest (its Ctrl is released by the focus change) and the VM manager
 * gives the focus back. 1 when the byte was taken. */
static int hotkey(uint8_t value)
{
    uint8_t key = value & 0x7f;
    if (key == 0x1d) I->ctrl_down = !(value & 0x80);
    if (!I->hotkey_enabled) return 0;
    if (value == 0x81 && I->esc_break_pending) {
        I->esc_break_pending = 0;
        return 1;
    }
    if (value == 0x01 && I->ctrl_down) {
        I->esc_break_pending = 1;
        cvdev_release_request = 1;
        return 1;
    }
    return 0;
}

static void route_byte(uint8_t value, int auxiliary)
{
    if (!auxiliary && I->focused && hotkey(value)) return;
    if (auxiliary) {
        ++I->stats.aux_bytes;
        if (I->caps & CVGP_CAP_MOUSE) cvgp_aux_raw(&I->devices, I->generation, value);
    } else if (I->focused) {
        ++I->stats.keys_forwarded;
        cvgp_key_raw(&I->devices, I->generation, value);
    } else {
        /* The desktop does not read the keyboard while a DOS window runs;
         * an unfocused key belongs to no guest and is withheld. */
        ++I->stats.keys_dropped;
        cvgp_key_raw(&I->devices, I->generation, value);    /* resets prefix only */
    }
}

static int pull_physical(void)
{
    uint32_t status = cvdev_in(KBC_STATUS, 1);
    if (!(status & 1)) return 0;
    /* A byte for another VM stays in the controller for that VM to read. */
    if (status & 0x20 ? !I->pull_mouse : !I->pull_keyboard) return 0;
    route_byte((uint8_t)cvdev_in(KBC_DATA, 1), (status & 0x20) != 0);
    return 1;
}

int cvdev_irq_filter(uint32_t irq)
{
    if (!I->active || !(I->caps & (CVGP_CAP_KEYBOARD | CVGP_CAP_MOUSE))) return 0;
    if (irq != 1 && irq != 12) return 0;
    pull_physical();
    pump();
    return 1;
}

/* ---- Host time ---- */

/* Returns elapsed microseconds once at least 0.5 ms passed, else 0. Only
 * RDTSC here: this runs on every return to V86, which the desktop's
 * emulated instructions make very frequent. */
static uint32_t advance(void)
{
    uint32_t low, high, delta, us, pit;
    cvvid_rdtsc(&low, &high);
    delta = low - I->last_low;
    if (high - I->last_high > 1 || (high != I->last_high && low >= I->last_low))
        delta = 0xffffffffUL;
    if (delta < I->tsc_per_us * 500u) return 0;        /* at most every 0.5 ms */
    I->last_low = low;
    I->last_high = high;
    us = delta / I->tsc_per_us;
    if (us > 200000UL) us = 200000UL;
    pit = us * 1193u + (us * 182u) / 1000u;         /* 1.193182 MHz */
    cvgp_advance(&I->devices, I->generation, pit);
    return us;
}

/* ---- AC'97 PCM out (ICH NABM box at +10h) ---- */

static uint32_t pci_read(uint32_t address)
{
    cvdev_out(0xcf8, address, 4);
    return cvdev_in(0xcfc, 4);
}

static int ac97_find(void)
{
    uint32_t device, function, address, id, klass, command;
    for (device = 0; device != 32; ++device) {
        for (function = 0; function != 8; ++function) {
            address = 0x80000000UL | (device << 11) | (function << 8);
            id = pci_read(address);
            if ((id & 0xffff) == 0xffff) {
                if (!function) break;
                continue;
            }
            klass = pci_read(address + 8) >> 16;
            if ((id & 0xffff) != 0x8086 || klass != 0x0401) continue;
            nam = pci_read(address + 0x10) & 0xfffcUL;
            nabm = pci_read(address + 0x14) & 0xfffcUL;
            if (!nam || !nabm) return 0;
            command = pci_read(address + 4);
            cvdev_out(0xcf8, address + 4, 4);
            cvdev_out(0xcfc, command | 5u, 4);  /* I/O space and bus master */
            return 1;
        }
    }
    return 0;
}

static void ac97_reset_output(void)
{
    unsigned guard;
    cvdev_out(nabm + 0x1b, 0, 1);
    cvdev_out(nabm + 0x1b, 2, 1);
    for (guard = 0; guard != 100000u; ++guard)
        if (!(cvdev_in(nabm + 0x1b, 1) & 2)) break;
}

static void render_buffer(uint32_t index)
{
    int16_t *out = (int16_t *)buffer_linear[index];
    unsigned i, source, frames_in;
    struct cvdev_instance *current = I;
    if (!A) {                                   /* no session plays: silence */
        zero(out, CVDEV_AUDIO_FRAMES * 4u);
        return;
    }
    I = A;
    if (audio_rate == AUDIO_RATE) {
        cvgp_render_audio(&I->devices, I->generation, out, CVDEV_AUDIO_FRAMES, AUDIO_RATE);
    } else {
        /* Fixed-rate codec: synthesise at 44.1 kHz, nearest-sample stretch. */
        frames_in = (unsigned)((CVDEV_AUDIO_FRAMES * AUDIO_RATE) / audio_rate) + 1u;
        if (frames_in > CVDEV_AUDIO_FRAMES * 2u) frames_in = CVDEV_AUDIO_FRAMES * 2u;
        cvgp_render_audio(&I->devices, I->generation, resample, frames_in, AUDIO_RATE);
        for (i = 0; i != CVDEV_AUDIO_FRAMES; ++i) {
            source = (unsigned)((i * AUDIO_RATE) / audio_rate);
            if (source >= frames_in) source = frames_in - 1u;
            out[i * 2] = resample[source * 2];
            out[i * 2 + 1] = resample[source * 2 + 1];
        }
    }
    ++I->stats.buffers_rendered;
    I = current;
}

static void audio_service(void)
{
    uint32_t civ, status, queued, halted;
    struct cvdev_instance *current = I;
    if (!audio_running) return;
    I = A ? A : &instance_none;              /* the stream's report fields */
    civ = cvdev_in(nabm + 0x14, 1) & 31u;
    status = cvdev_in(nabm + 0x16, 2);
    halted = status & 1u;
    I->stats.reserved[0] = civ | (cvdev_in(nabm + 0x15, 1) << 8) | (cvdev_in(nabm + 0x1b, 1) << 16);
    I->stats.reserved[1] = status;
    I->stats.reserved[2] = (next_fill - civ) & 31u;
    if (halted) {
        ++I->stats.underruns;
        next_fill = (civ + 1u) & 31u;
    }
    queued = (next_fill - civ) & 31u;
    while (queued < AUDIO_LEAD) {
        render_buffer(next_fill);
        cvdev_out(nabm + 0x15, next_fill, 1);
        next_fill = (next_fill + 1u) & 31u;
        ++queued;
    }
    cvdev_out(nabm + 0x16, 0x1c, 2);                /* clear LVBCI/BCIS/FIFOE */
    if (halted) cvdev_out(nabm + 0x1b, 1, 1);
    I = current;
}

static uint32_t audio_start(const cvdev_pages *pages)
{
    uint32_t i;
    if (!ac97_find()) return CVDEV_AUDIO_NO_DEVICE;
    saved_master = (uint16_t)cvdev_in(nam + 0x02, 2);
    saved_pcm = (uint16_t)cvdev_in(nam + 0x18, 2);
    saved_ext = (uint16_t)cvdev_in(nam + 0x2a, 2);
    saved_rate = (uint16_t)cvdev_in(nam + 0x2c, 2);
    saved_bdbar = cvdev_in(nabm + 0x10, 4);
    saved_lvi = (uint8_t)cvdev_in(nabm + 0x15, 1);
    ac97_reset_output();
    audio_rate = 48000u;
    if (cvdev_in(nam + 0x28, 2) & 1u) {               /* variable rate audio */
        cvdev_out(nam + 0x2a, saved_ext | 1u, 2);
        cvdev_out(nam + 0x2c, AUDIO_RATE, 2);
        audio_rate = cvdev_in(nam + 0x2c, 2);
        if (audio_rate < 8000u || audio_rate > 48000u) audio_rate = 48000u;
    }
    I->stats.rate = audio_rate;
    cvdev_out(nam + 0x02, 0x0000, 2);
    cvdev_out(nam + 0x18, 0x0808, 2);
    bdl = (uint32_t *)pages->linear[0];
    for (i = 0; i != CVDEV_AUDIO_BUFFERS; ++i) {
        buffer_linear[i] = pages->linear[i + 1];
        bdl[i * 2] = pages->physical[i + 1];
        bdl[i * 2 + 1] = CVDEV_AUDIO_FRAMES * 2u;   /* samples, no IOC */
    }
    cvdev_out(nabm + 0x10, pages->physical[0], 4);
    next_fill = 0;
    audio_running = 1;
    for (i = 0; i != AUDIO_LEAD; ++i) {
        render_buffer(next_fill);
        cvdev_out(nabm + 0x15, next_fill, 1);
        next_fill = (next_fill + 1u) & 31u;
    }
    cvdev_out(nabm + 0x1b, 1, 1);                    /* run bus master */
    return CVDEV_AUDIO_RUNNING;
}

static void audio_stop(void)
{
    if (!audio_running) return;
    audio_running = 0;
    ac97_reset_output();
    cvdev_out(nabm + 0x10, saved_bdbar, 4);
    cvdev_out(nabm + 0x15, saved_lvi, 1);
    cvdev_out(nam + 0x2a, saved_ext, 2);
    cvdev_out(nam + 0x2c, saved_rate, 2);
    cvdev_out(nam + 0x18, saved_pcm, 2);
    cvdev_out(nam + 0x02, saved_master, 2);
}

/* ---- Ports ---- */

static void add_port(unsigned *count, uint32_t port)
{
    if (*count < sizeof(I->trap_ports) / sizeof(I->trap_ports[0]) - 1u)
        I->trap_ports[(*count)++] = (uint16_t)port;
}

static void build_trap_list(void)
{
    unsigned count = 0;
    uint32_t port;
    if (I->caps & (CVGP_CAP_KEYBOARD | CVGP_CAP_MOUSE)) {
        add_port(&count, 0x60);
        add_port(&count, 0x64);
    }
    if (I->caps & CVGP_CAP_DMA_SB) {
        for (port = 0x00; port <= 0x0f; ++port) add_port(&count, port);
        add_port(&count, 0x81); add_port(&count, 0x82); add_port(&count, 0x83);
        add_port(&count, 0x87); add_port(&count, 0x89); add_port(&count, 0x8a);
        add_port(&count, 0x8b); add_port(&count, 0x8f);
        for (port = 0xc0; port <= 0xde; port += 2) add_port(&count, port);
        for (port = 0x220; port <= 0x22f; ++port) add_port(&count, port);
    } else if (I->caps & CVGP_CAP_OPL3) {
        add_port(&count, 0x220); add_port(&count, 0x221);
        add_port(&count, 0x228); add_port(&count, 0x229);
    }
    if (I->caps & CVGP_CAP_OPL3)
        for (port = 0x388; port <= 0x38b; ++port) add_port(&count, port);
    I->trap_ports[count] = 0xffff;
}

const uint16_t *cvdev_trap_ports(void)
{
    return I->trap_ports;
}

int cvdev_port_claimed(uint32_t port)
{
    const uint16_t *p;
    if (!I->active) return 0;
    for (p = I->trap_ports; *p != 0xffff; ++p)
        if (*p == port) return 1;
    return 0;
}

/* ISA DMA channel 2 belongs to the floppy controller the host BIOS drives;
 * it stays physical while the other channels are the guest's virtual DMA. */
static int dma_physical_only(uint32_t port)
{
    return port == 0x04 || port == 0x05 || port == 0x81;
}

static uint8_t read_byte(uint32_t port)
{
    uint32_t value = 0xff;
    ++I->stats.port_reads;
    /* The controller is trapped for another VM's session: without keyboard
     * and mouse this session reads the real one. */
    if ((port == KBC_DATA || port == KBC_STATUS) &&
        !(I->caps & (CVGP_CAP_KEYBOARD | CVGP_CAP_MOUSE)))
        return (uint8_t)cvdev_in(port, 1);
    if (port == KBC_DATA || port == KBC_STATUS) {
        if (port == KBC_DATA && I->a20_output_pending) {
            I->a20_output_pending = 0;
            return 0xdf;                   /* output port: A20 enabled */
        }
        if (!I->devices.kbc_count && pull_physical()) ++I->stats.lazy_pulls;
        cvgp_io_read(&I->devices, I->generation, (uint16_t)port, 1, 0, &value);
        if (port == KBC_STATUS && I->a20_output_pending) value |= 1;
        pump();
        return (uint8_t)value;
    }
    if (dma_physical_only(port)) return (uint8_t)cvdev_in(port, 1);
    if (cvgp_io_read(&I->devices, I->generation, (uint16_t)port, 1, 0, &value) != CVGP_IO_OK) {
        ++I->stats.unclaimed_io;
        value = cvdev_in(port, 1);
    } else if (port == 0x08) {
        value |= cvdev_in(0x08, 1) & 0x44;  /* channel 2 TC/request bits */
    }
    pump();
    return (uint8_t)value;
}

static void write_byte(uint32_t port, uint8_t value)
{
    ++I->stats.port_writes;
    if ((port == KBC_DATA || port == KBC_STATUS) &&
        !(I->caps & (CVGP_CAP_KEYBOARD | CVGP_CAP_MOUSE))) {
        cvdev_out(port, value, 1);
        return;
    }
    if (port == KBC_STATUS) {
        /* A20 through the controller output port stays with Jemm: the gate
         * is reported enabled and never toggled by a guest. */
        if (value == 0xd1) { I->a20_data_pending = 1; return; }
        if (value == 0xd0) { I->a20_output_pending = 1; return; }
        if (value == 0xdd || value == 0xdf || value == 0xff) return;
    } else if (port == KBC_DATA && I->a20_data_pending) {
        I->a20_data_pending = 0;
        return;
    }
    if (dma_physical_only(port)) { cvdev_out(port, value, 1); return; }
    if ((port == 0x0a || port == 0x0b) && (value & 3) == 2) {
        cvdev_out(port, value, 1);
        return;
    }
    if (port == I->devices.sb.base + 0x0c && !I->devices.sb.argument_need)
        ++I->stats.dsp_commands;
    if (cvgp_io_write(&I->devices, I->generation, (uint16_t)port, 1, 0, value) != CVGP_IO_OK) {
        ++I->stats.unclaimed_io;
        cvdev_out(port, value, 1);
    }
    pump();
}

uint32_t cvdev_port_read(uint32_t port, uint32_t width)
{
    uint32_t result = 0, i;
    if (!I->active) return 0xffffffffUL;
    if (width != 2 && width != 4) width = 1;
    for (i = 0; i != width; ++i)
        result |= (uint32_t)read_byte(port + i) << (i * 8u);
    cvdev_poll();
    return result;
}

void cvdev_port_write(uint32_t port, uint32_t width, uint32_t value)
{
    uint32_t i;
    if (!I->active) return;
    if (width != 2 && width != 4) width = 1;
    for (i = 0; i != width; ++i)
        write_byte(port + i, (uint8_t)(value >> (i * 8u)));
    cvdev_poll();
}

/* ---- Lifecycle ---- */

int cvdev_begin(uint32_t owner, uint32_t requested, uint32_t flags,
                uint32_t tsc_khz, const cvdev_pages *pages,
                uint32_t *granted, uint32_t *audio)
{
    cvgp_backend backend;
    uint32_t available = CVGP_CAP_KEYBOARD | CVGP_CAP_MOUSE | CVGP_CAP_DMA_SB |
                         CVGP_CAP_OPL3;
    *granted = 0;
    *audio = CVDEV_AUDIO_NONE;
    if (I->active) return CVDEV_ERR_ACTIVE;
    I->pull_keyboard = I->pull_mouse = 1;
    I->hotkey_enabled = I->ctrl_down = I->esc_break_pending = cvdev_release_request = 0;
    if (!owner || !requested || (requested & ~available) || tsc_khz < 1000u)
        return CVDEV_ERR_ARGUMENT;
    zero(&I->stats, sizeof(I->stats));
    I->stats.magic = 0x56445643UL;
    I->stats.version = 0x0100;
    I->stats.bytes = (uint16_t)sizeof(I->stats);
    zero(&backend, sizeof(backend));
    backend.memory_read8 = memory_read8;
    backend.opl_write = opl_write;
    backend.opl_generate = opl_generate;
    backend.opl_reset = opl_reset;
    backend.opaque = I;
    I->opl_chip = 0;
    cvgp_init(&I->devices);
    if (cvgp_begin(&I->devices, owner, requested, available, &backend) != CVGP_OK)
        return CVDEV_ERR_MODEL;
    I->devices.pic[0].auto_eoi = I->devices.pic[1].auto_eoi = 1;
    /* The guest's timer is the real PIT behind Jemm's profile: the model's
     * own channel 0 must not add a second, virtual IRQ0. */
    I->devices.pit[0].running = 0;
    I->generation = owner;
    I->caps = requested;
    I->focused = 0;
    I->a20_data_pending = I->a20_output_pending = 0;
    I->tsc_per_us = tsc_khz / 1000u;
    cvvid_rdtsc(&I->last_low, &I->last_high);
    build_trap_list();
    I->active = 1;
    /* One AC'97 stream for all sessions: the newest one that wants it plays
     * (CVSESSION moves it with the focus). */
    I->wants_audio = (flags & CVDEV_FLAG_AUDIO_OUT) &&
                     (I->caps & (CVGP_CAP_DMA_SB | CVGP_CAP_OPL3)) && pages;
    if (I->wants_audio) {
        A = I;
        *audio = audio_running ? CVDEV_AUDIO_RUNNING : audio_start(pages);
        if (*audio != CVDEV_AUDIO_RUNNING) {
            A = 0;
            I->wants_audio = 0;
        }
    }
    if (I->wants_audio) player_set(I, 1);
    I->stats.audio = *audio;
    I->stats.ac97_nam = nam;
    I->stats.ac97_nabm = nabm;
    *granted = I->caps;
    return CVDEV_OK;
}

int cvdev_end(void)
{
    if (!I->active) return CVDEV_ERR_INACTIVE;
    if (A == I) A = 0;                          /* CVSESSION stops the stream */
    player_set(I, 0);
    I->wants_audio = 0;
    cvgp_end(&I->devices, I->generation);
    I->active = 0;
    I->caps = 0;
    I->focused = 0;
    I->opl_chip = 0;
    I->trap_ports[0] = 0xffff;
    return CVDEV_OK;
}

int cvdev_active(void)
{
    return (int)I->active;
}

void cvdev_set_pull(uint32_t keyboard, uint32_t mouse)
{
    I->pull_keyboard = keyboard ? 1u : 0u;
    I->pull_mouse = mouse ? 1u : 0u;
}

void cvdev_set_hotkey(uint32_t enabled)
{
    I->hotkey_enabled = enabled ? 1u : 0u;
    I->ctrl_down = I->esc_break_pending = 0;
    cvdev_release_request = 0;
}

int cvdev_focus(uint32_t value)
{
    if (!I->active) return CVDEV_ERR_INACTIVE;
    I->focused = value ? 1u : 0u;
    cvgp_set_focus(&I->devices, I->generation, (int)I->focused);
    pump();
    return CVDEV_OK;
}

int cvdev_key(uint32_t scan, uint32_t flags)
{
    if (!I->active) return CVDEV_ERR_INACTIVE;
    if (!scan || scan > 0x7f) return CVDEV_ERR_ARGUMENT;
    cvgp_key_event(&I->devices, I->generation, (uint8_t)scan, (flags & 2) != 0, (flags & 1) != 0);
    pump();
    return CVDEV_OK;
}

int cvdev_mouse(uint32_t dx, uint32_t dy, uint32_t buttons)
{
    if (!I->active) return CVDEV_ERR_INACTIVE;
    if (!(I->caps & CVGP_CAP_MOUSE)) return CVDEV_ERR_ARGUMENT;
    cvgp_mouse_event(&I->devices, I->generation, (int)(int16_t)dx, (int)(int16_t)dy,
                     (uint8_t)(buttons & 7));
    pump();
    return CVDEV_OK;
}

/* A session that wants audio but does not own the stream plays muted: its
 * SB16 DMA and OPL go on in real time (their IRQs keep coming, a DOS program
 * in the background never waits on its sound) into a discarded buffer, for
 * the time its devices advanced (US, from advance()). */
static void play_muted(uint32_t us)
{
    uint32_t ms, frames, n;
    I->muted_us += us;
    ms = I->muted_us / 1000u;
    I->muted_us -= ms * 1000u;
    frames = ms * (AUDIO_RATE / 100u) + I->muted_frac;      /* tenths */
    I->muted_frac = frames % 10u;
    frames /= 10u;
    while (frames) {
        n = frames < CVDEV_AUDIO_FRAMES ? frames : CVDEV_AUDIO_FRAMES;
        cvgp_render_audio(&I->devices, I->generation, discard, n, AUDIO_RATE);
        frames -= n;
    }
}

void cvdev_poll(void)
{
    uint32_t us;
    if (!I->active) return;
    us = advance();
    if (!us) return;
    ++I->stats.polls;
    if (I->wants_audio && I != A && audio_running) play_muted(us);
    I->audio_elapsed_us += us;
    if (I->audio_elapsed_us >= 4000u) {
        I->audio_elapsed_us = 0;
        /* An IRQ taken in another context (the HDPMI client) leaves its byte
         * in the controller; collect it so the buffer never stays full. */
        if ((I->caps & (CVGP_CAP_KEYBOARD | CVGP_CAP_MOUSE)) && !I->devices.kbc_count &&
            pull_physical())
            ++I->stats.lazy_pulls;
    }
    pump();
}

/* The one AC'97 stream, whichever VM runs: the owner session's devices are
 * advanced (their IRQs pend in the owner's VM) and, every 4 ms at most, the
 * stream is serviced; the other sessions that play go on muted (the running
 * one's in cvdev_poll). AC'97 register accesses leave the virtual machine;
 * eight queued buffers hold 186 ms of audio. */
void cvdev_audio_poll(void)
{
    static uint32_t last_lo, last_hi;
    uint32_t lo, hi, us;
    struct cvdev_instance *current = I;
    unsigned i;
    if (!audio_running) return;
    if (A && A != I && A->active) {
        I = A;
        if (advance()) pump();
        I = current;
    }
    for (i = 0; i != CVDEV_PLAYERS; ++i) {       /* the muted ones, too */
        if (!players[i] || players[i] == A || players[i] == current || !players[i]->active)
            continue;
        I = players[i];
        us = advance();
        if (us) {
            play_muted(us);
            pump();
        }
        I = current;
    }
    cvvid_rdtsc(&lo, &hi);
    us = A && A->tsc_per_us ? (lo - last_lo) / A->tsc_per_us : 4000u;
    if (hi - last_hi > 1 || us >= 4000u) {
        last_lo = lo;
        last_hi = hi;
        audio_service();
    }
}

uint32_t cvdev_audio_running(void)
{
    return audio_running;
}

/* The session that plays through the AC'97: an instance that wants audio,
 * or 0 for silence. */
void cvdev_audio_owner(void *instance)
{
    struct cvdev_instance *wanted = (struct cvdev_instance *)instance;
    A = wanted && wanted->active && wanted->wants_audio ? wanted : 0;
}

void cvdev_audio_stop(void)
{
    A = 0;
    audio_stop();
}

uint32_t cvdev_instance_bytes(void)
{
    return (uint32_t)sizeof(struct cvdev_instance);
}

/* The session to act for (an instance block of cvdev_instance_bytes(),
 * zero-filled before its first DEV_BEGIN) and its VM; 0 for none. */
void cvdev_select(void *instance, uint32_t vm)
{
    I = instance ? (struct cvdev_instance *)instance : &instance_none;
    if (instance) I->vm = vm;
}

void *cvdev_selected(void)
{
    return I == &instance_none ? 0 : I;
}

uint32_t cvdev_wants_audio(void)
{
    return I->active && I->wants_audio;
}

void cvdev_note_bridge(void)
{
    ++I->stats.bridge_calls;
}

void cvdev_report_state(cvdev_report *out)
{
    I->stats.magic = 0x56445643UL;                      /* bytes CVDV */
    I->stats.version = 0x0100;
    I->stats.bytes = (uint16_t)sizeof(I->stats);
    I->stats.active = I->active;
    I->stats.caps = I->caps;
    I->stats.focused = I->focused;
    /* Protected-mode delivery: delivered | held lines << 16 | claimed << 31. */
    I->stats.reserved[3] = cvdev_pm_state();
    *out = I->stats;
}
