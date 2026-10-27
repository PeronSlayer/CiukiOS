/* Final visible frames from production desktop, software and accelerated.
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include "../../../assets/cursors/paint.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct desktop d;
static unsigned shell_paints;
void desk_test_profile(const char *name) { if (!strcmp(name,"bars_menus")) shell_paints++; }

static uint32_t *scanout;
static unsigned caps, copies, window_copies, fills, uploads, cursor_moves;
static uint64_t uploaded_pixels;
static uint32_t expected_generation=55;
static unsigned full_uploads;
static int query_failure;
static int display_info(int display,struct ciuki_framebuffer_info *out)
{
    assert(display==7);
    if (query_failure) { errno=EBUSY; return -1; }
    *out=(struct ciuki_framebuffer_info){.size=sizeof(*out),.width=d.width,.height=d.height,
        .bpp=32,.generation=expected_generation};
    return 0;
}
static int fail_copy, fail_cursor, fail_cursor_set, fail_fill, shown, cursor_x, cursor_y;
/* f2-92: engine-only rows below the scanout ([vram_first, vram_end), the
 * scanout buffer extends over them) and host-to-screen writes. */
static uint32_t vram_first, vram_end, cache_epoch;
static unsigned writes, cache_copies, fail_write;
static uint64_t written_pixels, engine_copied_pixels;
static struct ciuki_accel_request cursor;
static unsigned stretches;
static int stretch_fail;
static int stretch_info(int fd,uint32_t generation,struct ciuki_command_info *out)
{
    assert(fd==7 && generation==55);
    *out=(struct ciuki_command_info){.size=sizeof(*out),.version=1,
        .native_commands=caps ? 1u<<CIUKI_CMD_SCALED : 0,
        .capabilities=caps ? CIUKI_CAP_BATCH|CIUKI_CAP_SCALED|CIUKI_CAP_STRETCH : 0};
    return 0;
}
static int stretch_batch(int fd,const void *wire,uint32_t bytes,struct ciuki_batch_reply *reply)
{
    assert(fd==7 && bytes==224);
    const struct ciuki_batch_header *h=wire;
    const struct ciuki_batch_record *r=(const void *)((const uint8_t *)wire+h->records_offset);
    assert(h->record_count==1 && h->target_count==1 && r->opcode==CIUKI_CMD_SCALED &&
        r->filter==CIUKI_FILTER_NEAREST_EDGE && r->sw==r->dw && r->sh==1 && r->src==1 && r->dst==1);
    for (unsigned y=0;y<r->dh;y++) memcpy(scanout+((unsigned)r->dy+y)*d.width+(unsigned)r->dx,
        scanout+(unsigned)r->sy*d.width+(unsigned)r->sx,r->dw*4);
    stretches++;
    *reply=(struct ciuki_batch_reply){.size=64,.version=1,.state=CIUKI_FENCE_SUCCEEDED,.flags=CIUKI_FENCE_CPU_SAFE};
    if (stretch_fail) { stretch_fail=0; caps=0; shown=0; return -EIO; }
    return 0;
}
static int stretch_fence(int fd,uint64_t id,uint16_t action,uint32_t timeout,struct ciuki_batch_reply *reply)
{ (void)fd;(void)id;(void)action;(void)timeout;(void)reply;assert(0);return -EIO; }
static int upload(int display,int surface,const struct ciuki_rect *r)
{
    (void)display; (void)surface;
    for (uint32_t y=0;y<r->height;y++)
        memcpy(scanout+((uint32_t)r->dst_y+y)*d.width+(uint32_t)r->dst_x,
               d.pixels+((uint32_t)r->src_y+y)*d.width+(uint32_t)r->src_x,r->width*4);
    uploads++; uploaded_pixels+=(uint64_t)r->width*r->height;
    if (!r->dst_x && !r->dst_y && r->width==d.width && r->height==d.height) full_uploads++;
    return 0;
}
static void completed_ram_frame(void)
{
    uint32_t *expected=calloc((size_t)d.width*d.height,4); assert(expected);
    struct desktop *oracle=malloc(sizeof(d)); assert(oracle); *oracle=d;
    oracle->pixels=expected;
    desk_composite(oracle,(struct desk_box){0,0,(int)d.width,(int)d.height});
    if (memcmp(d.pixels,expected,(size_t)d.width*d.height*4))
        for (unsigned p=0;p<d.width*d.height;p++) if (d.pixels[p]!=expected[p]) {
            fprintf(stderr,"RAM mismatch at %u,%u got=%x expected=%x copy=%d client=%d\n",
                p%d.width,p/d.width,d.pixels[p],expected[p],d.copy_pending,d.copy_client);break;
        }
    assert(!memcmp(d.pixels,expected,(size_t)d.width*d.height*4));
    free(oracle); free(expected);
}
static int service(int display,const struct ciuki_accel_request *q)
{
    assert(display==7 && q->size==sizeof(*q) && q->generation==expected_generation);
    if (q->operation==CIUKI_ACCEL_CACHE_EPOCH) return (int)cache_epoch;
    if (q->operation==CIUKI_ACCEL_QUERY) return caps;
    if (q->operation==CIUKI_ACCEL_OFFSCREEN) return (caps&1u) && vram_end ? (int)(vram_end<<16|vram_first) : 0;
    if (q->operation==CIUKI_ACCEL_WRITE) {
        /* Uploads target engine-only rows only, never the visible frame. */
        assert(caps&1u && vram_end && q->rect.dst_y>=(int32_t)vram_first &&
               q->rect.dst_y+(int64_t)q->rect.height<=(int64_t)vram_end && q->rect.dst_x>=0 &&
               q->rect.dst_x+(int64_t)q->rect.width<=(int64_t)d.width && !q->write.clip_width);
        if (fail_write) { fail_write=0; caps=0; shown=0; return -EIO; }
        const uint32_t *src=(const uint32_t *)(uintptr_t)((uint64_t)q->write.pixels_hi<<32|q->write.pixels_lo);
        assert(q->rect.src_y>=0 && q->rect.src_y+(int64_t)q->rect.height<=(int64_t)q->write.source_height &&
               q->rect.width<=q->write.source_width && q->write.stride>=q->write.source_width*4);
        for (uint32_t y=0;y<q->rect.height;y++)
            memcpy(scanout+((uint32_t)q->rect.dst_y+y)*d.width+(uint32_t)q->rect.dst_x,
                   (const uint8_t *)src+((uint32_t)q->rect.src_y+y)*(size_t)q->write.stride+(uint32_t)q->rect.src_x*4,q->rect.width*4);
        writes++; written_pixels+=(uint64_t)q->rect.width*q->rect.height;
        return 0;
    }
    if (q->operation==CIUKI_ACCEL_COPY || q->operation==CIUKI_ACCEL_FILL) completed_ram_frame();
    if (q->operation==CIUKI_ACCEL_COPY) {
        copies++;
        engine_copied_pixels+=(uint64_t)q->rect.width*q->rect.height;
        if (q->rect.src_y>=(int32_t)d.height) cache_copies++;
        else if (q->rect.height>1) window_copies++;
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
    } else if (q->operation==CIUKI_ACCEL_CURSOR_SET) {
        if (fail_cursor_set) { caps=0; shown=0; fail_cursor_set=0; return -EIO; }
        cursor=*q;
    }
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
    oracle->pixels=expected; oracle->hw_cursor=shown;
    desk_composite(oracle,(struct desk_box){0,0,(int32_t)d.width,(int32_t)d.height});
    /* Derive expected pixels from the pinned source artwork, independently
     * of the mask submitted to the presenter. Mono hardware has no shadow. */
    if (shown) {
        /* G25's accessibility pointer (large/inverted) selects the artwork; G12's Mouse > Pointers scheme
         * (0 Ciuki, 1 large, 2 inverted) is applied to it the way compositor.c cursor_raster() does. */
        struct desk_tool_cursor art=desk_tool_cursor_scheme(d.cursor_shape,d.sh.cfg.access.pointer);
        const struct desk_cursor_raster *source=desk_generic_cursor(d.cursor_shape);
        unsigned width=32,height=32; int hx=art.hot_x,hy=art.hot_y;
        if(d.shell && d.sh.cfg.pointer_scheme==1 && source) {
            width=source->width*3/2; if(width>32) width=32;
            height=source->height*3/2; if(height>32) height=32;
            hx=(int)(source->hot_x*width/source->width); hy=(int)(source->hot_y*height/source->height);
        }
        for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++) {
            unsigned sx=x,sy=y;
            if(d.shell && d.sh.cfg.pointer_scheme==1 && source) { sx=x*source->width/width; sy=y*source->height/height; }
            int px=d.mouse_x-hx+(int)x,py=d.mouse_y-hy+(int)y;
            uint32_t bit=0x80000000u>>sx;
            if(px<0 || py<0 || (unsigned)px>=d.width || (unsigned)py>=d.height || !(art.mask[sy]&bit)) continue;
            int white=!!(art.image[sy]&bit);
            if(d.shell && d.sh.cfg.pointer_scheme==2) white=!white;
            expected[(unsigned)py*d.width+(unsigned)px]=white ? 0xffffffu : 0;
        }
    }
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
    } else {
        /* Software motion restores the old footprint and uploads the new
         * one; the selected theme supplies the arrow's occupied bounds. */
        const struct desk_cursor_raster *arrow=&desk_cursors[DESK_CURSOR_ARROW];
        assert(d.frame_stats.damage_px<=2ull*arrow->width*arrow->height);
    }
    /* Every shape and hot spot must match software, including clipping at
     * all four screen corners and restoring the previous larger footprint. */
    for (unsigned shape=0;shape<DESK_CURSOR_COUNT;shape++) for (unsigned corner=0;corner<4;corner++) {
        d.cursor_shape=shape; d.cursor_dirty=1;
        d.mouse_x=(corner&1) ? 639 : 0; d.mouse_y=(corner&2) ? 479 : 0;
        redraw();
        if (available&2u) assert(d.cursor_loaded==shape);
    }
    if (available&2u) {
        fail_cursor_set=1; d.cursor_shape=DESK_CURSOR_TEXT; d.cursor_dirty=1; redraw();
        assert(!d.hw_cursor && !caps);
        caps=available; desk_accel_reset(&d,55); redraw();
    }
    d.cursor_shape=DESK_CURSOR_ARROW; d.cursor_dirty=1; d.mouse_x=333; d.mouse_y=245; redraw();
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
    /* F2-74: disjoint client writes, scrolls and moves keep their shortcuts.
     * Both copy endpoints, shadows and the entry sweep remain protected. */
    desk_shell_enable(&d);
    desk_move(&d,i,50,240); redraw();
    desk_shell_notify(&d,"Removable media","E: is ready to use.",DU_DLG_ICON_INFO,SH_NOTIFY_MEDIA);
    struct desk_box sweep=desk_shell_notification_bounds(&d);
    assert(d.sh.notification.slide==20);
    redraw();
    d.now_ns+=200000000ull; desk_shell_tick(&d); redraw();
    struct desk_box settled=desk_shell_notification_bounds(&d);
    assert(!memcmp(&sweep,&settled,sizeof(sweep)));
    struct desk_box content=desk_content(&d.clients[i]);
    shell_paints=0;
    desk_composite(&d,(struct desk_box){content.x+2,content.y+2,20,10});
    assert(!shell_paints); /* precise client-only painter is still selected */
    before_copies=window_copies;
    memmove(pixels+5*width,pixels,(height-5)*width*4);
    for (unsigned p=0;p<5*width;p++) pixels[p]=(p*3871u)&0xffffff;
    dirty_client(i);redraw();
    if (available&2u) assert(window_copies==before_copies+1);
    before_copies=window_copies;
    desk_move(&d,i,60,260);assert(d.copy_pending);redraw();
    if (available&2u) assert(window_copies==before_copies+1);
    /* Only the destination intersects. Then only the source intersects. */
    desk_move(&d,i,settled.x+10,settled.y+10);assert(d.copy_pending);redraw();
    shell_paints=0;
    content=desk_content(&d.clients[i]);
    desk_composite(&d,(struct desk_box){content.x+2,content.y+2,20,10});
    assert(shell_paints);
    before_copies=window_copies;
    memmove(pixels+5*width,pixels,(height-5)*width*4);
    for (unsigned p=0;p<5*width;p++) pixels[p]=(p*5939u)&0xffffff;
    dirty_client(i);redraw();
    /* Its entire small client can lie in the protected shadow/entry sweep;
     * the full-frame oracle also checks the no-safe-source fallback. */
    desk_move(&d,i,60,260);assert(d.copy_pending);redraw();
    /* A frame whose rightmost client column touches only the left shadow. */
    int shadow_x=settled.x-(int)width-DESK_BORDER;
    desk_move(&d,i,shadow_x,settled.y);assert(d.copy_pending);redraw();
    desk_move(&d,i,60,260);assert(d.copy_pending);redraw();
    /* The lower, clipped entry sweep is protected too. */
    assert(desk_notification_intersects(&d,(struct desk_box){settled.x,settled.y+settled.h-1,1,1}));
    assert(!desk_notification_intersects(&d,(struct desk_box){settled.x-1,settled.y,1,1}));
    /* Coalescing a safe move must check the retained original source. */
    desk_move(&d,i,65,265);assert(d.copy_pending);
    desk_move(&d,i,70,270);assert(d.copy_pending);redraw();
    desk_shell_key(&d,DU_KEY_ESC,1);redraw();
    d.shell=0;desk_damage_add(&d.damage,(struct desk_box){0,0,640,480},640,480);redraw();
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
/* f2-92: the offscreen caches (wallpaper/icon background and window
 * content) behind moves, raises, menus and closes: every completed frame
 * identical to the software oracle, rows really served from the caches. */
