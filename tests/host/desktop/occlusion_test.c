/* SPDX-License-Identifier: MIT
 * fx-2: an application paints only inside its window's visible region
 * (Windows 9x USER: WM_PAINT is clipped to the visible region, the window
 * manager repaints what a z-order change uncovers).
 *
 * The production desktop runs against four presenters: the CPU upload of
 * the software engine (the T23 trial of image 9a60215f: accel caps=0,
 * pointer=sw), the 2D engine (screen COPY/FILL, hardware pointer) and the
 * f2-101d command batches (MONO, INVERT, KEYED, WRITE through production
 * preflight and swdib) at 32 and 16 bpp. Every frame is compared with a
 * full composite; in 32 bpp an independent oracle checks each window's
 * unoccluded client pixels against its own surface. Deterministic
 * scenarios hold a covering window's screen pixels fixed while the window
 * under it animates, scrolls, moves, retitles and draws with every
 * client-side command type; z-order changes (focus raise, v3 owned and
 * topmost bands, internal shell windows, v3 window order) must repaint
 * the overlap before either window's next update. */
#define DESKTOP_COMMANDS_NO_MAIN
#include "commands_test.c"
#include "../../../apps/libdeskui/v3.h"
#include <signal.h>
#include "../../../apps/desktop/shell.h"

enum { MODE_SOFTWARE, MODE_ENGINE, MODE_BATCH32, MODE_BATCH16, MODES };
static const char *const mode_names[MODES]={"software","engine","batch32","batch16"};
static unsigned mode;
static char step_name[96];
static unsigned frames_checked, visible_pixels_checked;

