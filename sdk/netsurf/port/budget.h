/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_BUDGET_H
#define CIUKI_NETSURF_BUDGET_H
#include <stdbool.h>
#include <stdint.h>
/* f6-20: the 75 ms layout time limit guards the UI against remote content
 * only. Local content (about:, resource: and file:) keeps the structural
 * admission limits but is never rejected by time. A span nested inside a
 * timed layout (an HTML object) inherits the outer deadline and limit.
 *
 * Lead decision 2026-10-23 (fx-11, revising f6-14's provisional 75 ms): the
 * document of a top-level browser window gets CIUKI_LAYOUT_DOCUMENT_LIMIT_NS,
 * 10 s. The T23 (trial 423b803a) spent 58 ms on the welcome page's layout work
 * alone; ordinary documents (5-13 ms of host layout) take 0.6-1.7 s there.
 * Frames, iframes, embedded objects and every other span keep the 75 ms
 * CIUKI_LAYOUT_LIMIT_NS; a page over its limit still gets the error page.
 * NetSurf is its own process: a long layout freezes only its window. During
 * the layout the browser neither repaints nor handles input; the desktop
 * keeps showing the last presented frame, can still move the window, and
 * marks it "Not responding" after an unanswered 5 s PING (apps/desktop/
 * client.c). The keepalive pump (ciuki_ui_keepalive, at most every 20 ms)
 * keeps reading the channel so the desktop never drops the client; input
 * beyond 64 held packets is dropped and counted (surface.c). */
#define CIUKI_LAYOUT_LIMIT_NS 75000000u
#define CIUKI_LAYOUT_DOCUMENT_LIMIT_NS 10000000000ull
enum ciuki_layout_cause { CIUKI_LAYOUT_ACCEPTED, CIUKI_LAYOUT_ADMISSION, CIUKI_LAYOUT_TIME };
struct ciuki_layout_span {
    uint64_t start, outer_deadline, limit, outer_limit;
    bool timed;
};
/* Set by browser_window.c around a top-level window's reformat (the first
 * layout runs before content_open gives the content its window). */
extern bool ciuki_layout_top_level;
extern uint64_t ciuki_layout_max_ns;        /* every layout, budget clock */
extern uint64_t ciuki_layout_local_max_ns;  /* untimed (local) layouts */
extern uint64_t ciuki_layout_remote_max_ns; /* timed (remote) layouts */
extern unsigned ciuki_layout_rejections;
/* Budget clock multiplier: 1 on CiukiOS. Host tests set the T23 model
 * (--ciuki-clock-scale=N) so host layouts are charged as a 1.1 GHz P3. */
extern uint32_t ciuki_layout_clock_scale;
bool ciuki_layout_local_url(const char *url);
/* A timed span gets limit_ns unless it nests in a timed span (then the
 * outer deadline and limit); ciuki_layout_begin uses CIUKI_LAYOUT_LIMIT_NS. */
struct ciuki_layout_span ciuki_layout_begin_limit(bool timed,uint64_t limit_ns);
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