static void cache_steps(uint64_t *now)
{
    for (unsigned k=0;k<200;k++) { *now+=100000000ull; desk_accel_cache_step(&d,*now); }
}
static void cache_scenario(void)
{
    fake_reset();struct desk_ops ops=fake_ops;ops.present=upload;
    desk_init(&d,&ops,640,480);d.display=7;
    vram_first=480; vram_end=1300;
    d.pixels=calloc((size_t)d.width*d.height,4);scanout=calloc((size_t)d.width*vram_end,4);assert(d.pixels && scanout);
    caps=3;copies=window_copies=fills=uploads=cursor_moves=writes=cache_copies=0;uploaded_pixels=written_pixels=0;
    shown=fail_copy=fail_cursor=fail_fill=fail_write=0;
    d.accel=service;desk_accel_reset(&d,55);
    assert(d.vram_first==480 && d.vram_end==1300);
    desk_shell_enable(&d);
    snprintf(d.sh.cfg.wallpaper,sizeof(d.sh.cfg.wallpaper),"builtin:%s",desk_builtin_wallpapers[0].id);
    desk_shell_apply(&d); assert(!desk_shell_wallpaper_update(&d));
    int a=window_new(10,20,200,150,40,60), b=window_new(11,21,180,120,300,200);
    redraw();
    uint64_t now=1000000000ull; cache_steps(&now);
    assert(writes>0 && d.vram[0].valid && d.vram[0].kind==DESK_VRAM_BACKGROUND);
    unsigned windows_cached=0;
    for (int k=1;k<DESK_VRAM_ENTRIES;k++) if (d.vram[k].valid && d.vram[k].kind==DESK_VRAM_WINDOW) windows_cached++;
    assert(windows_cached==2);
    /* 3D overwrites and returns exactly the same row range. Cached RAM
     * shadows alone cannot prove those pixels: the new epoch drops them. */
    cache_epoch++;
    memset(scanout+(size_t)d.width*vram_first,0xcc,(size_t)d.width*(vram_end-vram_first)*4);
    desk_move(&d,a,42,62); redraw();
    assert(d.vram_epoch==cache_epoch);
    for (unsigned i=0;i<DESK_VRAM_ENTRIES;i++) assert(!d.vram[i].valid);
    cache_steps(&now); assert(d.vram[0].valid);
    unsigned before=cache_copies;
    /* Moves expose wallpaper and the other window; raises expose content. */
    for (int k=0;k<6;k++) { desk_move(&d,a,40+k*23,60+k*11); redraw(); }
    desk_focus(&d,b); redraw(); desk_focus(&d,a); redraw();
    desk_move(&d,b,250,150); redraw();
    desk_menu_open(&d,SH_MENU_START,DESK_START_X,DESK_BAR); redraw();
    desk_menu_close(&d); redraw();
    unsigned during=cache_copies;
    assert(during>before && d.stats.cache_rows>0);
    /* Client content changes: only the exact comparison decides; the
     * changed window is re-uploaded after it is stable again. */
    uint32_t *pixels=(uint32_t *)d.clients[a].pixels;
    for (unsigned p=0;p<200*150;p++) pixels[p]=(p*2654435761u)&0xffffff;
    dirty_client(a); redraw();
    desk_move(&d,b,60,80); redraw(); desk_move(&d,b,300,220); redraw();
    unsigned written=writes; cache_steps(&now); assert(writes>written);
    desk_focus(&d,b); redraw(); desk_focus(&d,a); redraw();
    /* An icon selection changes the background; its cache restarts. */
    uint32_t serial=d.sh.background_serial;
    d.sh.icon_selected=0; desk_damage_add(&d.damage,(struct desk_box){0,0,640,480},640,480); redraw();
    /* A stale serial is never used (identical frame above); the next steps
     * restart the upload from the new background. */
    unsigned restarted=writes;
    cache_steps(&now); assert(d.sh.background_serial==serial || (writes>restarted && d.vram[0].serial==d.sh.background_serial));
    desk_move(&d,a,120,90); redraw();
    /* Closing a window exposes everything underneath. */
    desk_drop_client(&d,b); redraw();
    unsigned after=cache_copies;
    /* An upload failure: quarantined engine, software frames, no cache. */
    pixels[0]^=0x10101; dirty_client(a); redraw();
    now+=600000000ull; desk_accel_cache_step(&d,now); now+=600000000ull;
    fail_write=1; for (unsigned k=0;k<4 && fail_write;k++) { now+=600000000ull; desk_accel_cache_step(&d,now); }
    desk_move(&d,a,200,200); redraw(); redraw();
    assert(!d.accel_caps && !d.vram_end && d.accel_lost);
    printf("f2-92 desktop caches: frames identical, uploads=%u written_pixels=%llu cache_copies=%u->%u->%u cache_rows=%llu window_copies=%u fills=%u cpu_pixels=%llu\n",
        writes,(unsigned long long)written_pixels,before,during,after,(unsigned long long)d.stats.cache_rows,
        window_copies,fills,(unsigned long long)uploaded_pixels);
    for (int k=0;k<DESK_CLIENTS;k++) desk_drop_client(&d,k);
    desk_render_release(&d);free(d.pixels);free(scanout);vram_first=vram_end=0;assert(!fake_live());
}
/* f2-92 item 4: one 640x480 window dragged 40 steps of (8,6) over the
 * wallpaper at 1024x768x32, software presenter versus the engine model
 * (screen copies, offscreen caches). CPU bytes are the LFB uploads; the
 * time model uses the historical T23 upload rate of 134 MB/s
 * (docs/validation/f4-20-gfxdemo-frame-budget.md). */