static void visible_oracle(void)
{
    /* 32 bpp only: the scanout holds the exact XRGB pixels. A pixel of a
     * window's client area that no higher window (shadow included), no
     * shell chrome and no pointer covers is that surface's own pixel. */
    if (mode==MODE_BATCH16) return;
    struct desk_box pointer={d.mouse_x-40,d.mouse_y-40,80,80};
    struct desk_box occluders[SH_LEVELS+6]; unsigned occluder_count=d.shell ? desk_shell_occluders(&d,occluders) : 0;
    for (int i=0;i<DESK_CLIENTS;i++) {
        const struct desk_client *c=&d.clients[i];
        if (!c->window || c->minimized || c->collapsed || !c->pixels || c->internal) continue;
        struct desk_box b=desk_content(c);
        if (b.w!=(int)c->info.width || b.h!=(int)c->info.height || c->gfx_width) continue;
        for (int y=b.y;y<b.y+b.h;y++) for (int x=b.x;x<b.x+b.w;x++) {
            if (x<0 || y<DESK_BAR || x>=(int)d.width || y>=(int)d.height-(d.shell ? DESK_BAR : 0) ||
                desk_contains(pointer,x,y)) continue;
            if(d.geometry_outline) {
                struct desk_box q=d.geometry_preview;
                if(desk_contains(q,x,y) && (x<q.x+3 || x>=q.x+q.w-3 || y<q.y+3 || y>=q.y+q.h-3)) continue;
            }
            int covered=0;
            for (int j=0;j<DESK_CLIENTS && !covered;j++) if (j!=i && d.clients[j].window && !d.clients[j].minimized &&
                d.clients[j].order>c->order && desk_contains(desk_outer(&d.clients[j]),x,y)) covered=1;
            for (unsigned k=0;k<occluder_count && !covered;k++) {
                struct desk_box q=occluders[k]; q.x-=8; q.y-=16; q.w+=16; q.h+=32;
                covered=desk_contains(q,x,y);
            }
            if (covered || (d.shell && desk_shell_chrome_intersects(&d,(struct desk_box){x,y,1,1}))) continue;
            uint32_t want=c->pixels[(size_t)(y-b.y)*c->info.width+(unsigned)(x-b.x)]&0xffffffu;
            uint32_t got=scanout[(size_t)y*d.width+(unsigned)x];
            if (got!=want) {
                fprintf(stderr,"%s %s: window %d client pixel %d,%d on screen %06x, its surface %06x\n",
                        mode_names[mode],step_name,i,x,y,got,want);
                abort();
            }
            visible_pixels_checked++;
        }
    }
}
static void batch_frame(void)
{
    /* commands_test.c command_redraw, reporting the first differing pixel. */
    assert(!desk_redraw(&d));
    uint32_t *expected=calloc((size_t)d.width*d.height,4), *visible=malloc((size_t)d.width*d.height*4);
    struct desktop *oracle=malloc(sizeof(d)); assert(expected && visible && oracle);
    *oracle=d; oracle->pixels=expected; oracle->hw_cursor=0;
    desk_composite(oracle,(struct desk_box){0,0,(int)d.width,(int)d.height});
    memcpy(visible,scanout,(size_t)d.width*d.height*4);
    if (pixel_bpp==16) for (unsigned y=0;y<d.height;y++) for (unsigned x=0;x<d.width;x++) {
        uint16_t value; memcpy(&value,(uint8_t *)scanout+(size_t)y*d.width*4+x*2,2);
        visible[(size_t)y*d.width+x]=(((255*((value>>11)&31)+15)/31)<<16)|(((255*((value>>5)&63)+31)/63)<<8)|((255*(value&31)+15)/31);
    }
    if (shown) for (unsigned y=0;y<colour_h;y++) for (unsigned x=0;x<colour_w;x++) {
        int dx=cursor_x+(int)x, dy=cursor_y+(int)y;
        if (dx<0 || dy<0 || dx>=(int)d.width || dy>=(int)d.height) continue;
        uint32_t s=colour_pixels[y*colour_w+x], *under=&visible[(unsigned)dy*d.width+(unsigned)dx];
        unsigned a=s>>24; if (!a) continue;
        unsigned red=((s>>16)&255)+(((((*under)>>16)&255)*(255-a)+127)/255);
        unsigned green=((s>>8)&255)+(((((*under)>>8)&255)*(255-a)+127)/255);
        unsigned blue=(s&255)+(((*under&255)*(255-a)+127)/255);
        *under=red<<16|green<<8|blue;
    }
    for (unsigned i=0;i<d.width*d.height;i++) {
        uint32_t a=expected[i], b=visible[i];
        if (pixel_bpp==16) { a=((a>>8)&0xf800)|((a>>5)&0x07e0)|((a>>3)&31); b=((b>>8)&0xf800)|((b>>5)&0x07e0)|((b>>3)&31); }
        if (a!=b) {
            fprintf(stderr,"%s %s: screen pixel %u,%u is %06x, the full composite %06x\n",mode_names[mode],step_name,i%d.width,i/d.width,b,a);
            abort();
        }
    }
    free(expected); free(visible); free(oracle);
}
static int fuzzing;
static void frame(void)
{
    if (mode>=MODE_BATCH32) batch_frame();
    else redraw();
    /* The full-composite comparison runs on every frame; the per-pixel
     * surface oracle on every deterministic frame and every third fuzz one. */
    if (!fuzzing || frames_checked%3==0) visible_oracle();
    frames_checked++;
}
static int service16(int display, const struct ciuki_accel_request *q)
{
    /* The 16 bpp scanout is packed (pitch width*4, two bytes a pixel);
     * accel_test.c's engine model addresses 32-bit pixels. */
    if (q->operation!=CIUKI_ACCEL_COPY && q->operation!=CIUKI_ACCEL_FILL) return service(display,q);
    assert(display==7 && q->generation==expected_generation);
    completed_ram_frame();
    uint8_t *base=(uint8_t *)scanout; size_t pitch=(size_t)d.width*4;
    if (q->operation==CIUKI_ACCEL_COPY) {
        copies++;
        for (uint32_t n=0;n<q->rect.height;n++) {
            uint32_t y=q->rect.dst_y>q->rect.src_y ? q->rect.height-1-n : n;
            memmove(base+((uint32_t)q->rect.dst_y+y)*pitch+(uint32_t)q->rect.dst_x*2,
                    base+((uint32_t)q->rect.src_y+y)*pitch+(uint32_t)q->rect.src_x*2,q->rect.width*2);
        }
    } else {
        fills++;
        uint16_t packed=((q->colour>>8)&0xf800)|((q->colour>>5)&0x7e0)|((q->colour>>3)&31);
        for (uint32_t y=0;y<q->rect.height;y++) for (uint32_t x=0;x<q->rect.width;x++)
            memcpy(base+((uint32_t)q->rect.dst_y+y)*pitch+((uint32_t)q->rect.dst_x+x)*2,&packed,2);
    }
    return 0;
}
static void begin(unsigned m, int shell)
{
    mode=m; fake_reset(); struct desk_ops ops=fake_ops; ops.present=m>=MODE_BATCH32 ? packed_upload : upload;
    desk_init(&d,&ops,640,480); d.display=7; d.mouse_x=600; d.mouse_y=12;
    pixel_bpp=m==MODE_BATCH16 ? 16 : 32;
    d.pixels=calloc((size_t)d.width*d.height,4); scanout=calloc((size_t)d.width*(d.height+2),4);
    assert(d.pixels && scanout);
    caps=0; shown=fail_copy=fail_cursor=fail_fill=fail_write=0; vram_first=vram_end=0;
    if (m==MODE_ENGINE) { caps=3; d.accel=service; desk_accel_reset(&d,55); assert(d.hw_cursor); }
    if (m>=MODE_BATCH32) {
        caps=1; vram_first=d.height; vram_end=d.height+2; d.display_info=format_info; d.lfb_bytes_per_pixel=pixel_bpp/8;
        d.accel=m==MODE_BATCH16 ? service16 : service; d.commands=commands_info; d.batch=batch_service; d.fence=fence_service;
        desk_accel_reset(&d,55); assert(d.hw_cursor && d.hw_colour_cursor);
    }
    if (shell) desk_shell_enable(&d);
}
static void end(void)
{
    for (int k=0;k<DESK_CLIENTS;k++) desk_drop_client(&d,k);
    desk_render_release(&d); free(d.pixels); free(scanout);
    free(d.sh.icons); free(d.sh.wall); free(d.sh.background); free(d.v3_links); free(d.sh.autoplay.workspace);
    d.sh.icons=NULL; d.sh.wall=NULL; d.sh.background=NULL; d.v3_links=NULL; d.sh.autoplay.workspace=NULL;
    assert(!fake_live());
}
static void fill_pattern(int i, uint32_t salt)
{
    struct desk_client *c=&d.clients[i]; uint32_t *p=(uint32_t *)c->pixels;
    for (unsigned y=0;y<c->info.height;y++) for (unsigned x=0;x<c->info.width;x++)
        p[y*c->info.width+x]=(y*92761u+x*817u+salt)&0xffffffu;
}
static void native_damage(int i, unsigned x, unsigned y, unsigned w, unsigned h)
{
    struct ciuki_message m; desk_message(&m,DESK_DAMAGE,20); m.sender_pid=d.clients[i].pid;
    desk_put32(m.data+4,x); desk_put32(m.data+8,y); desk_put32(m.data+12,w); desk_put32(m.data+16,h);
    desk_receive(&d,i,&m);
    if (!d.clients[i].window) { fprintf(stderr,"dropped by damage %u,%u %ux%u at %s\n",x,y,w,h,step_name); abort(); }
}
/* ---- v3 (NetSurf, Win32) windows through the production protocol ---- */
static uint32_t token;
static int v3_root=-1;
static void v3msg(struct ciuki_message *m, unsigned op, unsigned n, unsigned id, unsigned gen)
{ desk_v3_message(m,op,n,++token,id,gen); m->sender_pid=d.clients[v3_root].pid; }
static void v3_peer(int fd)
{
    fake_channel(fd); v3_root=desk_add_client(&d,fd); assert(v3_root>=0); token=0;
    struct ciuki_message m; desk_v3_message(&m,DESK_HELLO,0,0,0,0); m.sender_pid=77;
    assert(!desk_v3_receive(&d,v3_root,&m));
}
static int v3_create(int fd, unsigned id, unsigned style, unsigned owner, int x, int y, uint32_t salt)
{
    fake_surface(fd,200,100,0x7498b0);
    struct ciuki_message m; v3msg(&m,DESK_CREATE_WINDOW,40,id,1); m.fd_count=1; m.fds[0]=fd;
    desk_put32(m.data+16,style); desk_put32(m.data+20,owner); desk_put32(m.data+24,owner ? 1 : 0);
    desk_put32(m.data+28,(uint32_t)x); desk_put32(m.data+32,(uint32_t)y);
    desk_put32(m.data+36,200); desk_put32(m.data+40,100); desk_put32(m.data+44,1); desk_put32(m.data+48,4); memcpy(m.data+52,"Test",4);
    assert(!desk_v3_receive(&d,v3_root,&m));
    for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].v3_peer==(unsigned)v3_root+1 && d.clients[i].v3_id==id) {
        fill_pattern(i,salt);
        struct ciuki_message damage; v3msg(&damage,DESK_DAMAGE,16,id,1);
        desk_put32(damage.data+16,0); desk_put32(damage.data+20,0); desk_put32(damage.data+24,200); desk_put32(damage.data+28,100);
        (void)desk_v3_receive(&d,v3_root,&damage);
        return i;
    }
    assert(0); return -1;
}
static void any_damage(int i, unsigned x, unsigned y, unsigned w, unsigned h)
{
    struct desk_client *c=&d.clients[i];
    if (c->version!=DESK_V3_VERSION) { native_damage(i,x,y,w,h); return; }
    struct ciuki_message m; v3msg(&m,DESK_DAMAGE,16,c->v3_id,c->v3_generation);
    desk_put32(m.data+16,x); desk_put32(m.data+20,y); desk_put32(m.data+24,w); desk_put32(m.data+28,h);
    (void)desk_v3_receive(&d,v3_root,&m);
}
static void v3_window_order(int i, unsigned index)
{
    struct desk_client *c=&d.clients[i]; struct ciuki_message m; v3msg(&m,DESK_V3_WINDOW_ORDER,20,c->v3_id,c->v3_generation);
    desk_put32(m.data+16,DESK_V3_ORDER_Z); desk_put32(m.data+20,index);
    desk_put32(m.data+24,0); desk_put32(m.data+28,0); desk_put32(m.data+32,0);
    (void)desk_v3_receive(&d,v3_root,&m);
}

