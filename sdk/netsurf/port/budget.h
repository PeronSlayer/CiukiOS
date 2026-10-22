/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_BUDGET_H
#define CIUKI_NETSURF_BUDGET_H
#include <stdbool.h>
#include <stdint.h>
/* f6-20: the 75 ms layout time limit guards the UI against remote content
 * only. Local content (about:, resource: and file:) keeps the structural
 * admission limits but is never rejected by time. A span nested inside a
 * timed layout (an HTML object) inherits the outer deadline. */
#define CIUKI_LAYOUT_LIMIT_NS 75000000u
enum ciuki_layout_cause { CIUKI_LAYOUT_ACCEPTED, CIUKI_LAYOUT_ADMISSION, CIUKI_LAYOUT_TIME };
struct ciuki_layout_span {
    uint64_t start, outer_deadline;
    bool timed;
};
extern uint64_t ciuki_layout_max_ns;        /* every layout, budget clock */
extern uint64_t ciuki_layout_local_max_ns;  /* untimed (local) layouts */
extern uint64_t ciuki_layout_remote_max_ns; /* timed (remote) layouts */
extern unsigned ciuki_layout_rejections;
/* Budget clock multiplier: 1 on CiukiOS. Host tests set the T23 model
 * (--ciuki-clock-scale=N) so host layouts are charged as a 1.1 GHz P3. */
extern uint32_t ciuki_layout_clock_scale;
bool ciuki_layout_local_url(const char *url);
struct ciuki_layout_span ciuki_layout_begin(bool timed);
bool ciuki_layout_expired(void);
bool ciuki_layout_end(struct ciuki_layout_span span, bool accepted);
/* f6-20 keepalive: the Ciuki surface installs a pump that reads (never
 * processes) desktop packets; long layouts (ciuki_layout_expired) and paints
 * call ciuki_ui_keepalive, which runs it at most every 20 ms. */
extern void (*ciuki_ui_pump)(void);
void ciuki_ui_keepalive(void);
/* Cause and duration of the most recent ciuki_layout_end. */
enum ciuki_layout_cause ciuki_layout_last_cause(void);
uint64_t ciuki_layout_last_ns(void);
#endif
