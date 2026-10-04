/* CiukiOS monitored-session guest devices inside CVSESSION (ring 0).
 *
 * Binds the peripheral model (guest_peripherals.c) to real guest cycles:
 * V86 port traps installed through Jemm, protected-mode port I/O bridged by
 * the HDPMI 3.24 adapter, physical 8042 bytes captured from the negotiated
 * Jemm interrupt profile, virtual IRQs raised into that profile's virtual
 * PIC, and PCM streamed to the ICH AC'97 PCM-out engine for the session.
 * Every function runs in Jemm's context with flat selectors, IF=0 and the
 * private device stack; none calls DOS, BIOS or firmware.
 */
#ifndef CIUKIOS_SESSION_DEVICES_H
#define CIUKIOS_SESSION_DEVICES_H

#include <stdint.h>

#if defined(__WATCOMC__)
#define CVDEV_CALL __cdecl
#else
#define CVDEV_CALL
#endif

#define CVDEV_AUDIO_BUFFERS      32u
#define CVDEV_AUDIO_FRAMES       1024u   /* per 4 KiB page, 16-bit stereo */
#define CVDEV_AUDIO_PAGES        (CVDEV_AUDIO_BUFFERS + 1u)
#define CVDEV_STATE_BYTES        128u

/* DEV_BEGIN flags */
#define CVDEV_FLAG_AUDIO_OUT     0x0001u

/* DEV_BEGIN audio result */
#define CVDEV_AUDIO_NONE         0u
#define CVDEV_AUDIO_RUNNING      1u
#define CVDEV_AUDIO_NO_DEVICE    2u
#define CVDEV_AUDIO_NO_RATE      3u

/* Results (mirrored as VM_ERROR_* by the glue) */
#define CVDEV_OK                 0
#define CVDEV_ERR_ACTIVE         1
#define CVDEV_ERR_INACTIVE       2
#define CVDEV_ERR_MODEL          3
#define CVDEV_ERR_ARGUMENT       4

typedef struct cvdev_pages {
    uint32_t linear[CVDEV_AUDIO_PAGES];
    uint32_t physical[CVDEV_AUDIO_PAGES];
} cvdev_pages;

/* 128-byte DEV_STATE packet (all little-endian dwords). */
typedef struct cvdev_report {
    uint32_t magic;                 /* 'CVDV' */
    uint16_t version, bytes;
    uint32_t active, caps, focused, audio;
    uint32_t keys_forwarded, keys_dropped, aux_bytes, lazy_pulls;
    uint32_t irqs_raised, irq_failures, port_reads, port_writes;
    uint32_t unclaimed_io, model_errors, last_error, last_port;
    uint32_t buffers_rendered, underruns, polls, sb_blocks;
    uint32_t opl_writes, dsp_commands, ac97_nam, ac97_nabm;
    uint32_t bridge_calls, rate, reserved[4];
} cvdev_report;

int      CVDEV_CALL cvdev_begin(uint32_t generation, uint32_t caps, uint32_t flags,
                                uint32_t tsc_khz, const cvdev_pages *pages,
                                uint32_t *granted, uint32_t *audio);
int      CVDEV_CALL cvdev_end(void);
int      CVDEV_CALL cvdev_active(void);
int      CVDEV_CALL cvdev_focus(uint32_t focused);
/* Which physical 8042 bytes the model may take (both by default). With
 * several VMs the mouse stays with the desktop's VM, and the keyboard is
 * taken only while the session's VM has the focus. */
void     CVDEV_CALL cvdev_set_pull(uint32_t keyboard, uint32_t mouse);
/* Ctrl+Esc taken from the guest and reported in cvdev_release_request (set to
 * 1; the caller clears it and gives the focus back). Off by default. */
void     CVDEV_CALL cvdev_set_hotkey(uint32_t enabled);
/* Several VMs can each own a device model: CVSESSION selects the instance
 * block (cvdev_instance_bytes(), zero-filled before first use) and VM it acts
 * for; 0 selects none. */
uint32_t CVDEV_CALL cvdev_instance_bytes(void);
void     CVDEV_CALL cvdev_select(void *instance, uint32_t vm);
void    *CVDEV_CALL cvdev_selected(void);
/* The one AC'97 stream: serviced from any VM (the owner session's devices
 * advance with it), played from the owner (0: silence), stopped by
 * CVSESSION when no session wants it. */
void     CVDEV_CALL cvdev_audio_poll(void);
uint32_t CVDEV_CALL cvdev_audio_running(void);
void     CVDEV_CALL cvdev_audio_owner(void *instance);
void     CVDEV_CALL cvdev_audio_stop(void);
uint32_t CVDEV_CALL cvdev_wants_audio(void);
extern uint32_t cvdev_release_request;
int      CVDEV_CALL cvdev_key(uint32_t scan, uint32_t flags);
/* Relative PS/2 movement (signed 16-bit) and buttons from the host. */
int      CVDEV_CALL cvdev_mouse(uint32_t dx, uint32_t dy, uint32_t buttons,
                               uint32_t wheel);
/* Port handlers. Return 1 when the port belongs to this session. */
int      CVDEV_CALL cvdev_port_claimed(uint32_t port);
uint32_t CVDEV_CALL cvdev_port_read(uint32_t port, uint32_t width);
void     CVDEV_CALL cvdev_port_write(uint32_t port, uint32_t width, uint32_t value);
/* Physical IRQ1/IRQ12 filter from the Jemm profile: 1 = consumed. */
int      CVDEV_CALL cvdev_irq_filter(uint32_t irq);
/* Host-time service: advance devices, stream audio, raise virtual IRQs. */
void     CVDEV_CALL cvdev_poll(void);
void     CVDEV_CALL cvdev_report_state(cvdev_report *out);
void     CVDEV_CALL cvdev_note_bridge(void);
/* Ports trapped for this session, terminated by 0xFFFF. */
const uint16_t *CVDEV_CALL cvdev_trap_ports(void);

#endif
