/* Production chrome cursor routing, toolkit hit testing and semantic mapping.
 * All unrelated browser sections are discarded by the host link.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../../sdk/netsurf/port/chrome.c"
#include <assert.h>
static unsigned selected, selections;
int ciuki_browser_cursor(struct ciuki_browser_surface *s,unsigned shape)
{ assert(s==surface);selected=shape;selections++;return 0; }
int main(void)
{
    struct ciuki_browser_surface browser_surface={0};surface=&browser_surface;
    surface->current.info.width=640;surface->current.info.height=480;
    active=true;ui.canvas.width=640;ui.canvas.height=480;ui.capture=ui.menu=-1;
    ui.mouse_x=100;ui.mouse_y=100;
    ciuki_chrome_pointer(GUI_POINTER_POINT);assert(selected==DESK_CURSOR_HAND);
    ciuki_chrome_pointer(GUI_POINTER_CARET);assert(selected==DESK_CURSOR_TEXT);
    ui.count=1;ui.widgets[0]=(struct du_widget){.kind=DU_EDIT,.box={40,4,400,26}};
    ui.mouse_y=20;chrome_cursor();assert(selected==DESK_CURSOR_TEXT);
    ciuki_chrome_pointer(GUI_POINTER_HELP);assert(selected==DESK_CURSOR_TEXT);
    ui.mouse_y=100;chrome_cursor();assert(selected==DESK_CURSOR_HELP);
    loading=true;ciuki_chrome_pointer(GUI_POINTER_DEFAULT);assert(selected==DESK_CURSOR_ARROW_BUSY);
    ciuki_chrome_pointer(GUI_POINTER_POINT);assert(selected==DESK_CURSOR_HAND);
    loading=false;ciuki_chrome_pointer(GUI_POINTER_DEFAULT);assert(selected==DESK_CURSOR_ARROW);
    ui.dialog.kind=1;ui.dialog.box=(struct du_rect){0,0,500,200};
    ui.dialog.count=1;ui.dialog.items[0]=(struct du_widget){.kind=DU_EDIT,.box={80,80,200,30}};
    ciuki_chrome_pointer(GUI_POINTER_POINT);assert(selected==DESK_CURSOR_TEXT);
    ui.dialog.kind=0;ui.mouse_y=470;chrome_cursor();assert(selected==DESK_CURSOR_ARROW);
    unsigned before=selections;active=false;ciuki_chrome_pointer(GUI_POINTER_POINT);assert(selections==before);
    puts("PASS f2-99 browser chrome: content hand/caret/help, URL and dialog I-beams, status arrow, loading feedback and viewport return");
    fflush(stdout);return 0;
}
