/* SPDX-License-Identifier: MIT
 * f2-88b: the Control Panel's wallpaper preview worker. One job at a time on its own thread; the UI thread
 * only starts it and polls it. The lock protects the job hand-over and is never held while a job runs. */
#ifndef CIUKI_CONTROL_DECODER_H
#define CIUKI_CONTROL_DECODER_H
#include "control.h"
typedef int (*cp_decoder_work)(const struct cp_preview_req *, struct cp_preview_done *);
/* Default work: read + decode (when req->image is empty) + render the 192x144 preview. */
int cp_decoder_work_default(const struct cp_preview_req *, struct cp_preview_done *);
void cp_decoder_set_work(cp_decoder_work);   /* tests */
int cp_decoder_begin(const struct cp_preview_req *);      /* 0, -EBUSY, or -errno; returns at once */
int cp_decoder_poll(struct cp_preview_done *);            /* 0 running/lock busy, 1 finished; returns at once */
void cp_decoder_test_lock(int hold);        /* tests: hold the hand-over lock as a publishing worker does */
/* Upper bound of UI-thread time spent per call (nanoseconds): begin and poll copy a request or a result. */
#define CP_DECODER_UI_SLICE_NS 10000000ull
#endif
