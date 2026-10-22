/* NetSurf's semantic pointer -> CiukiOS's original system artwork.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NETSURF_POINTER_H
#define CIUKI_NETSURF_POINTER_H
#include "netsurf/mouse.h"
#include "apps/desktop/protocol.h"
static inline unsigned ciuki_pointer_shape(unsigned shape)
{
    switch (shape) {
    case GUI_POINTER_POINT: return DESK_CURSOR_HAND;
    case GUI_POINTER_CARET: return DESK_CURSOR_TEXT;
    case GUI_POINTER_UP: case GUI_POINTER_DOWN: return DESK_CURSOR_RESIZE_NS;
    case GUI_POINTER_LEFT: case GUI_POINTER_RIGHT: return DESK_CURSOR_RESIZE_EW;
    case GUI_POINTER_RU: case GUI_POINTER_LD: return DESK_CURSOR_RESIZE_NESW;
    case GUI_POINTER_LU: case GUI_POINTER_RD: return DESK_CURSOR_RESIZE_NWSE;
    case GUI_POINTER_CROSS: return DESK_CURSOR_CROSSHAIR;
    case GUI_POINTER_MOVE: return DESK_CURSOR_MOVE;
    case GUI_POINTER_WAIT: return DESK_CURSOR_BUSY;
    case GUI_POINTER_HELP: return DESK_CURSOR_HELP;
    case GUI_POINTER_NO_DROP: case GUI_POINTER_NOT_ALLOWED: return DESK_CURSOR_FORBIDDEN;
    case GUI_POINTER_PROGRESS: return DESK_CURSOR_ARROW_BUSY;
    default: return DESK_CURSOR_ARROW;
    }
}
#endif
