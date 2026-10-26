/* Production DOS Box viewport -> client damage -> desktop -> COPY/FILL
 * transport. Synthetic title-shaped frames, no guest/game image.
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/dosbox/window.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static struct desktop d;
static uint32_t *scanout;
static uint64_t uploaded, copied, filled, requests;
static int upload(int display,int surface,const struct ciuki_rect *r)
{
    (void)display;(void)surface;
    for (unsigned y=0;y<r->height;y++)memcpy(scanout+(r->dst_y+y)*d.width+r->dst_x,
        d.pixels+(r->src_y+y)*d.width+r->src_x,r->width*4);
    uploaded+=(uint64_t)r->width*r->height;requests++;
    return 0;
}
static int engine(int display,const struct ciuki_accel_request *q)
{
    assert(display==7);
    if (q->operation==CIUKI_ACCEL_QUERY)return 3;
    if (q->operation==CIUKI_ACCEL_OFFSCREEN)return 0;
    if (q->operation==CIUKI_ACCEL_COPY) {
        const struct ciuki_rect *r=&q->rect;
        /* COPY directions, including overlap, follow the display contract. */
        for (unsigned n=0;n<r->height;n++) {
            unsigned y=r->dst_y>r->src_y ? r->height-1-n : n;
            memmove(scanout+(r->dst_y+y)*d.width+r->dst_x,
                scanout+(r->src_y+y)*d.width+r->src_x,r->width*4);
        }
        copied+=(uint64_t)r->width*r->height;requests++;
    } else if (q->operation==CIUKI_ACCEL_FILL) {
        for (unsigned y=0;y<q->rect.height;y++)for (unsigned x=0;x<q->rect.width;x++)
            scanout[(q->rect.dst_y+y)*d.width+q->rect.dst_x+x]=q->colour;
        filled+=(uint64_t)q->rect.width*q->rect.height;requests++;
    }
    return 0;
}
static void check_frame(void)
{
    uint32_t *reference=calloc((size_t)d.width*d.height,4);assert(reference);
    uint32_t *pixels=d.pixels;d.pixels=reference;
    desk_composite(&d,(struct desk_box){0,0,(int)d.width,(int)d.height});d.pixels=pixels;
    assert(!memcmp(reference,pixels,(size_t)d.width*d.height*4));
    assert(!memcmp(reference,scanout,(size_t)d.width*d.height*4));
    free(reference);
}
static void replay(unsigned title,unsigned zoom)
{
    fake_reset();struct desk_ops ops=fake_ops;ops.present=upload;
    desk_init(&d,&ops,1024,768);d.display=7;
    d.pixels=calloc((size_t)d.width*d.height,4);
    scanout=calloc((size_t)d.width*d.height,4);assert(d.pixels && scanout);
    d.accel=engine;desk_accel_reset(&d,55);
    fake_channel(10);fake_surface(20,640,480,4);
    int client=desk_add_client(&d,10);assert(client>=0);
    struct ciuki_message m;desk_message(&m,DESK_HELLO,4);m.sender_pid=42;
    desk_receive(&d,client,&m);
    desk_message(&m,DESK_CREATE_WINDOW,20);m.sender_pid=42;m.fd_count=1;m.fds[0]=20;
    desk_put32(m.data+4,640);desk_put32(m.data+8,480);desk_put32(m.data+12,4);
    memcpy(m.data+16,"Test",4);desk_receive(&d,client,&m);
    desk_move(&d,client,100,100);assert(d.clients[client].window);
    struct dosbox_window w={0};dosbox_profile_default(&w.profile,"TEST.EXE");w.profile.zoom=zoom;
    uint32_t *guest=calloc(640*480,4);assert(guest);
    struct du_canvas canvas={.pixels=(uint32_t *)d.clients[client].pixels,.width=640,.height=480,
        .clip={0,0,640,480}};
    db_paint(&w,&canvas,guest);assert(!desk_redraw(&d));check_frame();
    uploaded=copied=filled=requests=0;
    uint64_t scene=0,compares=0,client_words=0;
    for (unsigned frame=1;frame<=4;frame++) {
        uint32_t source[15],damage[15]={0};
        memset(source,0,sizeof(source));
        for (unsigned y=0;y<480;y++)if(title==2 || (y>=40 && y<440))source[y/32]|=1u<<(y%32);
        for (unsigned y=0;y<480;y++)for (unsigned x=0;x<640;x++) {
            /* Mode Y: 320x200 -> 640x400, centred in the kernel surface.
             * Its unchanged black borders are absent from scanline damage. */
            if(title!=2 && (y<40 || y>=440)) { guest[y*640+x]=0;continue; }
            /* Vary the palette without turning every frame into a scroll
             * that the compositor could satisfy by translating scanout. */
            unsigned index=title==2 ? (x+y*7)^(frame*13) : title==1 ?
                ((x/8)*17+(y/2)*3)^(frame*37) : (x/2+(y/2)*7)^(frame*29);
            guest[y*640+x]=(index&255u)*0x010101u;
        }
        db_paint_rows(&w,&canvas,guest,source,damage);
        struct desk_box content=desk_content(&d.clients[client]);
        for (unsigned y=0;y<480;) {
            if (!(damage[y/32]&(1u<<(y%32)))) { y++;continue; }
            unsigned first=y++;
            while(y<480 && (damage[y/32]&(1u<<(y%32))))y++;
            client_words+=(uint64_t)(y-first)*640;
            desk_client_damage(&d,client,(struct desk_box){content.x,content.y+(int)first,640,(int)(y-first)});
        }
        assert(!desk_redraw(&d));check_frame();
        scene+=d.frame_stats.scene_px;compares+=d.frame_stats.compare_px;
        uint64_t calls=requests;
        /* A repeated viewport is neither recomposed nor presented. */
        assert(!desk_client_damage(&d,client,content));assert(!desk_redraw(&d));
        assert(requests==calls);
    }
    static const char *const names[]={"doom-shaped","wolf-shaped","war2-shaped"};
    printf("dos-window-replay title=%s zoom=%u frames=4 viewport_bytes_per_frame=%llu scene_px_per_frame=%llu compare_px_per_frame=%llu upload_bytes_per_frame=%llu copy_px_per_frame=%llu fill_px_per_frame=%llu requests_per_frame=%llu unchanged_requests=0\n",
        names[title],zoom,(unsigned long long)(client_words*4/4),
        (unsigned long long)(scene/4),(unsigned long long)(compares/4),
        (unsigned long long)(uploaded*4/4),(unsigned long long)(copied/4),
        (unsigned long long)(filled/4),(unsigned long long)(requests/4));
    free(guest);for(int i=0;i<DESK_CLIENTS;i++)desk_drop_client(&d,i);
    desk_render_release(&d);free(d.sh.icons);free(d.sh.wall);free(d.pixels);free(scanout);
    assert(!fake_live());
}
int main(void)
{
    for(unsigned title=0;title<3;title++)for(unsigned zoom=0;zoom<3;zoom++)replay(title,zoom);
    puts("PASS fx-doswin3 viewport/compositor/COPY replay, full RAM and scanout oracle, unchanged frames have zero requests");
    return 0;
}
