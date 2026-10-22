/* f2-96: one pointer on screen. The production compositor against a model of
 * the Savage engine and its hardware cursor (savage2d.c semantics: CURSOR_SET
 * hides before loading, SHOW needs a loaded shape, a stopped or quarantined
 * engine hides the cursor, fbdev answers EBUSY/EAGAIN without touching it).
 * After every frame: the scanout is exactly the scene without a pointer or
 * with the software pointer (no stale pointer image anywhere), a visible
 * hardware cursor never has a software pointer beside it, and in steady
 * state exactly one pointer is visible where the software pointer would be.
 * Golden frames (the visible picture: scanout plus the modelled hardware
 * cursor) pin the software state, the hardware state and every switch.
 * The T23 trial of a4d9454e showed both pointers a few pixels apart: a busy
 * or refused cursor request used to drop the hardware owner without hiding
 * the hardware cursor, which then stayed frozen beside the software one.
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../assets/cursors/cursors.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 240
static struct desktop d;
static uint32_t *scanout;
static const char *frames;
static uint64_t clock_ns=1000000000ull;
static uint64_t test_clock(void) { return clock_ns; }
static uint32_t generation=55;
void desk_test_profile(const char *name) { (void)name; } /* paint_probe.h hook */

static struct {
    unsigned caps;
    int shown, loaded, x, y;
    uint32_t mask[32], image[32], fg, bg;
    /* Answers to give before acting: EBUSY leaves everything unchanged. */
    unsigned busy_query, busy_set, busy_move, busy_show;
    int refuse_move, lose_on_move;
    unsigned sets, moves, shows, hides, busy;
} hw;
static void engine_lost(void) { hw.caps=0; hw.shown=0; hw.loaded=0; }
static int service(int display,const struct ciuki_accel_request *q)
{
    assert(display==7 && q->size==sizeof(*q));
    if (q->generation!=generation) return -EAGAIN;
    switch (q->operation) {
    case CIUKI_ACCEL_QUERY:
        if (hw.busy_query) { hw.busy_query--; hw.busy++; return -EBUSY; }
        return (int)hw.caps;
    case CIUKI_ACCEL_OFFSCREEN: return 0;
    case CIUKI_ACCEL_COPY:
        if (!(hw.caps&1u)) return -ENODEV;
        for (uint32_t n=0;n<q->rect.height;n++) {
            uint32_t y=q->rect.dst_y>q->rect.src_y ? q->rect.height-1-n : n;
            memmove(scanout+((uint32_t)q->rect.dst_y+y)*W+(uint32_t)q->rect.dst_x,
                    scanout+((uint32_t)q->rect.src_y+y)*W+(uint32_t)q->rect.src_x,q->rect.width*4);
        }
        return 0;
    case CIUKI_ACCEL_FILL:
        if (!(hw.caps&1u)) return -ENODEV;
        for (uint32_t y=0;y<q->rect.height;y++) for (uint32_t x=0;x<q->rect.width;x++)
            scanout[((uint32_t)q->rect.dst_y+y)*W+(uint32_t)q->rect.dst_x+x]=q->colour;
        return 0;
    case CIUKI_ACCEL_CURSOR_SET:
        if (!(hw.caps&2u)) return -EOPNOTSUPP;
        if (hw.busy_set) { hw.busy_set--; hw.busy++; return -EBUSY; }
        hw.shown=0; /* savage2d cursor_set: cursor_show(false) first */
        memcpy(hw.mask,q->cursor_mask,sizeof(hw.mask)); memcpy(hw.image,q->cursor_image,sizeof(hw.image));
        hw.fg=q->colour; hw.bg=q->background; hw.loaded=1; hw.sets++;
        return 0;
    case CIUKI_ACCEL_CURSOR_MOVE:
        if (!(hw.caps&2u)) return -EOPNOTSUPP;
        if (hw.busy_move) { hw.busy_move--; hw.busy++; return -EBUSY; }
        if (hw.lose_on_move) { hw.lose_on_move=0; engine_lost(); return -EIO; }
        if (hw.refuse_move) { hw.refuse_move=0; return -EINVAL; }
        hw.x=q->rect.dst_x; hw.y=q->rect.dst_y; hw.moves++;
        return 0;
    case CIUKI_ACCEL_CURSOR_SHOW:
        if (!(hw.caps&2u)) return -EOPNOTSUPP;
        if (hw.busy_show) { hw.busy_show--; hw.busy++; return -EBUSY; }
        if (q->colour && !hw.loaded) return -ENODEV;
        hw.shown=q->colour!=0;
        if (hw.shown) hw.shows++; else hw.hides++;
        return 0;
    }
    return -EINVAL;
}
static int upload(int display,int surface,const struct ciuki_rect *r)
{
    (void)display; (void)surface;
    for (uint32_t y=0;y<r->height;y++)
        memcpy(scanout+((uint32_t)r->dst_y+y)*W+(uint32_t)r->dst_x,
               d.pixels+((uint32_t)r->src_y+y)*W+(uint32_t)r->src_x,r->width*4);
    return 0;
}
/* The scene with the software pointer (pointer=1) or without any pointer. */
static void compose(int pointer,uint32_t *out)
{
    struct desktop *o=malloc(sizeof(d)); assert(o); *o=d;
    memset(out,0,(size_t)W*H*4); o->pixels=out;
    o->hw_cursor=!pointer; o->hw_cursor_shown=0;
    desk_composite(o,(struct desk_box){0,0,W,H});
    free(o);
}
static void visible(uint32_t *out)
{
    memcpy(out,scanout,(size_t)W*H*4);
    if (!hw.shown) return;
    for (int y=0;y<32;y++) for (int x=0;x<32;x++) {
        int px=hw.x+x, py=hw.y+y;
        if (px<0 || py<0 || px>=W || py>=H || !(hw.mask[y]&(0x80000000u>>x))) continue;
        out[py*W+px]=(hw.image[y]&(0x80000000u>>x)) ? hw.fg : hw.bg;
    }
}
static void save(const char *name,const uint32_t *pixels)
{
    if (!frames || !name) return;
    char path[512]; snprintf(path,sizeof(path),"%s/%s.ppm",frames,name);
    FILE *f=fopen(path,"wb"); assert(f);
    fprintf(f,"P6\n%d %d\n255\n",W,H);
    for (int i=0;i<W*H;i++) { uint8_t rgb[3]={pixels[i]>>16,pixels[i]>>8,pixels[i]}; fwrite(rgb,1,3,f); }
    fclose(f);
}
static unsigned checks;
/* steady: the frame after every retry. The pointer is then visible exactly
 * once, at the software pointer's place and in its shape. */
