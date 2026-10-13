/* Native launch panel drawn using the existing theme and Tango icons.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_LOADING_H
#define CIUKI_DOSBOX_LOADING_H
#include "../libdeskui/deskui.h"
#include "../desktop/catalog.h"

static inline const char *dosbox_launch_icon(const char *command)
{
    for (unsigned i = 0; i < DESK_GAME_COUNT; i++)
        if (!strcmp(command, desk_games[i].command)) return desk_games[i].tango;
    return du_icon_name("program:dosbox");
}
static inline void dosbox_panel(struct du_canvas *canvas, const char *name,
                                const char *icon, const char *error, const char *detail,
                                uint64_t elapsed)
{
    du_fill(canvas, canvas->clip, DU_FACE);
    struct du_rect box = {100, 160, 440, 160};
    du_fill(canvas, box, DU_FACE); du_edge(canvas, box, DU_RAISED);
    du_icon_draw(canvas, 120, 186, du_icon(icon, 32), DU_ICON_NORMAL);
    char title[80];
    snprintf(title, sizeof(title), error ? "%s" : "Loading %s...", error ? error : name);
    du_text_fit(canvas, 166, 186, 350, title, DU_INK);
    du_text_fit(canvas, 166, 214, 350, detail, DU_INK);
    struct du_rect bar = {120, 264, 400, 18};
    du_edge(canvas, bar, DU_SUNKEN);
    if (!error) {
        /* An indeterminate pulse, not an invented completion percentage. */
        int offset = (int)((elapsed / 80u) % 344u);
        du_fill(canvas, (struct du_rect){124 + offset, 268, 48, 10}, DU_SELECT);
    }
}
#endif