/* ---- the covering window's screen pixels never change ---- */
static uint32_t screen_pixel(int x, int y)
{
    if (pixel_bpp==32) return scanout[(size_t)y*d.width+(unsigned)x];
    uint16_t value; memcpy(&value,(uint8_t *)scanout+(size_t)y*d.width*4+(unsigned)x*2,2); return value;
}
static uint32_t *held;
static struct desk_box held_box;
static void hold(int upper)
{
    /* The opaque frame below the rounded corners: those pixels depend on
     * nothing beneath the window. */
    struct desk_box f=d.clients[upper].frame; int r=desk_theme->radius;
    held_box=(struct desk_box){f.x,f.y+r,f.w,f.h-r};
    free(held); held=malloc((size_t)held_box.w*held_box.h*4); assert(held);
    for (int y=0;y<held_box.h;y++) for (int x=0;x<held_box.w;x++) held[(size_t)y*held_box.w+x]=screen_pixel(held_box.x+x,held_box.y+y);
}
static void unchanged(const char *what)
{
    snprintf(step_name,sizeof(step_name),"%s",what); frame();
    for (int y=0;y<held_box.h;y++) for (int x=0;x<held_box.w;x++) {
        uint32_t now=screen_pixel(held_box.x+x,held_box.y+y), was=held[(size_t)y*held_box.w+x];
        if (now!=was) {
            fprintf(stderr,"%s %s: covering window pixel %d,%d changed %06x -> %06x\n",mode_names[mode],what,
                    held_box.x+x,held_box.y+y,was,now);
            abort();
        }
    }
}
static void lower_draws_with_commands(int lower, unsigned round)
{
    /* The toolkit's client-side commands (draw.c du_commands): text (MONO),
     * focus/drag outlines (INVERT) and icons (KEYED), all positioned so
     * they span the area the upper window covers. */
    struct desk_client *c=&d.clients[lower];
    struct du_canvas canvas={(uint32_t *)c->pixels,(int)c->info.width,(int)c->info.height,{0,0,(int)c->info.width,(int)c->info.height}};
    static uint8_t bits[64*256/8]; static uint32_t icon[64*64];
    for (unsigned k=0;k<sizeof(bits);k++) bits[k]=(uint8_t)(k*37u+round);
    for (unsigned k=0;k<64*64;k++) icon[k]=(k+round)%5 ? (k*2654435761u+round)&0xffffffu : 0xff00ffu;
    struct ciuki_batch_record records[3]={
        {.opcode=CIUKI_CMD_MONO,.record_bytes=128,.dx=(int)(c->info.width-256),.dy=8,.dw=256,.dh=64,.sw=256,.sh=64,
         .stride=32,.data_bytes=sizeof(bits),.foreground=0x102030u+round,.background=0xfafafau-round},
        {.opcode=CIUKI_CMD_INVERT,.record_bytes=128,.dx=(int)(c->info.width-200),.dy=20,.dw=200,.dh=(unsigned)c->info.height-30},
        {.opcode=CIUKI_CMD_KEYED,.record_bytes=128,.dx=(int)(c->info.width-64),.dy=(int)(c->info.height-64),.dw=64,.dh=64,.key=0xff00ffu}};
    assert(!du_commands(&canvas,records,2,bits,NULL,0,0));
    assert(!du_commands(&canvas,records+2,1,NULL,icon,64,64));
    any_damage(lower,0,0,c->info.width,c->info.height);
}
static void covered_scenario(unsigned m, int shell)
{
    begin(m,shell);
    int lower=window_new(10,20,420,300,40,60);
    int upper=window_new(11,21,220,160,300,180);
    fill_pattern(lower,0x111111u); fill_pattern(upper,0x777777u);
    native_damage(lower,0,0,420,300); native_damage(upper,0,0,220,160);
    snprintf(step_name,sizeof(step_name),"setup"); frame();
    assert(d.clients[upper].order>d.clients[lower].order);
    hold(upper);
    struct desk_client *c=&d.clients[lower]; uint32_t *p=(uint32_t *)c->pixels; unsigned w=c->info.width, h=c->info.height;
    for (unsigned round=0;round<6;round++) {
        /* Animation: a rectangle that crosses under the upper window. */
        for (unsigned y=100;y<280;y++) for (unsigned x=200;x<420;x++) p[y*w+x]=(x*13u+y*7u+round*0x10101u)&0xffffffu;
        native_damage(lower,200,100,220,180); unchanged("lower animates under the upper window");
        /* Retained scroll (ScrollWindowEx): blit plus the exposed strip. */
        memmove(p,p+7*w,(size_t)(h-7)*w*4);
        for (unsigned k=(h-7)*w;k<h*w;k++) p[k]=(k*7631u+round)&0xffffffu;
        native_damage(lower,0,0,w,h); unchanged("lower scrolls up");
        memmove(p+5*w,p,(size_t)(h-5)*w*4);
        for (unsigned k=0;k<5*w;k++) p[k]=(k*1483u+round)&0xffffffu;
        native_damage(lower,0,0,w,h); unchanged("lower scrolls down");
        lower_draws_with_commands(lower,round); unchanged("lower draws MONO/INVERT/KEYED");
        /* Moves under the upper window, single and coalesced. */
        desk_move(&d,lower,40+(int)round*6,60+(int)round*3); unchanged("lower moves");
        desk_move(&d,lower,30+(int)round*4,70); desk_move(&d,lower,36+(int)round*5,64); unchanged("lower moves twice in a frame");
        /* A move whose frame also carries new client pixels. */
        desk_move(&d,lower,44,58+(int)round); p[17+round]^=0xffffffu; native_damage(lower,0,0,w,h);
        unchanged("lower moves and updates");
        /* DESK_SET_TITLE (shell-version clients): the caption repaint. */
        snprintf(c->title,sizeof(c->title),"%s",round&1 ? "Title" : "Other"); desk_caption_damage(&d,lower);
        unchanged("lower retitles");
    }
    /* Z-order change: the lower window is raised and now covers the old
     * upper one; that one's updates may not reach the raised window. */
    desk_focus(&d,lower); snprintf(step_name,sizeof(step_name),"raise"); frame();
    assert(d.clients[lower].order>d.clients[upper].order);
    hold(lower);
    uint32_t *q=(uint32_t *)d.clients[upper].pixels;
    for (unsigned round=0;round<4;round++) {
        for (unsigned k=0;k<220*160;k++) q[k]=(k*31u+round*0x30303u)&0xffffffu;
        native_damage(upper,0,0,220,160); unchanged("former upper animates after the raise");
        desk_move(&d,upper,300-(int)round*5,180+(int)round*4); unchanged("former upper moves after the raise");
    }
    free(held); held=NULL;
    end();
}

