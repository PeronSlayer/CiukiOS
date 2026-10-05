/* Checked extended-memory storage and native RGB presentation for CiukWeb. */
#ifndef CIUKIOS_WEBSTORE_H
#define CIUKIOS_WEBSTORE_H
#include "app.h"
/* Shared scratch: image decode results are committed before any paint call. */
extern u8 webstore_pixels[6146];

u16 webstore_alloc(u32 bytes);
u32 webstore_lock(u16 handle);
void webstore_unlock(u16 handle);
void webstore_free(u16 handle);
int webstore_read(u16 handle,u32 offset,void *buffer,u16 count);
int webstore_write(u16 handle,u32 offset,const void *buffer,u16 count);
u16 webstore_available(void); /* largest free XMS block, KiB */
/* RGB888, padded to an even row stride; no physical video-mode changes. */
void webstore_image(u16 handle,u16 width,u16 height,int x,int y,int w,int h,
                    int clip_x,int clip_y,int clip_w,int clip_h);
#endif
