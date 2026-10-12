/* Shared host-test helpers for the accessories (input synthesis, renders).
 * SPDX-License-Identifier: MIT */
#ifndef ACC_TEST_H
#define ACC_TEST_H
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static inline void at_key(struct du_ui *u, int code, int value)
{ struct ciuki_input_event e = { .type = CIUKI_INPUT_KEY, .code = code, .value = value }; du_input(u, &e); }
static inline void at_tap(struct du_ui *u, int code) { at_key(u, code, 1); at_key(u, code, 0); }
static inline void at_chord(struct du_ui *u, int modifier, int code) { at_key(u, modifier, 1); at_tap(u, code); at_key(u, modifier, 0); }
static inline void at_move(struct du_ui *u, int x, int y)
{ struct ciuki_input_event e = { .type = CIUKI_INPUT_MOTION, .value = x, .value2 = y }; du_input(u, &e); }
static inline void at_click(struct du_ui *u, int x, int y)
{
    at_move(u, x, y);
    struct ciuki_input_event e = { .type = CIUKI_INPUT_BUTTON, .code = 1, .value = 1 }; du_input(u, &e);
    e.value = 0; du_input(u, &e);
}
static inline void at_click_widget(struct du_ui *u, int id)
{
    struct du_widget *w = du_get(u, id); assert(w);
    at_click(u, w->box.x + w->box.w / 2, w->box.y + w->box.h / 2);
}
/* Writes <folder>/<name>.xrgb (raw XRGB8888 rows) when a folder is given. */
static inline void at_save(const char *folder, const char *name, const struct du_ui *u)
{
    if (!folder) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.xrgb", folder, name);
    FILE *f = fopen(path, "wb"); assert(f);
    size_t n = (size_t)u->canvas.width * (size_t)u->canvas.height;
    assert(fwrite(u->canvas.pixels, 4, n, f) == n); fclose(f);
    snprintf(path, sizeof(path), "%s/%s.size", folder, name);
    f = fopen(path, "w"); assert(f); fprintf(f, "%d %d\n", u->canvas.width, u->canvas.height); fclose(f);
}
#endif
