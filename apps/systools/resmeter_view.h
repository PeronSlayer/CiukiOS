/* Resource Meter window: System / User / GDI free-resource bars coloured by level.
 * Windows 98's Resource Meter (Resource Kit app. D, "System Resource Meter") shows the three
 * free percentages; CiukiOS has no 16-bit heaps, so (documented in system-tools.md):
 *   User = free share of the desktop's client table (window count of a DESK_WINDOWS_QUERY, cap 16),
 *   GDI  = free share of physical pages (window surfaces are page-backed),
 *   System = the lower of the two.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_RESMETER_VIEW_H
#define CIUKI_RESMETER_VIEW_H
#include "systools.h"
#include "../libdeskui/deskui.h"
#define RMV_WIDTH 320
#define RMV_HEIGHT 180
#define RMV_INTERVAL_NS 5000000000ull
enum { RMV_SYSTEM_BAR = 1, RMV_USER_BAR, RMV_GDI_BAR, RMV_SYSTEM_TEXT, RMV_USER_TEXT, RMV_GDI_TEXT, RMV_SUMMARY, RMV_CLOSE, RMV_TIMER = 1 };
struct resmeter_host {
    void *ctx;
    void (*refresh)(void *ctx);     /* gathers the figures and calls rmv_update (maybe later: the window query is asynchronous) */
    void (*quit)(void *ctx);
};
struct resmeter_app {
    struct du_ui *ui; struct resmeter_host host;
    struct rm_state state; int user_known;
    enum rm_level level[3];         /* System, User, GDI bar colour levels */
    unsigned percent[3];
    char text[3][16];
};
int rmv_init(struct resmeter_app *, struct du_ui *, const struct resmeter_host *);
/* One measurement. `windows_known` 0: the desktop did not answer; User is then shown as unavailable and
 * System is the GDI share alone. */
void rmv_update(struct resmeter_app *, const struct rm_desktop *, int windows_known);
void rmv_event(struct du_ui *, const struct du_event *, void *app);
void rmv_paint(struct du_ui *, void *app);
/* Colour of a level (green, amber, red). */
uint32_t rmv_color(enum rm_level);
enum rm_level rmv_level_of(unsigned free_percent);
#endif
