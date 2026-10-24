/* SPDX-License-Identifier: GPL-2.0-only
 * Production browser shortcut/crop boundary; unrelated frontend is discarded. */
#include "../../../sdk/netsurf/port/chrome.c"
#include <assert.h>
/* ASan retains the exported download table when chrome.c is included.
 * This printing-only fixture must never enter the browser download core. */
const char *download_context_get_filename(const download_context *context)
{ (void)context;abort(); }
void download_context_destroy(download_context *context)
{ (void)context;abort(); }
static unsigned prints,setups;
void cp_ui_setup(struct cp_ui *p,struct du_ui *u)
{ assert(p==&printing && u==&ui);setups++; }
void cp_ui_image(struct cp_ui *p,struct du_ui *u,const char *title,const uint32_t *pixels,unsigned width,unsigned height)
{
    assert(p==&printing && u==&ui && !strcmp(title,"NetSurf - visible page"));
    assert(pixels==surface->current.back+CIUKI_BROWSER_TOP*640 && width==640 && height==480-CIUKI_BROWSER_TOP-CIUKI_BROWSER_STATUS);
    prints++;
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);static uint32_t pixels[640*480];
    static struct ciuki_browser_surface browser_surface;surface=&browser_surface;
    surface->ready=true;surface->current.back=pixels;surface->current.info.width=640;surface->current.info.height=480;
    assert(!print_key(0x19) && !prints && !setups);
    ui.modifiers=DU_CTRL;assert(!print_key(0x18));assert(print_key(0x19) && prints==1);
    ui.modifiers|=DU_SHIFT;assert(print_key(0x19) && setups==1 && prints==1);
    ui.modifiers=DU_CTRL;surface->ready=false;assert(print_key(0x19) && prints==1);
    surface->ready=true;surface->current.info.height=20;assert(print_key(0x19) && prints==1);
    puts("PASS G31 NetSurf: Ctrl+P visible-page crop, Ctrl+Shift+P setup, readiness and small-surface bounds");
    return 0;
}
