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
    du_text_wrap(canvas,(struct du_rect){120,192,400,112},
        "1  CiukiDOS (default)\n2  FreeDOS 1.3\n3  Memory paged (default)\n"
        "4  Memory locked (compatibility)\n5  Network: off (default)\n"
        "6  Network: IPX (LAN)\nF12 or Esc to return to the game.",DU_INK);
    du_text_wrap(canvas,(struct du_rect){120,308,400,40},selection,DU_INK);
    du_text_wrap(canvas,(struct du_rect){120,350,400,46},detail,DU_INK);
}
#endif
