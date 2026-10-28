/* SPDX-License-Identifier: MIT
 * Host-only scheduling of the production AutoPlay state machine. */
#ifndef DESK_AUTOPLAY_WAIT_H
#define DESK_AUTOPLAY_WAIT_H
#include <assert.h>
#include <time.h>
static void autoplay_drain(struct desktop *d)
{
    for (unsigned i=0;i<3000;i++) {
        desk_autoplay_service(d);
        (void)desk_autoplay_launch(d);
        int phase=d->sh.autoplay.phase;
        if (phase==AP_IDLE || phase==AP_READY || phase==AP_LAUNCHING) return; /* LAUNCHING: the launch worker's turn */
        struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    assert(!"AutoPlay worker did not complete");
}
#endif
