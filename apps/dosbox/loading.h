/* Native launch panel drawn using the existing theme and Tango icons.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_LOADING_H
#define CIUKI_DOSBOX_LOADING_H
#include "../libdeskui/deskui.h"
#include "../desktop/catalog.h"
#include <stdio.h>
#include <string.h>

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
    struct desk_paint p={canvas->pixels,canvas->width,canvas->height,
        canvas->clip.x,canvas->clip.y,canvas->clip.w,canvas->clip.h};
    struct desk_frame_style style={.title=error ? "Program did not start" : "Starting program",.active=1};
    desk_paint_frame(&p,box.x,box.y,box.w,box.h,&style);
    du_icon_draw(canvas,120,202,du_icon(DU_ICON_CIUKI,32),DU_ICON_NORMAL);
    du_icon_draw(canvas,box.x+box.w-40,box.y+7,du_icon(icon,16),DU_ICON_NORMAL);
    char title[80];
    snprintf(title,sizeof(title),error ? "%s" : "Loading %s...",error ? error : name);
    struct du_rect old=canvas->clip;
    canvas->clip=du_intersection(old,(struct du_rect){166,200,350,24});
    du_text_font(canvas,desk_theme->font_bold,166,202,title,DU_INK); canvas->clip=old;
    du_text_wrap(canvas,(struct du_rect){166,226,350,30},detail,DU_INK);
    struct du_rect bar={120,272,400,18};
    if (!error) du_progress_paint(canvas,bar,0,0,elapsed);
    else du_well(canvas,bar,DU_FACE);
}
#endif
