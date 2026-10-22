/* f0-55 boot splash progress (docs/boot.md, "Boot splash progress").
 * The quiet-boot splash shows a progress bar under the Ciuki photograph.
 * It is indeterminate until the first phase below is reported, then it
 * fills to the reported phase's share; it never advances from a timer.
 * Phases are listed in boot order; each maps to a fixed share in permille,
 * weighted by the 1.0.0 alpha T23 serial timeline (storage and audio
 * dominate). Progress never goes back, so a phase that a boot skips (safe
 * mode disables ATA and storage) only makes the next one a larger step.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SPLASH_H
#define CIUKI_SPLASH_H
#include <stdbool.h>
#include <stdint.h>

enum splash_phase {
    SPLASH_PHASE_DRIVERS,     /* [init] flag: driver activation starts */
    SPLASH_PHASE_FRAMEBUFFER, /* [init] framebuffer activation_seq=2 */
    SPLASH_PHASE_INPUT,       /* [init] input activation_seq=3 */
    SPLASH_PHASE_ATA,         /* [init] ata activation_seq=4 */
    SPLASH_PHASE_DISK,        /* [storage] kind=1: a disk was scanned */
    SPLASH_PHASE_VOLUME,      /* [storage] kind=2: a FAT volume was mounted */
    SPLASH_PHASE_IDENTITY,    /* [storage] kind=3: boot volume identity */
    SPLASH_PHASE_STORAGE,     /* [init] storage done */
    SPLASH_PHASE_FILES,       /* [init] files bootstrap done */
    SPLASH_PHASE_AUDIO,       /* [init] audio init done */
    SPLASH_PHASE_DEVICES,     /* network and USB started, drivers_init ends */
    SPLASH_PHASE_SUPERVISOR,  /* [supervisor] bootstrap: spawning the desktop */
    SPLASH_PHASE_DESKTOP,     /* the desktop image is loaded */
    SPLASH_PHASE_HANDOVER,    /* display grant: the desktop owns the screen */
    SPLASH_PHASES
};

#define SPLASH_PERMILLE_FULL 1000u

static inline uint32_t splash_phase_permille(enum splash_phase phase)
{
    static const uint16_t share[SPLASH_PHASES] = {
        40, 60, 80, 120, 250, 400, 520, 600, 660, 860, 900, 920, 970, SPLASH_PERMILLE_FULL,
    };
    return (unsigned)phase < SPLASH_PHASES ? share[phase] : 0;
}

/* Storage activation entries (init.h enum activation_kind: 1 storage,
 * 2 mount, 3 storage identity); device entries are reported explicitly. */
static inline int splash_storage_phase(unsigned kind)
{
    return kind == 1 ? SPLASH_PHASE_DISK : kind == 2 ? SPLASH_PHASE_VOLUME :
           kind == 3 ? SPLASH_PHASE_IDENTITY : -1;
}

/* Thread context with IF=1. Stores the phase and draws the bar at once when
 * the splash is on screen; a no-op on verbose boots and after the handover. */
void console_splash_phase(enum splash_phase phase);

/* f1-97: the 2D accelerator decision, drawn under the bar by the console
 * worker as `2D acceleration: YES` or `2D acceleration: NO (<reason>)`.
 * Thread context; a no-op when the splash is not on screen. */
void console_splash_accel(bool accelerated, const char *reason);

#endif
