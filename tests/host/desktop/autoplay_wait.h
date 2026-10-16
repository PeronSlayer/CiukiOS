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
        if (phase==AP_IDLE || phase==AP_READY) return;
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    assert(!"AutoPlay worker did not complete");
}
#endif