/* ---- z-order changes that bypass the raised window's own damage ---- */
static void restack_scenario(unsigned m)
{
    /* Found by the fuzz below (HEAD f9eb2b88 fails here): focusing a v3
     * owner raises it, then desk_v3_restack lifts its owned window above
     * every other window. The owned window's overlap with a native window
     * was never repainted: on screen the native window stayed on top while
     * the owned window, now higher, painted its next update over it. */
    begin(m,0);
    int native=window_new(10,20,300,200,240,120);
    v3_peer(30);
    int owner=v3_create(31,1,DESK_V3_OVERLAPPED,0,40,60,0x010203);
    int owned=v3_create(32,2,DESK_V3_OVERLAPPED,1,200,170,0x040506);
    snprintf(step_name,sizeof(step_name),"v3 setup"); frame();
    desk_focus(&d,native); snprintf(step_name,sizeof(step_name),"native raised"); frame();
    assert(d.clients[native].order>d.clients[owned].order);
    desk_focus(&d,owner); snprintf(step_name,sizeof(step_name),"owner raised, owned restacked"); frame();
    assert(d.clients[owned].order>d.clients[native].order && d.clients[owner].order>d.clients[native].order);
    for (unsigned round=0;round<3;round++) {
        fill_pattern(owned,round*0x0f0f0fu); any_damage(owned,0,0,200,100);
        snprintf(step_name,sizeof(step_name),"owned animates %u",round); frame();
        fill_pattern(native,round*0x050505u); native_damage(native,0,0,300,200);
        snprintf(step_name,sizeof(step_name),"native animates %u",round); frame();
    }
    /* A topmost v3 window and SetWindowPos-style reorders. */
    int topmost=v3_create(33,3,DESK_V3_OVERLAPPED|DESK_V3_TOPMOST,0,280,200,0x070809);
    snprintf(step_name,sizeof(step_name),"topmost created"); frame();
    desk_focus(&d,native); snprintf(step_name,sizeof(step_name),"native under topmost"); frame();
    assert(d.clients[topmost].order>d.clients[native].order);
    v3_window_order(owner,DESK_V3_ORDER_BOTTOM); snprintf(step_name,sizeof(step_name),"owner to bottom"); frame();
    v3_window_order(topmost,DESK_V3_ORDER_NOTOPMOST); snprintf(step_name,sizeof(step_name),"topmost cleared"); frame();
    desk_focus(&d,owned); snprintf(step_name,sizeof(step_name),"owned focused"); frame();
    end();
}
static void internal_scenario(unsigned m)
{
    /* The trial's shape: a shell window (Run, Programs, AutoPlay...) and
     * native windows are open when the first v3 window (NetSurf) starts.
     * desk_v3_restack then places every internal window above all others;
     * only the new v3 window's own frame was repainted. */
    begin(m,1);
    int run=desk_internal_open(&d,DESK_INTERNAL_RUN); assert(run>=0);
    snprintf(step_name,sizeof(step_name),"internal opened"); frame();
    struct desk_box f=d.clients[run].frame;
    int native=window_new(10,20,260,160,f.x+f.w/2,f.y+f.h/3);
    fill_pattern(native,0x222222u); native_damage(native,0,0,260,160);
    snprintf(step_name,sizeof(step_name),"native over internal"); frame();
    assert(d.clients[native].order>d.clients[run].order);
    v3_peer(30);
    int browser=v3_create(31,1,DESK_V3_OVERLAPPED,0,380,300,0x0a0b0c);
    snprintf(step_name,sizeof(step_name),"browser starts, internal restacked"); frame();
    assert(d.clients[run].order>d.clients[native].order && browser>=0);
    for (unsigned round=0;round<3;round++) {
        fill_pattern(native,round*0x030303u); native_damage(native,0,0,260,160);
        snprintf(step_name,sizeof(step_name),"native animates under internal %u",round); frame();
        desk_damage_add(&d.damage,desk_content(&d.clients[run]),d.width,d.height);
        snprintf(step_name,sizeof(step_name),"internal repaints %u",round); frame();
    }
    end();
}

