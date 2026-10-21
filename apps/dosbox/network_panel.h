/* Settings panel extension, same Ciuki portrait and existing theme.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_NETWORK_PANEL_H
#define CIUKI_DOSBOX_NETWORK_PANEL_H
#include "loading.h"
static inline void dosbox_network_panel(struct du_canvas *canvas, const char *selection, const char *detail)
{
    du_fill(canvas, (struct du_rect){100,120,440,280}, DU_FACE);
    struct desk_paint paint = {canvas->pixels,canvas->width,canvas->height,
        canvas->clip.x,canvas->clip.y,canvas->clip.w,canvas->clip.h};
    struct desk_frame_style style = {.title="Game settings",.active=1};
    desk_paint_frame(&paint,100,120,440,280,&style);
    du_icon_draw(canvas,120,155,du_icon(DU_ICON_CIUKI,32),DU_ICON_NORMAL);
    du_text_font(canvas,desk_theme->font_bold,170,160,"DOS kernel, memory and network",DU_INK);
    /* Eight rows of 16 px (Inter 12 ascent+descent+1) below the 32 px icon. */
    du_text_wrap(canvas,(struct du_rect){120,188,400,128},
        "1  CiukiDOS (default)\n2  FreeDOS 1.3\n3  Memory paged (default)\n"
        "4  Memory locked (compatibility)\n5  Network: off (default)\n"
        "6  Network: IPX (LAN)\n7  CiukiDOS unified (preview)\n"
        "F12 or Esc to return to the game.",DU_INK);
    du_text_wrap(canvas,(struct du_rect){120,318,400,40},selection,DU_INK);
    du_text_wrap(canvas,(struct du_rect){120,358,400,40},detail,DU_INK);
}
#endif