static void move_measurement(unsigned available)
{
    fake_reset();struct desk_ops ops=fake_ops;ops.present=upload;
    desk_init(&d,&ops,1024,768);d.display=7;
    vram_first=available ? 768 : 0; vram_end=available ? 2048 : 0;
    d.pixels=calloc((size_t)d.width*d.height,4);scanout=calloc((size_t)d.width*(available ? vram_end : 768),4);assert(d.pixels && scanout);
    caps=available;copies=window_copies=fills=uploads=cursor_moves=writes=cache_copies=0;
    uploaded_pixels=written_pixels=engine_copied_pixels=0;shown=fail_copy=fail_cursor=fail_fill=fail_write=0;
    if (available) { d.accel=service;desk_accel_reset(&d,55); }
    desk_shell_enable(&d);
    snprintf(d.sh.cfg.wallpaper,sizeof(d.sh.cfg.wallpaper),"builtin:%s",desk_builtin_wallpapers[0].id);
    desk_shell_apply(&d); assert(!desk_shell_wallpaper_update(&d));
    int i=window_new(10,20,640,480,100,120);
    d.mouse_x=1000; d.mouse_y=700; redraw();
    uint64_t now=1000000000ull;
    if (available) cache_steps(&now);
    uint64_t setup_written=written_pixels;
    uint64_t base_pixels=uploaded_pixels; unsigned base_copies=copies, base_fills=fills;
    for (int k=1;k<=40;k++) { desk_move(&d,i,100+k*8,120+k*6); redraw(); }
    uint64_t cpu=(uploaded_pixels-base_pixels)*4;
    printf("f2-92 window move 1024x768x32 %s: 40 steps cpu_bytes=%llu (%.1f MB) model_upload_ms=%.1f per_step_ms=%.2f engine_copies=%u (window=%u cache=%u) engine_fills=%u engine_pixels=%llu setup_writes=%llu px\n",
        available ? "engine" : "software",(unsigned long long)cpu,cpu/1e6,cpu/134e6*1e3,cpu/134e6*1e3/40,
        copies-base_copies,window_copies,cache_copies,fills-base_fills,(unsigned long long)engine_copied_pixels,(unsigned long long)setup_written);
    static uint64_t software_bytes;
    if (!available) software_bytes=cpu;
    else assert(software_bytes && cpu*4<software_bytes && window_copies>=30 && cache_copies>0);
    for (int k=0;k<DESK_CLIENTS;k++) desk_drop_client(&d,k);
    desk_render_release(&d);free(d.pixels);free(scanout);vram_first=vram_end=0;assert(!fake_live());
}
/* f2-92: on the T23 the engine became ready after the desktop's first
 * frame with an unchanged mode generation; the hardware cursor, offscreen
 * rows and therefore window-move copies must follow (5a10eef4 had none). */