/* ---- randomized sequences over every operation ---- */
static unsigned seed;
static unsigned rnd(unsigned n) { seed=seed*1103515245u+12345u; return (seed>>8)%n; }
static void fuzz(unsigned m, unsigned run)
{
    seed=run*7919u+m+1; fuzzing=1;
    begin(m,run&1);
    int win[6]; unsigned count=3;
    for (int k=0;k<3;k++) {
        win[k]=window_new(10+k,20+k,160+40*(unsigned)k,120+20*(unsigned)k,60+70*k,70+40*k);
        fill_pattern(win[k],(uint32_t)k*0x111111u); native_damage(win[k],0,0,d.clients[win[k]].info.width,d.clients[win[k]].info.height);
        snprintf(step_name,sizeof(step_name),"run=%u create %d",run,k); frame();
    }
    if (run&2) {
        v3_peer(30);
        win[3]=v3_create(31,1,DESK_V3_OVERLAPPED,0,100,90,0x010203);
        win[4]=v3_create(32,2,DESK_V3_OVERLAPPED,1,180,150,0x040506);
        win[5]=v3_create(33,3,(run&4) ? DESK_V3_OVERLAPPED|DESK_V3_TOPMOST : DESK_V3_OVERLAPPED,0,260,200,0x070809);
        count=6; snprintf(step_name,sizeof(step_name),"run=%u v3 windows",run); frame();
    }
    for (unsigned step=0;step<160;step++) {
        int i=win[rnd(count)]; struct desk_client *c=&d.clients[i];
        unsigned w=c->info.width, h=c->info.height, op=rnd(9); uint32_t *p=(uint32_t *)c->pixels;
        if (c->minimized && (op==3 || op==4 || op==8)) op=7; /* the shell restores first */
        snprintf(step_name,sizeof(step_name),"run=%u step=%u op=%u window=%d",run,step,op,i);
        if (getenv("OCC_TRACE")) { fprintf(stderr,"%s %s focus=%d",mode_names[m],step_name,d.focus);
            for (int k=0;k<DESK_CLIENTS;k++) if (d.clients[k].window) fprintf(stderr," [%d o=%llu f=%d,%d,%dx%d m=%d c=%d]",k,(unsigned long long)d.clients[k].order,
                d.clients[k].frame.x,d.clients[k].frame.y,d.clients[k].frame.w,d.clients[k].frame.h,d.clients[k].minimized,d.clients[k].collapsed);
            fprintf(stderr," damage=%u copy=%d\n",d.damage.count,d.copy_pending); }
        switch (op) {
        case 0: case 1: {
            unsigned x=rnd(w), y=rnd(h), ww=1+rnd(w-x), hh=1+rnd(h-y), v=rnd(0xffffff);
            for (unsigned yy=y;yy<y+hh;yy++) for (unsigned xx=x;xx<x+ww;xx++) p[yy*w+xx]=(v+xx*3+yy*7)&0xffffffu;
            any_damage(i,x,y,ww,hh); break;
        }
        case 2: {
            unsigned n=1+rnd(h/3);
            if (rnd(2)) { memmove(p,p+n*w,(size_t)(h-n)*w*4); for (unsigned k=(h-n)*w;k<h*w;k++) p[k]=(k*7631u+step)&0xffffffu; }
            else { memmove(p+n*w,p,(size_t)(h-n)*w*4); for (unsigned k=0;k<n*w;k++) p[k]=(k*1483u+step)&0xffffffu; }
            any_damage(i,0,0,w,h); break;
        }
        case 3: desk_move(&d,i,c->frame.x+(int)rnd(41)-20,c->frame.y+(int)rnd(41)-20); break;
        case 4: desk_focus(&d,i); break;
        case 5: if (!rnd(4)) desk_collapse(&d,i); break;
        case 6: if (d.shell && !rnd(4)) desk_minimize(&d,i); break;
        case 8: if (c->version==DESK_V3_VERSION && !rnd(3))
                    v3_window_order(i,rnd(2) ? DESK_V3_ORDER_BOTTOM : DESK_V3_ORDER_TOP);
                else if (!rnd(2)) lower_draws_with_commands(i,step);
                break;
        default: break;
        }
        if (rnd(3)) frame();
    }
    snprintf(step_name,sizeof(step_name),"run=%u final",run); frame();
    fuzzing=0; end();
}
#ifndef DESKTOP_OCCLUSION_NO_MAIN /* fx-15: window_model_test.c reuses the harness above */
static void report_abort(int sig)
{ (void)sig; fprintf(stderr,"aborted in mode %s at %s\n",mode_names[mode],step_name); signal(SIGABRT,SIG_DFL); }
int main(int argc, char **argv)
{
    signal(SIGABRT,report_abort);
    unsigned runs=argc>1 ? (unsigned)atoi(argv[1]) : 12;
    setvbuf(stdout,NULL,_IONBF,0);
    (void)command_redraw; /* batch_frame is its reporting copy */
    for (unsigned m=0;m<MODES;m++) {
        covered_scenario(m,0); covered_scenario(m,1);
        restack_scenario(m);
        internal_scenario(m);
        for (unsigned run=0;run<runs;run++) fuzz(m,run);
        printf("fx-2 %s: covering window unchanged under animation/scroll/move/retitle/MONO/INVERT/KEYED, "
               "z-order changes repainted, %u randomized sequences\n",mode_names[m],runs);
    }
    printf("PASS fx-2 occlusion: %u frames identical to the full composite, %u unoccluded client pixels equal their surfaces\n",
           frames_checked,visible_pixels_checked);
    return 0;
}
#endif
