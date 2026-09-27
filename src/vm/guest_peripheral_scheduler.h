/* Peripheral-side proposal for the monitored-session scheduler track.
 *
 * This is a protected-host interface containing trusted pointers.  It is not
 * a DOS/Jemm/HDPMI wire ABI and must never be populated from guest addresses.
 * The scheduler implementation is owned by its track; this header only states
 * what the peripheral layer requires at one bounded service point.
 */
#ifndef CIUKIOS_GUEST_PERIPHERAL_SCHEDULER_H
#define CIUKIOS_GUEST_PERIPHERAL_SCHEDULER_H

#include "guest_peripherals.h"

#define CVGP_SCHED_MAGIC   0x43565053UL /* bytes SPVC */
#define CVGP_SCHED_VERSION 0x0100u
#define CVGP_SCHED_EVENTS_PER_SERVICE 128u

enum cvgp_host_event_type {
    CVGP_HOST_EVENT_NONE = 0,
    CVGP_HOST_EVENT_FOCUS = 1,
    CVGP_HOST_EVENT_KEY = 2,
    CVGP_HOST_EVENT_MOUSE = 3
};

typedef struct cvgp_host_event {
    uint8_t type, pressed, extended, buttons;
    uint8_t scan, reserved[3];
    int16_t mouse_dx, mouse_dy;
} cvgp_host_event;

/* poll_event returns 1 for an event, 0 for an empty queue, or -1 on failure.
 * deliver_irq is called only after cvgp_irq_acknowledge has entered virtual ISR
 * state, so failure is fatal to the session rather than permission to retry.
 * submit_pcm must copy or consume frames before returning and must not block.
 */
typedef int (CVGP_CALL *cvgp_poll_event_fn)(void *opaque, cvgp_host_event *event);
typedef int (CVGP_CALL *cvgp_deliver_irq_fn)(void *opaque, uint8_t vector);
typedef int (CVGP_CALL *cvgp_submit_pcm_fn)(void *opaque, const int16_t *stereo,
                                         unsigned frames, unsigned rate);
typedef void (CVGP_CALL *cvgp_report_error_fn)(void *opaque, unsigned error,
                                             uint16_t port,
                                             const char *english_message);

typedef struct cvgp_scheduler_binding {
    uint32_t magic;
    uint16_t version, bytes;
    uint32_t generation;
    cvgp_state *devices;
    void *opaque;
    cvgp_poll_event_fn poll_event;
    cvgp_deliver_irq_fn deliver_irq;
    cvgp_submit_pcm_fn submit_pcm;
    cvgp_report_error_fn report_error;
    int16_t *audio_scratch;           /* interleaved stereo, protected-owned */
    unsigned audio_capacity_frames;
    unsigned audio_rate;              /* CVGP_AUDIO_RATE when OPL is enabled */
} cvgp_scheduler_binding;

/* Required service ordering, implemented by the scheduler track:
 *  1. Poll at most CVGP_SCHED_EVENTS_PER_SERVICE and inject host events.
 *  2. Call cvgp_advance with monotonic elapsed PIT clocks even if guest_if=0.
 *  3. Acknowledge/deliver at most a bounded number of virtual IRQs if IF=1.
 *  4. Render and submit a bounded PCM slice from protected-owned storage.
 *  5. Publish request/service generations and any fatal diagnostic.
 * No step may call DOS, BIOS, firmware, allocate, or touch physical guest I/O.
 */

#endif
