/* Final visible frames from production desktop, software and accelerated.
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct desktop d;
static uint32_t *scanout;
static unsigned caps, copies, window_copies, fills, uploads, cursor_moves;
static uint64_t uploaded_pixels;
static int fail_copy, fail_cursor, fail_fill, shown, cursor_x, cursor_y;
static struct ciuki_accel_request cursor;
static int upload(int display,int surface,const struct ciuki_rect *r)
{
    (void)display; (void)surface;
    for (uint32_t y=0;y<r->height;y++)
        memcpy(scanout+((uint32_t)r->dst_y+y)*d.width+(uint32_t)r->dst_x,
               d.pixels+((uint32_t)r->src_y+y)*d.width+(uint32_t)r->src_x,r->width*4);
    uploads++; uploaded_pixels+=(uint64_t)r->width*r->height;
    return 0;
}
static void completed_ram_frame(void)
{
    uint32_t *expected=calloc((size_t)d.width*d.height,4); assert(expected);
    struct desktop *oracle=malloc(sizeof(d)); assert(oracle); *oracle=d;
    oracle->pixels=expected;
    desk_composite(oracle,(struct desk_box){0,0,(int)d.width,(int)d.height});
    assert(!memcmp(d.pixels,expected,(size_t)d.width*d.height*4));
    free(oracle); free(expected);
}
static int service(int display,const struct ciuki_accel_request *q)
{
    assert(display==7 && q->size==sizeof(*q) && q->generation==55);
    if (q->operation==CIUKI_ACCEL_QUERY) return caps;
    if (q->operation==CIUKI_ACCEL_COPY || q->operation==CIUKI_ACCEL_FILL) completed_ram_frame();
    if (q->operation==CIUKI_ACCEL_COPY) {
        copies++;
        if (q->rect.height>1) window_copies++;
        for (uint32_t n=0;n<q->rect.height;n++) {
            uint32_t y=q->rect.dst_y>q->rect.src_y ? q->rect.height-1-n : n;
            memmove(scanout+((uint32_t)q->rect.dst_y+y)*d.width+(uint32_t)q->rect.dst_x,
                    scanout+((uint32_t)q->rect.src_y+y)*d.width+(uint32_t)q->rect.src_x,q->rect.width*4);
            if (fail_copy) { fail_copy=0; caps=0; shown=0; return -EIO; }
        }
    } else if (q->operation==CIUKI_ACCEL_FILL) {
        fills++;
        for (uint32_t y=0;y<q->rect.height;y++) for (uint32_t x=0;x<q->rect.width;x++)
            scanout[((uint32_t)q->rect.dst_y+y)*d.width+(uint32_t)q->rect.dst_x+x]=q->colour;
        if (fail_fill) { fail_fill=0; caps=0; shown=0; return -EIO; }
    } else if (q->operation==CIUKI_ACCEL_CURSOR_SET) cursor=*q;
    else if (q->operation==CIUKI_ACCEL_CURSOR_MOVE) {
        if (fail_cursor) { caps=0; shown=0; fail_cursor=0; return -EIO; }
        cursor_moves++; cursor_x=q->rect.dst_x; cursor_y=q->rect.dst_y;
    } else if (q->operation==CIUKI_ACCEL_CURSOR_SHOW) shown=q->colour!=0;
    return 0;
}
static void identical_frame(void)
{
    uint32_t *expected=calloc((size_t)d.width*d.height,4), *visible=malloc((size_t)d.width*d.height*4);
    assert(expected && visible);
    struct desktop *oracle=malloc(sizeof(d)); assert(oracle); *oracle=d;
    oracle->pixels=expected; oracle->hw_cursor=0;
    desk_composite(oracle,(struct desk_box){0,0,(int32_t)d.width,(int32_t)d.height});
    memcpy(visible,scanout,(size_t)d.width*d.height*4);
    if (shown) for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x++) {
        int px=cursor_x+(int)x,py=cursor_y+(int)y;
        if (px<0 || py<0 || (unsigned)px>=d.width || (unsigned)py>=d.height) continue;
        if (cursor.cursor_mask[y]&(0x80000000u>>x))
            visible[(unsigned)py*d.width+(unsigned)px]=(cursor.cursor_image[y]&(0x80000000u>>x)) ? cursor.colour : cursor.background;
    }
    if (memcmp(visible,expected,(size_t)d.width*d.height*4)) {
        for (unsigned p=0;p<d.width*d.height;p++) if (visible[p]!=expected[p]) {
            fprintf(stderr,"frame mismatch x=%u y=%u got=%x expected=%x caps=%u copies=%u\n",p%d.width,p/d.width,visible[p],expected[p],caps,copies);
            break;
        }
        abort();
    }
    free(oracle);free(visible);free(expected);
}
static void redraw(void)
{
    /* Cursor failure intentionally queues one software recovery frame. */
    for (unsigned retry=0;retry<3;retry++) if (!desk_redraw(&d)) { identical_frame(); return; }
    abort();
}
static int window_new(int ch,int surf,unsigned w,unsigned h,int x,int y)
{
    fake_channel(ch); int i=desk_add_client(&d,ch); assert(i>=0);
    struct ciuki_message m;desk_message(&m,DESK_HELLO,4);m.sender_pid=42+i;desk_receive(&d,i,&m);
    fake_surface(surf,w,h,0x123456);
    desk_message(&m,DESK_CREATE_WINDOW,20);m.sender_pid=42+i;m.fd_count=1;m.fds[0]=surf;
    desk_put32(m.data+4,w);desk_put32(m.data+8,h);desk_put32(m.data+12,4);memcpy(m.data+16,"Test",4);
    desk_receive(&d,i,&m);assert(d.clients[i].window);
    desk_move(&d,i,x,y);
    uint32_t *p=(uint32_t *)d.clients[i].pixels;
    for (unsigned row=0;row<h;row++) for (unsigned col=0;col<w;col++) p[row*w+col]=((row+1)*92761u+col*817u)&0xffffff;
    return i;
}
static void dirty_client(int i)
{ desk_damage_add(&d.damage,desk_content(&d.clients[i]),d.width,d.height); }
static void scenario(unsigned available)
{
    fake_reset();struct desk_ops ops=fake_ops;ops.present=upload;
    desk_init(&d,&ops,640,480);d.display=7;
    d.pixels=calloc((size_t)d.width*d.height,4);scanout=calloc((size_t)d.width*d.height,4);assert(d.pixels && scanout);
    caps=available;copies=window_copies=fills=uploads=cursor_moves=0;uploaded_pixels=0;shown=fail_copy=fail_cursor=fail_fill=0;
    if (available) { d.accel=service;desk_accel_reset(&d,55); }
    int i=window_new(10,20,120,80,50,80);redraw();
    unsigned before_uploads=uploads;uint64_t before_pixels=uploaded_pixels;
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=13,.value2=5};
    desk_input(&d,&e);redraw();
    unsigned cursor_uploads=uploads-before_uploads;
    assert(d.frame_stats.scene_px==0);
    if (available&2u) {
        assert(cursor_uploads==0 && cursor_moves>=2);
        assert(!d.frame_stats.presents && !d.frame_stats.lfb_bytes && !d.frame_stats.damage_px);
    } else assert(d.frame_stats.damage_px<=374);
    unsigned before_copies=window_copies;
    desk_move(&d,i,80,95);desk_move(&d,i,90,100);redraw();
    if (available&2u) assert(window_copies==before_copies+1);
    /* Final chrome and new client content arriving during a pending copy. */
    desk_move(&d,i,110,105);
    uint32_t *pixels=(uint32_t *)d.clients[i].pixels;pixels[17]=0xabc012;dirty_client(i);redraw();
    /* Scroll up, then down. Exposed rows have entirely new content. */
    unsigned width=d.clients[i].info.width,height=d.clients[i].info.height;
    before_copies=window_copies;
    memmove(pixels,pixels+7*width,(height-7)*width*4);
    for (unsigned p=(height-7)*width;p<height*width;p++) pixels[p]=(p*7631u)&0xffffff;
    dirty_client(i);redraw();
    /* Application requests change the actual cursor in both presenters.
     * Compare every Paint shape after rapid switches and screen clipping. */
    for(unsigned shape=DESK_CURSOR_TEXT;shape<DESK_CURSOR_COUNT;shape++) {
        d.clients[i].cursor=shape;
        struct desk_box content=desk_content(&d.clients[i]);
        d.mouse_x=content.x+12; d.mouse_y=content.y+12;
        desk_cursor_update(&d); assert(d.cursor_shape==shape); redraw();
        d.mouse_x=0; d.mouse_y=0; d.cursor_shape=shape; d.cursor_dirty=1; redraw();
        d.mouse_x=(int)d.width-1; d.mouse_y=(int)d.height-1; d.cursor_dirty=1; redraw();
    }
    d.clients[i].cursor=DESK_CURSOR_ARROW; desk_cursor_update(&d); redraw();
    if (available&2u) assert(window_copies==before_copies+1);
    before_copies=window_copies;
    memmove(pixels+5*width,pixels,(height-5)*width*4);
    for (unsigned p=0;p<5*width;p++) pixels[p]=(p*1483u)&0xffffff;
    dirty_client(i);redraw();
    if (available&2u) assert(window_copies==before_copies+1);
    int j=window_new(11,21,130,90,130,110);redraw();
    before_copies=window_copies;desk_move(&d,i,140,120);redraw();assert(window_copies==before_copies);
    desk_drop_client(&d,j);redraw();
    printf("desktop accel=%u normal frames identical: copies=%u fills=%u uploads=%u uploaded_pixels=%llu cursor_motion_uploads=%u\n",
        available,copies,fills,uploads,(unsigned long long)uploaded_pixels,cursor_uploads);
    assert(d.stats.presents==copies+fills+uploads);
    assert(d.stats.lfb_bytes==uploaded_pixels*4);
    /* Enlarged software frames repeat non-solid scanlines. Their first row
     * is CPU-uploaded; duplicates must use actual screen copies when present. */
    before_copies=copies;
    for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++)
        pixels[y*width+x]=0x203040u+(y/2)*4173u+x*89u;
    dirty_client(i);redraw();
    if (available) assert(copies>before_copies);
    if (available) {
        fail_copy=1;
        for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++)
            pixels[y*width+x]=0x507080u+(y/2)*6319u+x*97u;
        dirty_client(i);redraw();redraw();assert(!caps && !d.accel_caps);
        caps=available;desk_accel_reset(&d,55);redraw();
    }
    /* A partially completed engine copy is reconstructed from client RAM. */
    if (available&2u) {
        fail_copy=1;desk_move(&d,i,160,130);redraw();redraw();assert(caps==0 && !d.hw_cursor);
        caps=3;desk_accel_reset(&d,55);redraw();
        fail_fill=1;
        for (unsigned p=0;p<width*height;p++) pixels[p]=0x234567;
        dirty_client(i);redraw();assert(!d.hw_cursor && caps==0);
        caps=3;desk_accel_reset(&d,55);redraw();
        fail_cursor=1;desk_input(&d,&e);redraw();assert(!d.hw_cursor && caps==0);
    }
    for (unsigned p=0;p<width*height;p++) pixels[p]=0x123456;
    dirty_client(i);redraw();
    printf("desktop accel=%u frames identical: copies=%u fills=%u uploads=%u pixels=%llu cursor_motion_uploads=%u delta_pixels=%llu\n",
        available,copies,fills,uploads,(unsigned long long)uploaded_pixels,cursor_uploads,
        (unsigned long long)(uploaded_pixels-before_pixels));
    for (int k=0;k<DESK_CLIENTS;k++) desk_drop_client(&d,k);
    desk_render_release(&d);free(d.pixels);free(scanout);assert(!fake_live());
}
int main(void)
{
    scenario(0);scenario(1);scenario(3);
    /* Actual mode reactivation is deferred. A completed CPU-presented frame
     * permits later screen blits when rectangle capability becomes ready. */
    fake_reset();struct desk_ops ops=fake_ops;ops.present=upload;desk_init(&d,&ops,640,480);d.display=7;
    d.pixels=calloc((size_t)d.width*d.height,4);scanout=calloc((size_t)d.width*d.height,4);assert(d.pixels && scanout);
    caps=0;copies=window_copies=fills=uploads=cursor_moves=0;uploaded_pixels=0;shown=fail_copy=fail_cursor=fail_fill=0;
    d.accel=service;desk_accel_reset(&d,55);
    int i=window_new(10,20,120,80,50,80);redraw();assert(d.scanout_valid && !d.accel_caps && !copies && !fills);
    caps=1;dirty_client(i);redraw();assert(d.scanout_valid && d.accel_caps==1);
    uint32_t *pixels=(uint32_t *)d.clients[i].pixels;
    for(unsigned y=0;y<80;y++)for(unsigned x=0;x<120;x++)pixels[y*120+x]=0x203040+y/2*4173+x*89;
    dirty_client(i);redraw();assert(copies>0);
    for(int k=0;k<DESK_CLIENTS;k++)desk_drop_client(&d,k);
    desk_render_release(&d);free(d.pixels);free(scanout);assert(!fake_live());
    puts("desktop acceleration/fallback visible-frame equivalence, Paint cursor shapes/hotspots/clipping and late-ready rectangles: PASS");return 0;
}
