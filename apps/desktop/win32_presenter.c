/* Desktop side of the f5-08 atomic mode/lease transaction.
 * SPDX-License-Identifier: MIT */
#include "desktop.h"
#include <ciuki/present_abi.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
uint32_t desk_w32_record(void *,uint32_t);
int ciuki_win32_presenter_lease(struct desktop *d,uint32_t pid,int acquire,int mode,
    uint32_t w,uint32_t h,uint32_t bpp,uint32_t generation)
{
    struct ciuki_w32_display_args a={.header={1,sizeof(a),W32_DISPLAY,0},
        .action=acquire ? W32_DISPLAY_GRANT : W32_DISPLAY_REVOKE,.pid=pid};
    if (acquire) {
        if (!w || !h || w>2048 || h>2048 || (bpp!=8 && bpp!=16 && bpp!=32)) return -EINVAL;
        a.mode=(uint32_t)mode;
        a.surface=(struct ciuki_present_surface){w,h,w*(bpp/8),w*h*(bpp/8),
            bpp==8 ? CIUKI_PRESENT_INDEX8 : bpp==16 ? CIUKI_PRESENT_RGB565 : CIUKI_PRESENT_XRGB8888};
    } else {
        a.token=d->v3_lease_tuple.token; a.generation=d->v3_lease_tuple.generation;
        if (!a.token || !a.generation) return -EACCES;
    }
    uint32_t status=desk_w32_record(&a,sizeof(a));
    int error=status ? ((int32_t)status<0 && status>=(uint32_t)-4095 ? (int32_t)status : -EIO) :
        a.error ? -(int)a.error : a.result ? 0 : -EIO;
    /* Teardown, DOS takeover or the first-frame watchdog may already have
     * revoked this tuple. EACCES alone is insufficient: prove the desktop's
     * display grant is usable again before permitting a repaint. */
    if (!acquire && error==-EACCES && d->display_info) {
        struct ciuki_framebuffer_info fb;
        if (!d->display_info(d->display,&fb) && fb.width==d->width && fb.height==d->height) error=0;
    }
    if (!error && acquire) {
        d->v3_lease_tuple.token=a.token;d->v3_lease_tuple.generation=a.generation;
        d->v3_lease_tuple.mode=a.mode;d->v3_lease_tuple.scale=a.scale;
        d->v3_lease_tuple.surface=a.surface;
        d->hw_cursor=0;d->cursor_loaded=DESK_CURSOR_COUNT;d->cursor_valid=0;
    }
    if (!acquire || error) {
        d->scanout_valid=d->frame_valid=0;d->cursor_valid=0;
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    }
    fprintf(stderr,"[win32] display lease=%s pid=%u serial=%u token=%u generation=%u mode=%u result=%d\n",
        acquire ? "grant" : "revoke",pid,generation,a.token,a.generation,a.mode,error);
    return error;
}
void desk_w32_presenter_poll(struct desktop *d)
{
    if (d->v3_lease_state!=1 || !d->display_info) return;
    struct ciuki_framebuffer_info fb;
    if (!d->display_info(d->display,&fb)) {
        /* Successful grant query means the kernel has resumed desktop
         * ownership. Keep composition paused until synchronous reconciliation. */
        int error=d->now_ns-d->v3_lease_started>=10000000000ull ? -ETIMEDOUT : -EIO;
        desk_v3_lease_failed(d,error);
    }
}
