/* SPDX-License-Identifier: MIT
 * G5's shared text helpers validate UTF-8 and convert Unicode/line endings.
 * The window-title validator rejects tabs/newlines and must not be used here. */
#include "apps/libdeskui/deskui.h"
static void gui_get_clipboard(char **buffer,size_t *length)
{
    *buffer=NULL;*length=0;
    const char *text=du_clipboard_get();if(!text)return;
    size_t n=strlen(text);
    if(n>DU_CLIPBOARD_MAX)return;
    char *copy=malloc(n+1);if(!copy)return;
    memcpy(copy,text,n+1);*buffer=copy;*length=n;
}
static void gui_set_clipboard(const char *buffer,size_t length,nsclipboard_styles styles[],int n_styles)
{
    (void)styles;(void)n_styles;
    if(length>DU_CLIPBOARD_MAX || (!buffer && length))return;
    du_clipboard_set_n(buffer,length);
}
static struct gui_clipboard_table clipboard_table={.get=gui_get_clipboard,.set=gui_set_clipboard};
struct gui_clipboard_table *framebuffer_clipboard_table=&clipboard_table;
