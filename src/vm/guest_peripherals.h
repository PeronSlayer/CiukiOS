/* CiukiOS per-session legacy input and sound devices.
 *
 * This is a deterministic device model.  It never performs physical I/O and
 * it does not install Jemm or HDPMI traps.  The session owner must route every
 * claimed V86 and protected-mode access here and must service the model from a
 * host scheduling boundary which is independent of the guest's virtual IF.
 */
#ifndef CIUKIOS_GUEST_PERIPHERALS_H
#define CIUKIOS_GUEST_PERIPHERALS_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(__WATCOMC__)
#define CVGP_CALL __cdecl
#else
#define CVGP_CALL
#endif

#define CVGP_ABI_VERSION       0x0100u
#define CVGP_PIT_HZ            1193182UL
#define CVGP_AUDIO_RATE        44100u
#define CVGP_DEFAULT_SB_BASE   0x0220u
#define CVGP_DEFAULT_SB_IRQ    7u
#define CVGP_DEFAULT_SB_DMA8   1u
#define CVGP_DEFAULT_SB_DMA16  5u
#define CVGP_KBC_QUEUE_BYTES   1152u

enum cvgp_capability {
    CVGP_CAP_KEYBOARD = 0x0001u,
    CVGP_CAP_MOUSE    = 0x0002u,
    CVGP_CAP_PIC_PIT  = 0x0004u,
    CVGP_CAP_DMA_SB   = 0x0008u,
    CVGP_CAP_OPL3     = 0x0010u
};

enum cvgp_result {
    CVGP_OK = 0,
    CVGP_ERR_ARGUMENT = 1,
    CVGP_ERR_ACTIVE = 2,
    CVGP_ERR_OWNER = 3,
    CVGP_ERR_MISSING_DEVICE = 4,
    CVGP_ERR_BACKEND = 5,
    CVGP_ERR_QUEUE_FULL = 6,
    CVGP_ERR_IO_WIDTH = 7,
    CVGP_ERR_STRING_IO = 8,
    CVGP_ERR_UNSUPPORTED_PORT = 9,
    CVGP_ERR_UNSUPPORTED_PIT = 10,
    CVGP_ERR_UNSUPPORTED_DMA = 11,
    CVGP_ERR_UNSUPPORTED_SB = 12,
    CVGP_ERR_MEMORY = 13
};

enum cvgp_io_result {
    CVGP_IO_OK = 0,
    CVGP_IO_NOT_CLAIMED = 1,
    CVGP_IO_REJECTED = 2
};

enum cvgp_service_flags {
    CVGP_SERVICE_NONE = 0,
    CVGP_SERVICE_IRQ = 1,
    CVGP_SERVICE_AUDIO = 2,
    CVGP_SERVICE_DIAGNOSTIC = 4
};

typedef uint8_t (CVGP_CALL *cvgp_memory_read8_fn)(void *opaque,
                                                uint32_t physical,
                                                int *valid);
typedef void (CVGP_CALL *cvgp_opl_write_fn)(void *opaque, uint16_t reg,
                                          uint8_t value);
/* Generate exactly frames of signed 16-bit interleaved stereo PCM. */
typedef int (CVGP_CALL *cvgp_opl_generate_fn)(void *opaque, int16_t *stereo,
                                            unsigned frames);
typedef void (CVGP_CALL *cvgp_opl_reset_fn)(void *opaque, unsigned sample_rate);

typedef struct cvgp_backend {
    void *opaque;
    cvgp_memory_read8_fn memory_read8;
    cvgp_opl_write_fn opl_write;
    cvgp_opl_generate_fn opl_generate;
    cvgp_opl_reset_fn opl_reset;
} cvgp_backend;

typedef struct cvgp_pic_unit {
    uint8_t irr, isr, imr, vector_base;
    uint8_t init_step, expect_icw4, single, auto_eoi, read_isr;
} cvgp_pic_unit;

typedef struct cvgp_pit_channel {
    uint32_t reload, remaining;
    uint16_t latch;
    uint8_t access, mode, write_phase, read_phase, latched, running;
    uint8_t latch_phase, write_latch, status, status_latched, null_count, bcd, mode2_low;
} cvgp_pit_channel;

typedef struct cvgp_dma_channel {
    uint16_t base_address, current_address;
    uint16_t base_count, current_count;
    uint8_t page, mode, masked, terminal;
} cvgp_dma_channel;

typedef struct cvgp_sb_state {
    uint16_t base;
    uint8_t irq, dma8, dma16;
    uint8_t mixer_index, mixer[256];
    uint8_t reset_latch, speaker, paused8, paused16;
    uint8_t command, argument_count, argument_need, arguments[4];
    uint8_t response[64], response_read, response_write, response_count;
    uint8_t test_register, sample_bits, sample_signed, stereo;
    uint8_t auto_init, exit_auto, active, silent;
    uint8_t irq_status;             /* mixer 82h: bit0 8-bit, bit1 16-bit DMA IRQ */
    uint32_t sample_rate, block_units, units_left;
    uint32_t resample_phase;
    int16_t held_left, held_right;
} cvgp_sb_state;

