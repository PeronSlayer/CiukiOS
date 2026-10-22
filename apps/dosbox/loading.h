/* Native launch panel drawn using the existing theme and Tango icons.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_LOADING_H
#define CIUKI_DOSBOX_LOADING_H
#include "../libdeskui/deskui.h"
#include "../desktop/catalog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline const char *dosbox_launch_icon(const char *command)
{
    for (unsigned i = 0; i < DESK_GAME_COUNT; i++)
        if (!strcmp(command, desk_games[i].command)) return desk_games[i].tango;
    return du_icon_name("program:dosbox");
}
/* f3-61: the kernel brought a full-screen game back to its window by itself
 * (it keeps running there); STATUS.reason (errno) names why. */
static inline const char *dosbox_return_text(int error)
{
    return error==ETIMEDOUT ? "The game stopped updating the full screen. It is still running in this window." :
                              "Full screen could not continue on this display. The game is still running in this window.";
}
/* f3-61: a session that stopped by itself after it was shown: a guest fault
 * or a DOS deadline (dosvm reasons 2 and 4) keeps the window with a message;
 * EXIT and the owner's own close end it as before. */
static inline int dosbox_stop_reported(unsigned reason) { return reason==2u || reason==4u; }
#define DOSBOX_PULSE_MS 250u
static inline struct du_rect dosbox_panel_pulse(struct du_canvas *canvas, uint64_t elapsed)
{
    struct du_rect bar = {120,272,400,18};
    du_progress_paint(canvas,bar,0,0,elapsed);
    return bar;
}
/* frame: the dialog frame title (f3-61: a running game's notices and a stop
 * after the program was shown are framed by the window name). */
static inline void dosbox_panel_framed(struct du_canvas *canvas, const char *frame, const char *name,
                                       const char *icon, const char *error, const char *detail,
                                       uint64_t elapsed)
{
    du_fill(canvas, canvas->clip, DU_FACE);
    struct du_rect box = {100, 160, 440, 160};
    struct desk_paint p={canvas->pixels,canvas->width,canvas->height,
        canvas->clip.x,canvas->clip.y,canvas->clip.w,canvas->clip.h};
    struct desk_frame_style style={.title=frame,.active=1};
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
    if (!error) dosbox_panel_pulse(canvas,elapsed);
    else du_well(canvas,bar,DU_FACE);
}
static inline void dosbox_panel(struct du_canvas *canvas, const char *name,
                                const char *icon, const char *error, const char *detail,
                                uint64_t elapsed)
{
    dosbox_panel_framed(canvas,error ? "Program did not start" : "Starting program",name,icon,error,detail,elapsed);
}
#endif
