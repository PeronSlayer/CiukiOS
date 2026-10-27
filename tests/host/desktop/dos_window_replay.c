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
static const uint32_t *client_surface;     /* fx-doswin3 round 3: the DOS window's own surface (fd 20) */
static uint64_t direct_uploads;
static int upload(int display,int surface,const struct ciuki_rect *r)
{
    (void)display;
    /* The kernel's surface present: from the desktop's frame, or straight
     * from a client surface (640 pixels a row) for a direct present. */
    int direct=surface==20;
    const uint32_t *source=direct ? client_surface : d.pixels; unsigned stride=direct ? 640u : d.width;
    assert(!direct || (r->src_x+r->width<=640 && r->src_y+r->height<=480));
    for (unsigned y=0;y<r->height;y++)memcpy(scanout+(r->dst_y+y)*d.width+r->dst_x,
        source+(r->src_y+y)*stride+r->src_x,r->width*4);
    uploaded+=(uint64_t)r->width*r->height;requests++;
    if (direct) direct_uploads+=(uint64_t)r->width*r->height;
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
    assert(!memcmp(reference,scanout,(size_t)d.width*d.height*4));
    /* A direct present leaves older pixels in the RAM frame inside its
     * stale boxes only; everything else is the composed frame. */
    for (unsigned y=0;y<d.height;y++) for (unsigned x=0;x<d.width;x++) {
        int stale=0;
        for (unsigned i=0;i<d.direct_stale.count && !stale;i++) {
            struct desk_box b=d.direct_stale.boxes[i];
            stale=(int)x>=b.x && (int)x<b.x+b.w && (int)y>=b.y && (int)y<b.y+b.h;
        }
        if (!stale) assert(pixels[y*d.width+x]==reference[y*d.width+x]);
    }
    free(reference);
}
static void check_composed(void)
{
    uint32_t *reference=calloc((size_t)d.width*d.height,4);assert(reference);
    uint32_t *pixels=d.pixels;d.pixels=reference;
    desk_composite(&d,(struct desk_box){0,0,(int)d.width,(int)d.height});d.pixels=pixels;
    unsigned ram=0,shown=0;
    for (size_t i=0;i<(size_t)d.width*d.height;i++) { ram+=pixels[i]!=reference[i]; shown+=scanout[i]!=reference[i]; }
    if (ram || shown) fprintf(stderr,"check_composed: %u RAM and %u scanout pixels differ from a full composition\n",ram,shown);
    assert(!ram && !shown);
    free(reference);
}
static void check_settled(void)
{
    desk_direct_settle(&d); assert(!d.direct_stale.count);
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
    client_surface=d.clients[client].pixels;
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
/* fx-doswin3 round 3: the same production pipeline for a launched DOS
 * window (dosbox=1). Unobscured, its damage is presented straight from its
 * surface (one request per damaged run of rows, nothing composed); a
 * window over it, a scroll or a capture recomposes the older RAM pixels
 * first. The scanout always equals a full composition. The scene-less
 * GFX_FRAME after a frame's DAMAGE is answered once that frame is shown. */
static void frame_pixels(uint32_t *guest, unsigned frame)
{
    for (unsigned y=0;y<480;y++)for (unsigned x=0;x<640;x++)
        guest[y*640+x]=(y<40 || y>=440) ? 0 : (((x/2+(y/2)*7)^(frame*29))&255u)*0x010101u;
}
static void damage_frame(struct dosbox_window *w, struct du_canvas *canvas, uint32_t *guest, int client, unsigned frame)
{
    uint32_t source[15]={0},damage[15]={0};
    for (unsigned y=40;y<440;y++)source[y/32]|=1u<<(y%32);
    frame_pixels(guest,frame);
    db_paint_rows(w,canvas,guest,source,damage);
    struct desk_box content=desk_content(&d.clients[client]);
    for (unsigned y=0;y<480;) {
        if (!(damage[y/32]&(1u<<(y%32)))) { y++;continue; }
        unsigned first=y++;
        while(y<480 && (damage[y/32]&(1u<<(y%32))))y++;
        desk_client_damage(&d,client,(struct desk_box){content.x,content.y+(int)first,640,(int)(y-first)});
    }
}
static int window_client(int fd,int surface,uint32_t w,uint32_t h,uint32_t colour)
{
    fake_channel(fd);fake_surface(surface,w,h,colour);
    int client=desk_add_client(&d,fd);assert(client>=0);
    struct ciuki_message m;desk_message(&m,DESK_HELLO,4);m.sender_pid=(uint32_t)(42+fd);
    desk_receive(&d,client,&m);
    desk_message(&m,DESK_CREATE_WINDOW,20);m.sender_pid=(uint32_t)(42+fd);m.fd_count=1;m.fds[0]=surface;
    desk_put32(m.data+4,w);desk_put32(m.data+8,h);desk_put32(m.data+12,4);
    memcpy(m.data+16,"Test",4);desk_receive(&d,client,&m);
    assert(d.clients[client].window);
    return client;
}
static int presented_reply(int client,uint32_t serial,int32_t *result)
{
    struct desk_client *c=&d.clients[client];
    for (unsigned k=0;k<c->tx_count;k++) {
        const struct ciuki_message *m=&c->tx[(c->tx_head+k)%DESK_TX_MAX];
        struct desk_packet p;
        if (m->data[1]==DESK_GFX_PRESENTED && desk_parse(m,0,NULL,&p) && p.serial==serial) { *result=p.result; return 1; }
    }
    return 0;
}
static void composed_frame(int client,uint32_t serial)
{
    struct ciuki_message m;desk_message(&m,DESK_GFX_FRAME,64);m.sender_pid=(uint32_t)(42+10);
    desk_put32(m.data+4,serial);desk_receive(&d,client,&m);
}
static void direct_replay(void)
{
    fake_reset();struct desk_ops ops=fake_ops;ops.present=upload;
    desk_init(&d,&ops,1024,768);d.display=7;
    d.pixels=calloc((size_t)d.width*d.height,4);
    scanout=calloc((size_t)d.width*d.height,4);assert(d.pixels && scanout);
    d.accel=engine;desk_accel_reset(&d,55);
    int client=window_client(10,20,640,480,4);
    d.clients[client].dosbox=1;              /* desktop.c: a launched "dosbox" */
    desk_move(&d,client,100,100);
    client_surface=d.clients[client].pixels;
    struct dosbox_window w={0};dosbox_profile_default(&w.profile,"TEST.EXE");w.profile.zoom=0;
    uint32_t *guest=calloc(640*480,4);assert(guest);
    struct du_canvas canvas={.pixels=(uint32_t *)d.clients[client].pixels,.width=640,.height=480,.clip={0,0,640,480}};
    db_paint(&w,&canvas,guest);assert(!desk_redraw(&d));check_frame();
    /* 1. Unobscured: presented straight from the surface. */
    uint64_t direct_requests=0, direct_px=0;
    for (unsigned frame=1;frame<=4;frame++) {
        damage_frame(&w,&canvas,guest,client,frame);
        uint64_t calls=requests; direct_uploads=0;
        if (getenv("DIRECT_DEBUG")) fprintf(stderr,"damage=%u move=%u present=%u copy=%d frame=%d scanout=%d restore=%d hw=%d shown=%d valid=%d dirty=%d box=%d,%d,%d,%d\n",
            d.damage.count,d.move_damage.count,d.present_damage.count,d.copy_pending,d.frame_valid,d.scanout_valid,d.restore_cpu_frame,
            d.hw_cursor,d.hw_cursor_shown,d.cursor_valid,d.cursor_dirty,d.damage.boxes[0].x,d.damage.boxes[0].y,d.damage.boxes[0].w,d.damage.boxes[0].h);
        assert(!desk_redraw(&d));check_frame();
        assert(!d.frame_stats.scene_px && d.frame_stats.direct_px==direct_uploads && direct_uploads>=300u*640u);
        direct_requests+=requests-calls; direct_px+=direct_uploads;
        assert(d.direct_stale.count);
    }
    assert(direct_requests==4);              /* one damaged run of rows a frame (Fit: 377) */
    /* 2. The composed acknowledgement: answered after the frame is shown. */
    damage_frame(&w,&canvas,guest,client,5);
    composed_frame(client,7); int32_t result=1;
    assert(d.clients[client].gfx_pending && !presented_reply(client,7,&result));
    assert(!desk_redraw(&d));check_frame();
    assert(!d.clients[client].gfx_pending && presented_reply(client,7,&result) && result==0);
    desk_flush(&d,client);
    composed_frame(client,8);                /* nothing pending: answered at once */
    assert(presented_reply(client,8,&result) && result==-EAGAIN);
    desk_flush(&d,client);
    /* 3. A window over part of it: composed, the older pixels first. */
    int other=window_client(11,21,200,150,0x00ff00u);
    desk_move(&d,other,300,250);
    assert(!desk_redraw(&d));check_frame();
    damage_frame(&w,&canvas,guest,client,6);
    direct_uploads=0;
    assert(!desk_redraw(&d));check_frame();
    assert(!direct_uploads && d.frame_stats.scene_px && !d.direct_stale.count);
    check_settled();
    /* Closed again: direct presents resume. */
    desk_drop_client(&d,other);
    assert(!desk_redraw(&d));check_frame();
    damage_frame(&w,&canvas,guest,client,7);
    direct_uploads=0; assert(!desk_redraw(&d));check_frame(); assert(direct_uploads==direct_px/4);
    /* 4. Moved after direct frames: the move copies and repaints exactly
     * (the older RAM pixels are recomposed first). */
    damage_frame(&w,&canvas,guest,client,9);
    assert(!desk_redraw(&d));check_frame(); assert(d.direct_stale.count);
    desk_move(&d,client,140,120);
    assert(!desk_redraw(&d));check_frame(); assert(!d.direct_stale.count);
    check_settled();
    damage_frame(&w,&canvas,guest,client,10);
    direct_uploads=0; assert(!desk_redraw(&d));check_frame(); assert(direct_uploads);
    /* 5. A capture (Print Screen) composes the older pixels first. */
    check_settled();
    damage_frame(&w,&canvas,guest,client,8);
    assert(!desk_redraw(&d));check_frame();
    /* 6. A non-DOS client may not use the scene-less form. */
    int plain=window_client(12,22,100,80,0x0000ffu);
    desk_move(&d,plain,880,640);            /* clear of the DOS window */
    struct ciuki_message m;desk_message(&m,DESK_GFX_FRAME,64);m.sender_pid=42+12;
    desk_put32(m.data+4,9);desk_receive(&d,plain,&m);
    assert(presented_reply(plain,9,&result) && result==-EPERM);
    assert(!desk_redraw(&d));check_frame();check_settled();
    /* Review 2026-10-27 03:14, P2-1: A shown, B presented directly (the RAM
     * frame keeps A), back to A: not an unchanged frame. */
    damage_frame(&w,&canvas,guest,client,11);assert(!desk_redraw(&d));check_frame();check_settled();
    damage_frame(&w,&canvas,guest,client,12);assert(!desk_redraw(&d));check_frame();assert(d.direct_stale.count);
    damage_frame(&w,&canvas,guest,client,11);
    assert(d.damage.count);                  /* the change back to A is damage */
    assert(!desk_redraw(&d));check_frame();
    check_settled();
    /* P2-2: a scroll plan whose translated source lies in stale pixels. Rows
     * 150..199 take the RAM frame's older pixels of rows 200..249, which the
     * scanout no longer shows; another window's change keeps the frame
     * composed without touching the stale rows. */
    uint32_t *surface=(uint32_t *)d.clients[client].pixels;
    struct desk_box content=desk_content(&d.clients[client]);
    for (unsigned r=0;r<480;r++) for (unsigned x=0;x<640;x++) surface[r*640+x]=(r*1024u+x)&0x7fffffu;
    desk_client_damage(&d,client,content);assert(!desk_redraw(&d));check_frame();check_settled();
    for (unsigned r=200;r<300;r++) for (unsigned x=0;x<640;x++) surface[r*640+x]=0x800000u|((r*1024u+x)&0x7fffffu);
    desk_client_damage(&d,client,(struct desk_box){content.x,content.y+200,640,100});
    assert(!desk_redraw(&d));check_frame();assert(d.direct_stale.count);
    for (unsigned r=150;r<200;r++) for (unsigned x=0;x<640;x++) surface[r*640+x]=((r+50)*1024u+x)&0x7fffffu;
    desk_client_damage(&d,client,(struct desk_box){content.x,content.y+150,640,50});
    uint32_t *plain_pixels=(uint32_t *)d.clients[plain].pixels; plain_pixels[0]^=0x00ff00u;
    struct desk_box plain_content=desk_content(&d.clients[plain]);
    desk_client_damage(&d,plain,(struct desk_box){plain_content.x,plain_content.y,1,1});
    uint64_t copies=copied;
    assert(!desk_redraw(&d));check_frame();check_settled();
    assert(copied==copies);                  /* no scroll copy from the scanout */
    /* P2-3: Print Screen between a move and its frame resolves the move
     * first (its copy source is the RAM frame of the old geometry). */
    damage_frame(&w,&canvas,guest,client,13);assert(!desk_redraw(&d));check_frame();assert(d.direct_stale.count);
    desk_move(&d,client,120,110);
    desk_direct_settle(&d);                  /* what desk_clip_capture does first */
    assert(!d.copy_pending && !d.direct_stale.count);
    check_composed();                        /* the capture's source is the composed screen */
    assert(!desk_redraw(&d));check_frame();check_composed();
    printf("dos-window-direct frames=4 requests_per_frame=%llu direct_px_per_frame=%llu scene_px=0 ack=presented/eagain/eperm overlap=composed move=exact capture=settled review=a-b-a/scroll-source/capture-move\n",
           (unsigned long long)(direct_requests/4),(unsigned long long)(direct_px/4));
    free(guest);for(int i=0;i<DESK_CLIENTS;i++)desk_drop_client(&d,i);
    desk_render_release(&d);free(d.sh.icons);free(d.sh.wall);free(d.pixels);free(scanout);
    assert(!fake_live());
}
int main(void)
{
    direct_replay();
    for(unsigned title=0;title<3;title++)for(unsigned zoom=0;zoom<3;zoom++)replay(title,zoom);
    puts("PASS fx-doswin3 viewport/compositor/COPY replay, full RAM and scanout oracle, unchanged frames have zero requests");
    return 0;
}