typedef struct cvgp_opl_state {
    uint16_t index[2];
    uint8_t regs[512];
    uint8_t status, timer_control;
    uint32_t timer_accum[2];
} cvgp_opl_state;

typedef struct cvgp_state {
    uint32_t owner_generation;
    uint32_t capabilities;
    uint32_t available;
    uint32_t service_generation;
    uint32_t unsupported_count;
    uint16_t last_port;
    uint8_t last_error, active, focused;
    cvgp_backend backend;

    uint8_t key_down[64];             /* scan | (E0 ? 100h : 0), 512 bits */
    uint8_t kbc_data[CVGP_KBC_QUEUE_BYTES];
    uint8_t kbc_aux[CVGP_KBC_QUEUE_BYTES];
    uint16_t kbc_read, kbc_write, kbc_count;
    uint8_t kbc_command, kbc_pending_command;
    uint8_t keyboard_scanning, keyboard_enabled, mouse_enabled;
    uint8_t mouse_stream, mouse_scaling, mouse_resolution, mouse_rate;
    uint8_t mouse_buttons, mouse_pending_command;
    uint8_t mouse_id, mouse_rate_step;
    uint8_t raw_prefix, raw_skip;     /* Set-1 E0 prefix / E1 bytes to skip */

    cvgp_pic_unit pic[2];
    cvgp_pit_channel pit[3];
    /* Channel-0 expiries not yet handed to the VM's authoritative PIC. */
    uint32_t pit0_pending_irqs;
    cvgp_dma_channel dma[8];
    uint8_t dma_flipflop[2], dma_status[2];
    cvgp_sb_state sb;
    cvgp_opl_state opl;
} cvgp_state;

void CVGP_CALL cvgp_init(cvgp_state *s);
int CVGP_CALL cvgp_begin(cvgp_state *s, uint32_t generation,
                       uint32_t requested, uint32_t available,
                       const cvgp_backend *backend);
int CVGP_CALL cvgp_end(cvgp_state *s, uint32_t generation);
int CVGP_CALL cvgp_set_focus(cvgp_state *s, uint32_t generation, int focused);
/* Set-1 scan code without the E0 byte.  extended selects the E0 namespace. */
int CVGP_CALL cvgp_key_event(cvgp_state *s, uint32_t generation,
                           uint8_t scan, int extended, int pressed);
/* Physical Set-1 byte from the host keyboard, forwarded 1:1 while focused
 * (E0/E1 prefixes and controller responses included) and tracked in the
 * key-down bitmap so a focus loss can still release every held key.
 * Ignored, with the prefix state reset, while unfocused. */
int CVGP_CALL cvgp_key_raw(cvgp_state *s, uint32_t generation, uint8_t value);
/* Physical auxiliary (PS/2 mouse) byte for the host pointer driver. It is
 * queued unchanged regardless of focus and survives focus transitions. */
int CVGP_CALL cvgp_aux_raw(cvgp_state *s, uint32_t generation, uint8_t value);
/* Mouse deltas use PS/2 coordinates: right/up are positive. */
int CVGP_CALL cvgp_mouse_event(cvgp_state *s, uint32_t generation,
                             int dx, int dy, uint8_t buttons, int wheel);

/* Advance virtual hardware time in PIT input clocks.  The caller must invoke
 * this from a host-owned scheduling boundary even while guest IF is clear.
 */
unsigned CVGP_CALL cvgp_advance(cvgp_state *s, uint32_t generation,
                              uint32_t pit_clocks);
/* Channel-0 PIT expiries remain queued separately from the PIC's single IRR
 * bit. The bridge checks its authoritative guest PIC state, queues one IRQ0,
 * then consumes exactly one expiry. pending() returns CVGP_OK or an error;
 * consume() returns 1 when it consumes one expiry, 0 when empty, or a CVGP
 * error (notably CVGP_ERR_OWNER) when the generation is invalid.
 */
int CVGP_CALL cvgp_pit_irq0_pending(cvgp_state *s, uint32_t generation,
                                  uint32_t *count);
int CVGP_CALL cvgp_pit_irq0_consume(cvgp_state *s, uint32_t generation);
/* Return one deliverable interrupt and enter its virtual ISR state.  A false
 * return means no unmasked request or guest_if==0.  Host work must still run.
 */
int CVGP_CALL cvgp_irq_acknowledge(cvgp_state *s, uint32_t generation,
                                 int guest_if, uint8_t *vector);

int CVGP_CALL cvgp_io_read(cvgp_state *s, uint32_t generation, uint16_t port,
                         unsigned width, int string_io, uint32_t *value);
int CVGP_CALL cvgp_io_write(cvgp_state *s, uint32_t generation, uint16_t port,
                          unsigned width, int string_io, uint32_t value);

/* Mix virtual SB and OPL into caller-owned stereo output.  The output buffer
 * is always initialized.  No host audio device is touched by this function.
 */
int CVGP_CALL cvgp_render_audio(cvgp_state *s, uint32_t generation,
                              int16_t *stereo, unsigned frames,
                              unsigned output_rate);

int CVGP_CALL cvgp_port_claimed(const cvgp_state *s, uint16_t port);
const char *CVGP_CALL cvgp_error_message(unsigned error);

#if defined(__cplusplus)
}
#endif
#endif
