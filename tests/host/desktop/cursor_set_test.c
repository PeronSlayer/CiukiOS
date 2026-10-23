/* Original cursors, independent software blend/hotspot oracle, exact cursor
 * damage and toolkit -> protocol -> desktop context selection. Host only.
 * SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../assets/cursors/paint.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 240
static struct desktop d;
static unsigned cursor_calls;
void desk_test_profile(const char *name) { if (!strcmp(name,"cursor")) cursor_calls++; }
static void redraw(void)
{
    fake_presents=0;
    assert(!desk_redraw(&d));
    assert(!d.cursor_dirty && !d.damage.count);
}
static uint32_t blend_shadow(uint32_t pixel, unsigned alpha)
{
    uint32_t out=0;
    for (unsigned shift=0;shift<24;shift+=8)
        out|=((((pixel>>shift)&255)*(255-alpha)+127)/255)<<shift;
    return out;
}
static void oracle(void)
{
    uint32_t *expected=calloc(W*H,4); assert(expected);
    struct desktop *copy=malloc(sizeof(d)); assert(copy); *copy=d;
    copy->pixels=expected; copy->hw_cursor=1;
    desk_composite(copy,(struct desk_box){0,0,W,H});
    const struct desk_cursor_raster *r=desk_generic_cursor(d.cursor_shape); assert(r);
    for (int y=0;y<32;y++) for (int x=0;x<32;x++) {
        int px=d.mouse_x-(int)r->hot_x+x, py=d.mouse_y-(int)r->hot_y+y;
        if (px<0 || py<0 || px>=W || py>=H) continue;
        uint32_t bit=0x80000000u>>x, *p=expected+py*W+px;
        if (r->mask[y]&bit) *p=(r->image[y]&bit) ? 0xffffffu : 0;
        else *p=blend_shadow(*p,r->shadow[y*32+x]);
    }
    if (memcmp(expected,d.pixels,W*H*4)) {
        for (int i=0;i<W*H;i++) if (expected[i]!=d.pixels[i]) {
            fprintf(stderr,"shape=%u pointer=%d,%d pixel=%d,%d expected=%06x actual=%06x\n",
                d.cursor_shape,d.mouse_x,d.mouse_y,i%W,i/W,expected[i],d.pixels[i]); break;
        }
        abort();
    }
    free(copy); free(expected);
}
static void save(const char *folder, const char *name)
{
    char path[512]; snprintf(path,sizeof(path),"%s/%s.ppm",folder,name);
    FILE *f=fopen(path,"wb"); assert(f); fprintf(f,"P6\n%d %d\n255\n",W,H);
    for (unsigned i=0;i<W*H;i++) {
        uint32_t p=d.pixels[i]; uint8_t rgb[]={p>>16,p>>8,p}; assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}
static void artwork(const char *folder)
{
    static const unsigned shapes[]={0,1,2,3,4,5,6,7,8,9,10,11,12,19,20,21};
    static const unsigned hot[16][2]={{4,3},{15,15},{4,3},{15,15},{15,15},{15,15},{15,15},{15,15},
        {15,15},{15,15},{12,3},{15,15},{15,15},{15,15},{15,15},{4,3}};
    static const int points[][2]={{100,90},{0,0},{W-1,0},{0,H-1},{W-1,H-1}};
    for (unsigned i=0;i<16;i++) {
        const struct desk_cursor_raster *r=desk_generic_cursor(shapes[i]); assert(r);
        assert(r->hot_x==hot[i][0] && r->hot_y==hot[i][1] && r->width<=32 && r->height<=32);
        unsigned shadow=0, black=0, white=0;
        for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x++) {
            uint32_t bit=0x80000000u>>x;
            assert(!(r->image[y]&~r->mask[y]));
            if ((r->mask[y]&bit) || r->shadow[y*32+x]) assert(x<r->width && y<r->height);
            assert(r->shadow[y*32+x]<=80 && (!(r->mask[y]&bit) || !r->shadow[y*32+x]));
            shadow+=r->shadow[y*32+x]!=0;
            black+=!!((r->mask[y]&~r->image[y])&bit); white+=!!(r->image[y]&bit);
        }
        assert(shadow && black && white);
        /* fx-13: the arrow is the classic size (Windows 9x standard arrow is
         * about 12x19-20 px): opaque pixels span 13x19 and the hotspot pixel
         * is the opaque tip. Every other shape is scaled by the same factor. */
        if (!i) {
            unsigned minx=32,maxx=0,miny=32,maxy=0;
            for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x++) if (r->mask[y]&(0x80000000u>>x)) {
                minx=x<minx?x:minx; maxx=x>maxx?x:maxx; miny=y<miny?y:miny; maxy=y>maxy?y:maxy;
            }
            assert(maxx-minx+1==13 && maxy-miny+1==19);
            assert(minx==r->hot_x && miny==r->hot_y && (r->mask[r->hot_y]&(0x80000000u>>r->hot_x)));
        }
        for (unsigned k=0;k<sizeof(points)/sizeof(*points);k++) {
            d.cursor_shape=shapes[i]; d.mouse_x=points[k][0]; d.mouse_y=points[k][1]; desk_cursor_motion(&d);
            redraw(); oracle();
            if (!k) { char name[32]; snprintf(name,sizeof(name),"cursor-%02u",shapes[i]); save(folder,name); }
        }
    }
    d.cursor_shape=DESK_CURSOR_ARROW; d.mouse_x=100; d.mouse_y=90; desk_cursor_motion(&d); redraw();
    cursor_calls=0; desk_damage_add(&d.damage,(struct desk_box){0,0,1,1},W,H); redraw();
    assert(!cursor_calls); oracle();
    /* Deliberately bypass list coalescing: both rectangles cover the same
     * translucent shadow. Each affected pixel must still be blended once. */
    struct desk_box b={d.cursor_box.x+6,d.cursor_box.y+5,5,6};
    d.damage.boxes[0]=b; d.damage.boxes[1]=b; d.damage.count=2;
    cursor_calls=0; redraw(); assert(cursor_calls && cursor_calls<=6); oracle();
    save(folder,"overlapping-damage");
    d.mouse_x=130;d.mouse_y=110; /* shell updates geometry with scene damage */
    desk_damage_add(&d.damage,(struct desk_box){0,0,1,1},W,H);redraw();oracle();
    puts("PASS f2-99: 16 original shapes, fixed hotspots, 80 software frames at all corners, alpha shadow oracle, disjoint/overlapping damage");
}
static void request_cursor(int i, unsigned shape)
{
    struct ciuki_message m; desk_message(&m,DESK_SET_CURSOR,8);
    m.data[0]=2; m.data[2]=1;
    m.sender_pid=42; desk_put32(m.data+4,shape); desk_receive(&d,i,&m);
    assert(d.clients[i].cursor==shape && d.cursor_shape==shape);
}
static void contexts(void)
{
    fake_channel(10); int i=desk_add_client(&d,10); assert(i>=0);
    struct ciuki_message m; desk_message(&m,DESK_HELLO,4); m.data[0]=2; m.data[2]=1;
    m.sender_pid=42; desk_receive(&d,i,&m);
    fake_surface(20,120,90,0x204060);
    desk_message(&m,DESK_CREATE_WINDOW,48); m.data[0]=2; m.data[2]=1;
    m.sender_pid=42; m.fd_count=1; m.fds[0]=20;
    desk_put32(m.data+4,120); desk_put32(m.data+8,90); desk_put32(m.data+12,4); memcpy(m.data+16,"Test",4);
    const unsigned values[]={DESK_RESIZABLE,120,90,240,180,1,1};
    for (unsigned k=0;k<7;k++) desk_put32(m.data+20+k*4,values[k]);
    desk_receive(&d,i,&m); assert(d.clients[i].window); desk_move(&d,i,60,50);
    struct desk_box b=desk_content(&d.clients[i]); d.mouse_x=b.x+20; d.mouse_y=b.y+20;
    uint32_t *pixels=calloc(120*90,4); assert(pixels);
    struct du_ui u; du_init(&u,pixels,120,90); u.mouse_x=20; u.mouse_y=20;
    struct du_widget *edit=du_add(&u,DU_EDIT,1,(struct du_rect){0,0,60,30},"Ciuki"); assert(edit);
    request_cursor(i,du_cursor_at(&u)); assert(d.cursor_shape==DESK_CURSOR_TEXT);
    /* Links use a canvas subregion, as the browser does, through the same API. */
    edit->hidden=1;
    struct du_widget *link=du_add(&u,DU_CANVAS,2,(struct du_rect){0,0,100,70},""); assert(link);
    link->pointer_cursor=DESK_CURSOR_HAND; link->cursor_box=(struct du_rect){10,10,30,20};
    request_cursor(i,du_cursor_at(&u)); assert(d.cursor_shape==DESK_CURSOR_HAND);
    u.mouse_x=60; request_cursor(i,du_cursor_at(&u)); assert(d.cursor_shape==DESK_CURSOR_ARROW);
    for (unsigned shape=DESK_CURSOR_OPEN_HAND;shape<DESK_CURSOR_COUNT;shape++) request_cursor(i,shape);
    request_cursor(i,DESK_CURSOR_ARROW);
    struct desk_box f=d.clients[i].frame;
    const int positions[][2]={{f.x,f.y+40},{f.x+f.w-1,f.y+40},{f.x+40,f.y},{f.x+40,f.y+f.h-1},
        {f.x,f.y},{f.x+f.w-1,f.y},{f.x,f.y+f.h-1},{f.x+f.w-1,f.y+f.h-1}};
    const unsigned shapes[]={5,5,6,6,7,8,8,7};
    for (unsigned n=0;n<8;n++) {
        d.mouse_x=positions[n][0]; d.mouse_y=positions[n][1]; desk_cursor_update(&d); assert(d.cursor_shape==shapes[n]);
    }
    d.mouse_x=b.x+20; d.mouse_y=b.y+20;
    d.clients[i].unresponsive=1; desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_BUSY);
    d.clients[i].unresponsive=0; d.mouse_x=280; d.mouse_y=200; d.now_ns=2000000000ull;
    d.clients[i].awaiting_frame=1; d.clients[i].launched_ns=d.now_ns;
    desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_ARROW_BUSY);
    d.clients[i].awaiting_frame=0; desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_ARROW);
    free(pixels);
    puts("PASS f2-99 context selection: text edit, link hit region, 8 resize edges/corners, busy, launch, appended cursor IDs over existing protocol");
}
int main(int argc, char **argv)
{
    assert(argc==2); fake_reset(); desk_init(&d,&fake_ops,W,H); d.display=7;
    d.pixels=calloc(W*H,4); assert(d.pixels);
    artwork(argv[1]); contexts();
    for (int i=0;i<DESK_CLIENTS;i++) desk_drop_client(&d,i);
    desk_render_release(&d); free(d.pixels); assert(!fake_live()); fflush(stdout);
    return 0;
}
