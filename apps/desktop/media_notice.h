/* Shared presentation of the copied media blocker list. SPDX-License-Identifier: MIT */
#ifndef DESK_MEDIA_NOTICE_H
#define DESK_MEDIA_NOTICE_H
#include "desktop.h"
#include "../libdeskui/deskui.h"
#include <ciuki/abi.h>
#include <stdio.h>
#include <string.h>

/* Match notification_box/draw_notification's maximum complete body. */
static inline struct du_rect desk_media_notice_body(int width,int height)
{
    int w=width-16; if (w>336) w=336;
    return (struct du_rect){0,0,w-68,height-2*DESK_BAR-80};
}
/* du_message may narrow to 300 px. Budget at that guaranteed width so its
 * final measured layout cannot acquire extra wrapped rows and clip the tail.
 * The rest is dialog.c's title, body margins and OK-button row. */
static inline struct du_rect desk_media_dialog_body(int width,int height)
{
    int w=width-8; if (w>300) w=300;
    return (struct du_rect){0,0,w-78,height-8-DESK_FRAME_TITLE-70};
}
static inline int desk_media_text_fits(const char *text,struct du_rect body)
{
    int pitch=desk_font_line(desk_theme->font_ui)+1;
    return du_text_wrap(NULL,body,text,0)*pitch<=body.h;
}
static inline void desk_media_blocker_text(char *out, size_t size, const struct ciuki_media_blockers *b,
                                         struct du_rect body)
{
    if (!size) return;
    snprintf(out,size,"Close files on this drive and try again.");
    if (!b || !b->count) return;
    snprintf(out,size,"Close these programs and try again:");
    unsigned count=b->count<CIUKI_PROCESS_MAX ? b->count : CIUKI_PROCESS_MAX;
    for (unsigned i=0;i<count;i++) {
        char line[64], more[48];
        snprintf(line,sizeof(line),"\n%.31s (%u)",b->programs[i].name,b->programs[i].pid);
        snprintf(more,sizeof(more),"\n... and %u more programs.",count-i-1);
        size_t n=strlen(out), end=n+strlen(line);
        /* Try the complete name plus the count that would follow it. The
         * last accepted entry has already reserved room for this fallback. */
        int fits=end+(i+1<count ? strlen(more) : 0)<size;
        if (fits) {
            snprintf(out+n,size-n,"%s%s",line,i+1<count ? more : "");
            fits=desk_media_text_fits(out,body);
        }
        if (!fits) {
            snprintf(out+n,size-n,"\n... and %u more programs.",count-i);
            break;
        }
        out[end]=0;
    }
}
#endif
