/* Portable surface adapter; no window-system dependencies.
 * SPDX-License-Identifier: MIT */
#include "include/ciuki/gl.h"
#include "vendor/src/zgl.h"
#include <errno.h>
#include <string.h>

static ZBuffer *buffer;
static uint32_t *target;
static unsigned visible_width, visible_height, target_stride, generation;

int ciuki_gl_bind(uint32_t *pixels, unsigned width, unsigned height, unsigned stride)
{
    if (!pixels || !width || !height || width>1600 || height>1200 ||
        stride<width || stride>16384) return -EINVAL;
    unsigned padded=(width+3u)&~3u;
    int direct=padded==width && stride==width;
    if (!buffer || (unsigned)buffer->xsize!=padded || (unsigned)buffer->ysize!=height ||
        buffer->frame_buffer_allocated==direct) {
        ZBuffer *next=ZB_open((int)padded,(int)height,ZB_MODE_RGBA,direct ? pixels : NULL);
        if (!next) return -ENOMEM;
        if (!buffer) { glInit(next); generation++; }
        else {
            /* GL raster state lives in ZBuffer too; resizing only replaces
             * storage. Keep depth/blend/stipple/point/texture state intact. */
            next->current_texture=buffer->current_texture;
            next->pointsize=buffer->pointsize;
            memcpy(next->stipplepattern,buffer->stipplepattern,sizeof(next->stipplepattern));
            next->dostipple=buffer->dostipple;
            next->blendeq=buffer->blendeq;next->sfactor=buffer->sfactor;next->dfactor=buffer->dfactor;
            next->enable_blend=buffer->enable_blend;
            next->depth_test=buffer->depth_test;next->depth_write=buffer->depth_write;
            gl_get_context()->zb=next;ZB_close(buffer);
        }
        buffer=next;
    } else if (direct) buffer->pbuf=pixels;
    target=pixels;visible_width=width;visible_height=height;target_stride=stride;
    glViewport(0,0,(int)width,(int)height);
    return 0;
}
void ciuki_gl_finish(void)
{
    if (!buffer) return;
    glFlush();
    if (buffer->frame_buffer_allocated)
        for (unsigned y=0;y<visible_height;y++)
            memcpy(target+(size_t)y*target_stride,buffer->pbuf+(size_t)y*buffer->xsize,visible_width*4u);
}
void ciuki_gl_close(void)
{
    if (!buffer) return;
    glClose();ZB_close(buffer);buffer=NULL;target=NULL;
}
unsigned ciuki_gl_generation(void) { return generation; }