static void late_cursor_ready(void)
{
    fake_reset();struct desk_ops ops=fake_ops;ops.present=upload;
    desk_init(&d,&ops,640,480);d.display=7;
    vram_first=480; vram_end=900;
    d.pixels=calloc((size_t)d.width*d.height,4);scanout=calloc((size_t)d.width*vram_end,4);assert(d.pixels && scanout);
    caps=0;copies=window_copies=fills=uploads=cursor_moves=writes=cache_copies=0;uploaded_pixels=0;
    shown=fail_copy=fail_cursor=fail_fill=fail_write=0;
    d.accel=service;desk_accel_reset(&d,55);
    int i=window_new(10,20,120,80,50,80);redraw();
    assert(!d.hw_cursor && !d.accel_caps && !d.vram_end);
    caps=3; dirty_client(i); redraw();
    assert(d.hw_cursor && d.accel_caps==3 && d.vram_first==480 && d.vram_end==900 && d.scanout_valid);
    unsigned before=window_copies;
    desk_move(&d,i,80,95);redraw();desk_move(&d,i,90,100);redraw();
    assert(window_copies>=before+1);
    puts("PASS f2-92 late activation: same-generation caps 0->3 loads the hardware cursor and offscreen rows, window moves use engine copies");
    for (int k=0;k<DESK_CLIENTS;k++) desk_drop_client(&d,k);
    desk_render_release(&d);free(d.pixels);free(scanout);vram_first=vram_end=0;assert(!fake_live());
}
static void foreign_regions(void)
{
    fake_reset();struct desk_ops ops=fake_ops;ops.present=upload;
    desk_init(&d,&ops,640,480);d.display=7;
    d.pixels=calloc((size_t)d.width*d.height,4);scanout=calloc((size_t)d.width*d.height,4);
    assert(d.pixels && scanout);caps=3;shown=0;fail_copy=fail_fill=fail_cursor=fail_cursor_set=0;
    copies=window_copies=fills=uploads=0;d.accel=service;desk_accel_reset(&d,55);
    int low=window_new(10,20,400,260,40,70);
    int high=window_new(11,21,180,120,130,150);redraw();
    struct desk_box content=desk_content(&d.clients[low]);
    struct desk_box covered={160,200,32,8};
    uint32_t *p=(uint32_t *)d.clients[low].pixels;
    for (int y=covered.y;y<covered.y+covered.h;y++) for (int x=covered.x;x<covered.x+covered.w;x++)
        p[(unsigned)(y-content.y)*400+(unsigned)(x-content.x)]=0x765432;
    assert(!desk_client_damage(&d,low,covered) && !d.damage.count);
    uint64_t frames=d.stats.frames, presents=d.presents;
    assert(!desk_redraw(&d) && d.stats.frames==frames && d.presents==presents);
    /* Partial coverage is clipped before either the scene or presenter. */
    assert(desk_client_damage(&d,low,content));
    uint64_t area=0;for (unsigned k=0;k<d.damage.count;k++) area+=(uint64_t)d.damage.boxes[k].w*d.damage.boxes[k].h;
    assert(area<(uint64_t)content.w*content.h);redraw();
    unsigned before=window_copies;
    const int moves[][2]={{48,76},{32,68},{48,60},{32,76},{40,70}};
    for (unsigned k=0;k<sizeof(moves)/sizeof(moves[0]);k++) {
        desk_move(&d,low,moves[k][0],moves[k][1]);assert(d.copy_pending);redraw();
    }
    assert(window_copies>before);
    /* Scroll beneath the covering window, both directions. */
    content=desk_content(&d.clients[low]);before=window_copies;
    memmove(p,p+7*400,(260-7)*400*4);
    for (unsigned n=(260-7)*400;n<260*400;n++)p[n]=(n*7631u)&0xffffff;
    desk_client_damage(&d,low,content);redraw();assert(window_copies>before);
    for(int direction=-1;direction<=1;direction+=2) {
        before=window_copies;
        for(unsigned y=0;y<260;y++) {
            uint32_t *row=p+y*400;
            if(direction<0) {
                memmove(row,row+9,(400-9)*4);
                for(unsigned x=400-9;x<400;x++)row[x]=(y*92761u+x*7631u)&0xffffff;
            } else {
                memmove(row+9,row,(400-9)*4);
                for(unsigned x=0;x<9;x++)row[x]=(y*3871u+x*8271u)&0xffffff;
            }
        }
        desk_client_damage(&d,low,content);redraw();assert(window_copies>before);
    }
    desk_shell_enable(&d);desk_menu_open(&d,SH_MENU_START,0,0);redraw();
    struct sh_menu *menu=&d.sh.menus[0];menu->hot=5;   /* Help to Run...: neither row opens a submenu (Settings and Find do) */
    desk_damage_add(&d.damage,menu->box,d.width,d.height);redraw();
    d.mouse_x=menu->box.x+40;d.mouse_y=menu->box.y+3+(menu->scroll_max ? 12 : 0)+6*menu->row_h+2;
    desk_cursor_motion(&d);
    desk_shell_motion(&d);area=0;
    for(unsigned k=0;k<d.damage.count;k++)area+=(uint64_t)d.damage.boxes[k].w*d.damage.boxes[k].h;
    assert(menu->hot==6 && area<=2u*(unsigned)(menu->box.w-4)*(unsigned)menu->row_h);redraw();
    struct desk_box under_menu={menu->box.x+60,menu->box.y+100,8,4};
    assert(!desk_client_damage(&d,low,under_menu) && !d.damage.count);
    desk_menu_close(&d);redraw();
    /* A z-order change in the same batch cannot reuse stale copy sources. */
    desk_move(&d,low,48,76);desk_focus(&d,low);redraw();
    content=desk_content(&d.clients[low]);
    before=window_copies;
    memmove(p+5*400,p,(260-5)*400*4);
    for (unsigned n=0;n<5*400;n++)p[n]=(n*3871u)&0xffffff;
    desk_client_damage(&d,low,content);redraw();assert(window_copies>before);
    /* Exposure reconstructs even changes that arrived while covered. */
    desk_drop_client(&d,high);redraw();
    for (int k=0;k<DESK_CLIENTS;k++)desk_drop_client(&d,k);
    desk_render_release(&d);free(d.sh.icons);free(d.sh.wall);free(d.pixels);free(scanout);assert(!fake_live());
    puts("PASS F2-98b foreign regions: hidden damage has zero writes, clipped moves in four directions, clipped scrolls and exposure match the full-frame oracle");
}
/* A 320x200 game expanded into a 640x480 window, with the actual dosbox
 * Fit row mapping. The display driver COPY transport must match the full
 * software composition, including the very first frame and recovery. */
