/* F2-83 production bar surfaces, UTF-8 ellipsis and repaint parity. MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct desktop d;
static uint32_t portrait[256*256], pixel;
static const uint32_t *store_icon(const char *name, unsigned size)
{ return du_icon_argb(du_icon_name(name),size); }
static void full(void) { desk_composite(&d,(struct desk_box){0,0,d.width,d.height}); }
static void parity(void)
{
    assert(d.damage.count);
    for (unsigned i=0; i<d.damage.count; i++) desk_composite(&d,d.damage.boxes[i]);
    size_t bytes=(size_t)d.width*d.height*4;
    uint32_t *copy=malloc(bytes); assert(copy); memcpy(copy,d.pixels,bytes);
    full(); assert(!memcmp(copy,d.pixels,bytes)); free(copy); d.damage.count=0;
}
static void point(int x, int y)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=x-d.mouse_x,.value2=y-d.mouse_y};
    desk_input(&d,&e);
}
static void ellipsis(void)
{
    uint32_t a[160*32], b[160*32];
    struct desk_paint p={a,160,32,0,0,160,32}, q={b,160,32,0,0,160,32};
    const char *text="Ciuki caf\xc3\xa9 - a long window title";
    for (int font=DESK_FONT_UI; font<=DESK_FONT_BOLD; font++) for (int width=0; width<135; width++) {
        for (unsigned i=0; i<160*32; i++) a[i]=b[i]=0x123456;
        int advance=desk_paint_text_ellipsis(&p,font,5,20,text,width,0xffffff);
        assert(advance<=width);
        if (width>=desk_text_advance(font,0x2026)) {
            size_t n=desk_text_fit_chars(font,text,width-desk_text_advance(font,0x2026));
            int used=desk_paint_text_n(&q,font,5,20,text,n,0xffffff);
            desk_paint_text(&q,font,5+used,20,"\xe2\x80\xa6",0xffffff);
            assert(!memcmp(a,b,sizeof(a)));
        }
        for (int y=0; y<32; y++) for (int x=0; x<160; x++)
            if (x<5 || x>=5+width) assert(a[y*160+x]==0x123456);
    }
}
static void surfaces(int rgb565)
{
    uint32_t pixels[128*32], copy[128*32];
    struct desk_paint p={pixels,128,32,0,0,128,32};
    for (int kind=0; kind<2; kind++) {
        desk_paint_bar(&p,0,0,128,32,kind ? desk_theme->taskbar : desk_theme->topbar,rgb565);
        memcpy(copy,pixels,sizeof(copy));
        for (int y=4; y<31; y++) for (int x=0; x<128; x++) {
            printf("PAIR bar-%d %06x %06x\n",kind,
                kind ? desk_theme->taskbar_text : desk_theme->topbar_text,pixels[y*128+x]);
            if (y==4) continue;
            for (int shift=0; shift<24; shift+=8) {
                int bits=rgb565 ? (shift==8 ? 2 : 3) : 0;
                int a=((pixels[y*128+x]>>shift)&255)>>bits;
                int b=((pixels[(y-1)*128+x]>>shift)&255)>>bits;
                assert(abs(a-b)<=1);
            }
        }
        /* An arbitrary partial repaint retains the screen-anchored phase. */
        p.cx=3; p.cy=7; p.cw=65; p.ch=15;
        desk_paint_bar(&p,0,0,128,32,kind ? desk_theme->taskbar : desk_theme->topbar,rgb565);
        assert(!memcmp(pixels,copy,sizeof(copy)));
        p.cx=p.cy=0; p.cw=128; p.ch=32;
        if (rgb565) {
            unsigned mixed=0;
            for (int y=4; y<31; y++) for (int x=1; x<4; x++) mixed+=pixels[y*128+x]!=pixels[y*128];
            assert(mixed); /* A uniform quantized strip would conceal banding. */
            for (int shift=0; shift<24; shift+=8) for (int y=8; y<=24; y+=4) {
                int previous=0, next=0, bits=shift==8 ? 2 : 3;
                for (int dy=0; dy<4; dy++) for (int x=0; x<4; x++) {
                    previous+=((pixels[(y-4+dy)*128+x]>>shift)&255)>>bits;
                    next+=((pixels[(y+dy)*128+x]>>shift)&255)>>bits;
                }
                assert(abs(previous-next)<=16); /* <= one native step per band mean */
            }
        }
    }
    unsigned normal=0;
    for (unsigned state=0; state<8; state++) {
        desk_paint_fill(&p,0,0,128,32,desk_theme->taskbar);
        desk_paint_task(&p,0,0,128,26,state,rgb565);
        uint32_t face=pixels[8*128+64];
        unsigned value=((face>>16)&255)+((face>>8)&255)+(face&255);
        if (!state) normal=value;
        if (state==DESK_TASK_ACTIVE) assert(value<normal);
        if (state==DESK_TASK_HOT || state==DESK_TASK_MINIMIZED) assert(value>normal);
        for (int y=3; y<22; y++) for (int x=4; x<124; x++)
            printf("PAIR task-%u %06x %06x\n",state,
                state & DESK_TASK_ACTIVE ? desk_theme->task_active_text : desk_theme->taskbar_text,pixels[y*128+x]);
        memcpy(copy,pixels,sizeof(copy));
        p.cx=1; p.cy=2; p.cw=60; p.ch=19;
        /* Restore the substrate before blending the same fractional corners. */
        desk_paint_fill(&p,0,0,128,32,desk_theme->taskbar);
        desk_paint_task(&p,0,0,128,26,state,rgb565);
        assert(!memcmp(pixels,copy,sizeof(copy)));
        p.cx=p.cy=0; p.cw=128; p.ch=32;
    }
}
static void scene(const char *dir, unsigned width, unsigned height, int rgb565)
{
    const struct desk_theme *theme=desk_theme;
    fake_reset(); desk_init(&d,&fake_ops,width,height);
    d.portrait=portrait; d.icon=store_icon; d.hw_cursor=1; desk_shell_enable(&d);
    strcpy(d.sh.cfg.theme,theme->name); desk_shell_apply(&d);
    d.lfb_bytes_per_pixel=rgb565 ? 2 : 4;
    d.pixels=calloc((size_t)width*height,4); assert(d.pixels);
    d.realtime_second=12*3600+34*60; d.sh.cfg.clock_24h=1;
    static const char *const titles[]={"CiukNote - a very long document name", "Files - Documents and Games", "Terminal - minimized"};
    for (int i=0; i<3; i++) {
        struct desk_client *c=&d.clients[i];
        c->window=1; c->channel=-1; c->order=i+1; c->pixels=&pixel;
        c->info.width=c->info.height=1; c->frame=desk_frame(230,120,150+i*60,110+i*90);
        strcpy(c->title,titles[i]);
    }
    d.focus=0; d.clients[2].minimized=1;
    full(); d.damage.count=0;
    struct desk_box task=desk_task_button(&d,1);
    point(task.x+task.w/2,task.y+task.h/2);
    assert(d.sh.hover_target==17); parity();
    char path[1024];
    snprintf(path,sizeof(path),"%s/%s-%ux%u-%dbpp.xrgb",dir,
        !strcmp(theme->name,"Platinum") ? "platinum" : "ciuki-blue",width,height,rgb565 ? 16 : 32);
    FILE *f=fopen(path,"wb"); size_t bytes=(size_t)width*height*4;
    assert(f && fwrite(d.pixels,1,bytes,f)==bytes && !fclose(f));
    desk_menu_open(&d,SH_MENU_TASK,task.x,task.y); parity();
    desk_menu_close(&d); parity();
    point(650,80); parity();
    desk_menu_open(&d,SH_MENU_START,0,0); parity();
    free(d.pixels); free(d.sh.background); free(d.sh.wall);
}
int main(int argc, char **argv)
{
    assert(argc==3);
    FILE *f=fopen(argv[1],"rb"); assert(f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    ellipsis();
    for (int theme=0; theme<desk_theme_count(); theme++) for (int bpp=0; bpp<2; bpp++) {
        assert(!desk_theme_use(desk_theme_at(theme)->name));
        surfaces(bpp);
        scene(argv[2],1024,768,bpp); scene(argv[2],800,600,bpp);
    }
    puts("PASS bar band steps, dither phase, UTF-8 ellipsis and hover/menu incremental parity");
    return 0;
}
