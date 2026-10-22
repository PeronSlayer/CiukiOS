/* SPDX-License-Identifier: MIT
 * One persistent preview worker, woken by a duplex SDK channel. */
#ifndef CIUKI_CONTROL_DECODER_H
#define CIUKI_CONTROL_DECODER_H
#include "control.h"
typedef int (*cp_decoder_work)(const struct cp_preview_req *, struct cp_preview_done *);
int cp_decoder_work_default(const struct cp_preview_req *, struct cp_preview_done *);
void cp_decoder_set_work(cp_decoder_work); /* tests, only while stopped */
int cp_decoder_begin(const struct cp_preview_req *);
int cp_decoder_fd(void); /* completion endpoint for the UI's poll set, -1 before first job */
int cp_decoder_poll(struct cp_preview_done *); /* take ONCE after readiness, never on a timer */
void cp_decoder_cancel(void); /* cooperative; never waits */
int cp_decoder_cancelled(void);
void cp_decoder_stop(void); /* join at process shutdown before freeing portrait/UI assets */
#define CP_DECODER_UI_SLICE_NS 10000000ull
#endif