static void dos_video_frame(unsigned available,unsigned zoom)
{
    fake_reset(); struct desk_ops ops=fake_ops; ops.present=upload;
    desk_init(&d,&ops,800,600); d.display=7;
    d.pixels=calloc((size_t)d.width*d.height,4); scanout=calloc((size_t)d.width*d.height,4);
    assert(d.pixels && scanout);
    caps=!!available; copies=window_copies=fills=uploads=cursor_moves=stretches=0;
    uploaded_pixels=0; shown=fail_copy=fail_cursor=fail_fill=0;
    if (available) {
        if (available==2) { d.commands=stretch_info; d.batch=stretch_batch; d.fence=stretch_fence; }
        d.accel=service; desk_accel_reset(&d,55);
    }
    int i=window_new(10,20,640,480,40,40);
    uint32_t *pixels=(uint32_t *)d.clients[i].pixels;
    for (unsigned y=0;y<480;y++) for (unsigned x=0;x<640;x++) {
        unsigned sy=y<28 ? 0 : zoom==0 ? (y-28)*480/452 : zoom==2 ? (y-28)*100/125 : y-28;
        unsigned sx=zoom==2 ? x*100/125 : x;
        pixels[y*640+x]=y<28 || sy<40 || sy>=440 ? 0 :
            (0x203040u+((sy-40)/2)*4173u+(sx/2)*89u)&0xffffff;
    }
    redraw(); /* identical_frame compares actual scanout to the CPU oracle */
    printf("101-video desktop zoom=%u accel=%u uploaded_pixels=%llu copies=%u stretches=%u fills=%u\n",
           zoom,available,(unsigned long long)uploaded_pixels,copies,stretches,fills);
    fflush(stdout);
    if (available && !getenv("CIUKI_101_BASELINE")) {
        assert((available==2 ? stretches>100 : copies>100) && uploaded_pixels<(uint64_t)d.width*d.height/2);
        /* A submitted copy can fail after modifying scanout. Reconstruction
         * still uses the client surface and completes with identical pixels. */
        if (available==2) stretch_fail=1; else fail_copy=1;
        for (unsigned p=0;p<640*480;p++) pixels[p]^=0x112233;
        dirty_client(i); redraw(); redraw(); assert(!caps && !d.accel_caps);
    }
    for (int k=0;k<DESK_CLIENTS;k++) desk_drop_client(&d,k);
    desk_render_release(&d); free(d.pixels); free(scanout); assert(!fake_live());
}
int main(void)
{
    for (unsigned zoom=0;zoom<3;zoom++) { dos_video_frame(0,zoom); dos_video_frame(1,zoom); dos_video_frame(2,zoom); }
    foreign_regions();
    late_cursor_ready();
    cache_scenario();
    move_measurement(0); move_measurement(3);
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
    /* A return can arrive as damage before the display-counter poll. Learn
     * the same-layout lease generation before any engine request, then send
     * the first repaint as one complete CPU upload. A busy query preserves
     * damage and the old generation for a later service turn. */
    d.display_info=display_info; expected_generation=56;
    query_failure=1; dirty_client(i);
    unsigned old_uploads=uploads;
    assert(desk_redraw(&d)==-1 && d.accel_generation==55 && d.damage.count && uploads==old_uploads);
    query_failure=0; full_uploads=0;
    redraw(); assert(d.accel_generation==56 && d.scanout_valid && full_uploads==1);
    assert(uploads==old_uploads+1 && d.frame_stats.completed);
    puts("PASS f3-43 desktop: restore damage queries the new generation before acceleration, failed query retains damage, first resumed frame is one full present");
    for(int k=0;k<DESK_CLIENTS;k++)desk_drop_client(&d,k);
    desk_render_release(&d);free(d.pixels);free(scanout);assert(!fake_live());
    puts("PASS localized notification protection: client-only painter, disjoint scroll/move COPY, source/destination overlap, shadow and entry sweep");
    puts("desktop acceleration/fallback visible-frame equivalence, Paint cursor shapes/hotspots/clipping and late-ready rectangles: PASS");return 0;
}