static void check(const char *name,int steady)
{
    uint32_t *bare=malloc((size_t)W*H*4), *soft=malloc((size_t)W*H*4), *seen=malloc((size_t)W*H*4);
    assert(bare && soft && seen);
    compose(0,bare); compose(1,soft); visible(seen);
    int is_bare=!memcmp(scanout,bare,(size_t)W*H*4), is_soft=!memcmp(scanout,soft,(size_t)W*H*4);
    int pointer=memcmp(bare,soft,(size_t)W*H*4)!=0; /* a pointer to show (not hidden by a client) */
    int soft_visible=pointer && is_soft && !is_bare;
    if (!is_bare && !is_soft) {
        for (int p=0;p<W*H;p++) if (scanout[p]!=soft[p] && scanout[p]!=bare[p]) {
            fprintf(stderr,"%s: stale pixel x=%d y=%d got=%06x bare=%06x soft=%06x hw=%d/%d shown=%d\n",
                name,p%W,p/W,scanout[p],bare[p],soft[p],d.hw_cursor,d.hw_cursor_shown,hw.shown);
            break;
        }
        abort();
    }
    if (hw.shown && soft_visible) { fprintf(stderr,"%s: hardware and software pointer both visible\n",name); abort(); }
    if (hw.shown) assert(d.hw_cursor || d.hw_cursor_shown); /* the desktop never forgets a visible cursor */
    if (steady) {
        assert(!d.cursor_dirty);
        if (pointer) {
            assert(hw.shown+soft_visible==1);
            if (hw.shown && memcmp(seen,soft,(size_t)W*H*4)) {
                fprintf(stderr,"%s: hardware cursor shape/position differs from the software pointer\n",name); abort();
            }
        } else assert(!hw.shown);
    }
    save(name,seen); checks++;
    free(bare); free(soft); free(seen);
}
static int one(void) { return desk_redraw(&d); }
static void settle(void)
{
    for (unsigned i=0;i<6;i++) {
        int r=desk_redraw(&d);
        if (!r && !d.cursor_dirty && !d.damage.count && !d.present_damage.count) return;
    }
    fprintf(stderr,"frames did not settle: hw=%d shown=%d dirty=%d\n",d.hw_cursor,d.hw_cursor_shown,d.cursor_dirty);
    abort();
}
static void motion(int dx,int dy) { d.mouse_x+=dx; d.mouse_y+=dy; desk_cursor_motion(&d); }
static int window_new(int ch,int surf,unsigned w,unsigned h,int x,int y)
{
    fake_channel(ch); int i=desk_add_client(&d,ch); assert(i>=0);
    struct ciuki_message m; desk_message(&m,DESK_HELLO,4); m.sender_pid=42+i; desk_receive(&d,i,&m);
    fake_surface(surf,w,h,0x123456);
    desk_message(&m,DESK_CREATE_WINDOW,20); m.sender_pid=42+i; m.fd_count=1; m.fds[0]=surf;
    desk_put32(m.data+4,w); desk_put32(m.data+8,h); desk_put32(m.data+12,4); memcpy(m.data+16,"Test",4);
    desk_receive(&d,i,&m); assert(d.clients[i].window);
    desk_move(&d,i,x,y);
    uint32_t *p=(uint32_t *)d.clients[i].pixels;
    for (unsigned row=0;row<h;row++) for (unsigned col=0;col<w;col++) p[row*w+col]=((row+1)*92761u+col*817u)&0xffffff;
    return i;
}
int main(int argc,char **argv)
{
    frames=argc>1 ? argv[1] : NULL;
    fake_reset(); struct desk_ops ops=fake_ops; ops.present=upload;
    desk_init(&d,&ops,W,H); d.display=7; d.time_ns=test_clock;
    d.pixels=calloc((size_t)W*H,4); scanout=calloc((size_t)W*H,4); assert(d.pixels && scanout);
    d.accel=service; hw.caps=0; desk_accel_reset(&d,generation);
    int i=window_new(10,20,140,90,60,50);
    d.mouse_x=200; d.mouse_y=120;

    /* 1. Engine not ready yet: software pointer. */
    settle(); assert(!d.hw_cursor && !hw.shown); check("01-software",1);
    /* 2. f2-92 late readiness, same generation: the hardware cursor takes
     * over and the software pointer leaves the scanout in the same frame. */
    hw.caps=3; motion(7,3); settle();
    assert(d.hw_cursor && d.hw_cursor_shown && hw.shown); check("02-hardware-late-ready",1);
    /* 3. Motion moves only the hardware cursor (no scanout upload). */
    unsigned moves=hw.moves; motion(20,10); settle();
    assert(hw.moves==moves+1 && !d.frame_stats.lfb_bytes); check("03-hardware-moved",1);
    /* 4. A busy MOVE keeps the hardware owner: the cursor stays where it was
     * for one frame, no software pointer is drawn, the next frame moves it. */
    hw.busy_move=1; motion(15,-6); assert(!one());
    assert(d.hw_cursor && hw.shown && d.cursor_dirty); check("04-busy-move-held",0);
    assert(!one()); check("05-busy-move-retried",1);
    /* 5. A busy CURSOR_SET on a shape change: the old shape stays visible,
     * then the new one is loaded and shown. */
    d.cursor_shape=DESK_CURSOR_TEXT; d.cursor_dirty=1; hw.busy_set=1; assert(!one());
    assert(d.hw_cursor && hw.shown && d.cursor_loaded!=DESK_CURSOR_TEXT); check("06-busy-shape-held",0);
    assert(!one()); assert(d.cursor_loaded==DESK_CURSOR_TEXT); check("07-busy-shape-retried",1);
    d.cursor_shape=DESK_CURSOR_ARROW; d.cursor_dirty=1; settle();
    /* 6. A busy QUERY is not an engine loss: no notice, no software pointer. */
    hw.busy_query=1; motion(-30,12); settle();
    assert(!d.accel_lost && d.accel_caps==3 && d.hw_cursor && hw.busy); check("08-busy-query",1);
    /* 7. A client bitmap pointer (v3) needs software; while the hide is busy
     * the hardware cursor stays alone, then the client pointer is drawn. */
    unsigned slot=DESK_CLIENTS-1;
    for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x++)
        d.clients[slot].v3_cursor_pixels[0][y*32+x]=x<14 && y<14 ? (x+y<4 ? 0x80ff00ffu : 0xffff00ffu) : 0;
    d.clients[slot].v3_hot_x[0]=3; d.clients[slot].v3_hot_y[0]=3;
    d.v3_cursor_slot=slot; d.v3_cursor_id=1; d.cursor_dirty=1;
    hw.busy_show=2; assert(!one());
    assert(!d.hw_cursor && d.hw_cursor_shown && hw.shown && d.cursor_dirty); check("09-v3-hide-busy",0);
    settle(); assert(!hw.shown && !d.hw_cursor_shown && !d.hw_cursor); check("10-v3-custom",1);
    /* 8. The client pointer ends: the engine takes the pointer back at once
     * (before f2-96 the software pointer stayed until a mode change). */
    d.v3_cursor_id=0; d.cursor_dirty=1; settle();
    assert(d.hw_cursor && hw.shown); check("11-v3-end-hardware",1);
    /* 9. Engine loss (quarantine hides the cursor): software returns. */
    engine_lost(); motion(-12,-20); settle();
    assert(!d.hw_cursor && !d.hw_cursor_shown); check("12-engine-loss-software",1);
    /* 10. The engine comes back: hardware again. */
    hw.caps=3; motion(5,5); settle(); assert(d.hw_cursor && hw.shown); check("13-engine-back-hardware",1);
    /* 11. A definitive refusal with the engine intact: the hardware cursor is
     * hidden first, then software draws (before f2-96 both stayed visible). */
    unsigned hides=hw.hides; hw.refuse_move=1; motion(9,9); settle();
    assert(!d.hw_cursor && !hw.shown && hw.hides==hides+1 && d.hw_cursor_fallbacks==1);
    check("14-refused-move-software",1);
    /* ... and the engine is offered the pointer again only after 1 s. */
    clock_ns+=500000000ull; motion(-4,0); settle(); assert(!d.hw_cursor); check(NULL,1);
    clock_ns+=600000000ull; motion(-4,0); settle(); assert(d.hw_cursor && hw.shown); check("15-rearm-hardware",1);
    /* 12. The same refusal while the hide is busy: nothing is drawn beside
     * the frozen hardware cursor until it is hidden. */
    hw.refuse_move=1; hw.busy_show=2; motion(6,-3); one();
    assert(!d.hw_cursor && d.hw_cursor_shown && hw.shown); check("16-refused-hide-busy",0);
    settle(); assert(!hw.shown && !d.hw_cursor); check("17-refused-hide-done",1);
    /* 13. Engine loss reported by a MOVE: software at once, no hide needed. */
    clock_ns+=2000000000ull; motion(1,1); settle(); assert(d.hw_cursor && hw.shown);
    hw.lose_on_move=1; motion(3,3); settle(); assert(!d.hw_cursor && !hw.shown); check("18-loss-on-move-software",1);
    /* 14. A display lease or mode change returns with a new generation (the
     * kernel hid the cursor before): hardware again with the new tuple. */
    hw.caps=3; generation=56; desk_accel_reset(&d,generation); settle();
    assert(d.hw_cursor && hw.shown); check("19-new-generation-hardware",1);
    /* 15. The window moves under a hardware pointer: engine copies, one pointer. */
    desk_move(&d,i,80,60); settle(); check("20-window-move-hardware",1);
    printf("PASS f2-96 pointer: %u frames, sets=%u moves=%u shows=%u hides=%u busy=%u retries=%u fallbacks=%u; "
           "never both pointers, busy keeps the owner, refusal hides first, client pointer and engine loss use software, the engine takes it back\n",
           checks,hw.sets,hw.moves,hw.shows,hw.hides,hw.busy,d.hw_cursor_retries,d.hw_cursor_fallbacks);
    for (int k=0;k<DESK_CLIENTS;k++) desk_drop_client(&d,k);
    desk_render_release(&d); free(d.pixels); free(scanout); assert(!fake_live());
    return 0;
}
