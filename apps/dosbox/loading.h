/* Native launch panel drawn using the existing theme and Tango icons.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_LOADING_H
#define CIUKI_DOSBOX_LOADING_H
#include "../libdeskui/deskui.h"
#include "../desktop/catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline const char *dosbox_launch_icon(const char *command)
{
    for (unsigned i = 0; i < DESK_GAME_COUNT; i++)
        if (!strcmp(command, desk_games[i].command)) return desk_games[i].tango;
    return du_icon_name("program:dosbox");
}
#define DOSBOX_PULSE_MS 250u
static inline struct du_rect dosbox_panel_pulse(struct du_canvas *canvas, uint64_t elapsed)
{
    struct du_rect bar = {120,272,400,18};
    du_progress_paint(canvas,bar,0,0,elapsed);
    return bar;
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
    if (!error) dosbox_panel_pulse(canvas,elapsed);
    else du_well(canvas,bar,DU_FACE);
}
/* Save only the dialog rectangle and its border, allocated while it is open. */
#define DOSBOX_SETTINGS_X 80u
#define DOSBOX_SETTINGS_Y 120u
#define DOSBOX_SETTINGS_W 480u
#define DOSBOX_SETTINGS_H 280u
static inline int dosbox_settings_capture(struct du_canvas *canvas,uint32_t **saved)
{
    *saved=malloc(DOSBOX_SETTINGS_W*DOSBOX_SETTINGS_H*sizeof(uint32_t));
    if (!*saved) return -1;
    for (unsigned y=0;y<DOSBOX_SETTINGS_H;y++)
        memcpy(*saved+y*DOSBOX_SETTINGS_W,canvas->pixels+(y+DOSBOX_SETTINGS_Y)*canvas->width+DOSBOX_SETTINGS_X,
               DOSBOX_SETTINGS_W*sizeof(uint32_t));
    return 0;
}
static inline void dosbox_settings_restore(struct du_canvas *canvas,uint32_t **saved)
{
    if (!*saved) return;
    for (unsigned y=0;y<DOSBOX_SETTINGS_H;y++)
        memcpy(canvas->pixels+(y+DOSBOX_SETTINGS_Y)*canvas->width+DOSBOX_SETTINGS_X,*saved+y*DOSBOX_SETTINGS_W,
               DOSBOX_SETTINGS_W*sizeof(uint32_t));
    free(*saved); *saved=NULL;
}
/* Existing window paint primitives, same Ciuki portrait and Tango family. */
/* Per-game settings: DOS kernel (f3-51) and memory mode (f3-46). */
static inline void dosbox_kernel_panel(struct du_canvas *canvas,const char *selection,const char *detail)
{
    du_fill(canvas,(struct du_rect){100,120,440,280},DU_FACE);
    struct desk_paint paint={canvas->pixels,canvas->width,canvas->height,
        canvas->clip.x,canvas->clip.y,canvas->clip.w,canvas->clip.h};
    struct desk_frame_style style={.title="Game settings",.active=1};
    desk_paint_frame(&paint,100,120,440,280,&style);
    du_icon_draw(canvas,120,160,du_icon(DU_ICON_CIUKI,32),DU_ICON_NORMAL);
    du_text_font(canvas,desk_theme->font_bold,170,160,"DOS kernel and memory",DU_INK);
    du_text_wrap(canvas,(struct du_rect){120,200,400,100},
                 "1  CiukiDOS (default)\n2  FreeDOS 1.3\n3  Memory paged (default)\n"
                 "4  Memory locked (compatibility)\n5  CiukiDOS unified (preview)\nF12 or Esc to return to the game.",DU_INK);
    du_text_wrap(canvas,(struct du_rect){120,305,400,40},selection,DU_INK);
    du_text_wrap(canvas,(struct du_rect){120,345,400,48},detail,DU_INK);
}
#endif
