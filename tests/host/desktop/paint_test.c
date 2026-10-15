/* CiukPaint production-code harness: real libdeskui input and pixel assertions.
 * SPDX-License-Identifier: MIT */
#include "../../../apps/paint/paint.h"
#include "../../../apps/desktop/catalog.h"
#include "../../../assets/cursors/paint.h"
#include "acc_test.h"
#include "../../../third_party/stb/stb_image_write.h"
#include "../../../third_party/stb/stb_image.h"
#include <errno.h>
#include <sys/stat.h>
static struct du_ui ui;
static uint32_t pixels[PAINT_W*PAINT_H+1];
static int include_cursor;
static struct paint_state state(void) { struct paint_state s; paint_snapshot(&s); return s; }
static void button(int n,int down)
{ struct ciuki_input_event e={.type=CIUKI_INPUT_BUTTON,.code=n,.value=down}; du_input(&ui,&e); }
static void image_move(int x,int y)
{ struct paint_state s=state(); at_move(&ui,s.view.x+4+(x-s.scroll_x)*s.zoom,s.view.y+4+(y-s.scroll_y)*s.zoom); }
static void stroke(int t,int x,int y,int xx,int yy,int b)
{
    at_click_widget(&ui,P_TOOL+t); image_move(x,y); button(b,1); image_move(xx,yy); button(b,0);
}
static void image_click(int x,int y,int b) { image_move(x,y); button(b,1); button(b,0); }
static void opt(int col,int row)
{ struct paint_state s=state(); at_click(&ui,s.options.x+2+col,s.options.y+2+row); }
static void dialog_button(unsigned b)
{ struct du_rect r=du_dialog_button(&ui,b); at_click(&ui,r.x+r.w/2,r.y+r.h/2); }
static void attributes(int w,int h)
{
    paint_command(M_ATTR); char buf[12];
    snprintf(buf,sizeof(buf),"%d",w); du_set_text(&ui.dialog.items[3],buf);
    snprintf(buf,sizeof(buf),"%d",h); du_set_text(&ui.dialog.items[5],buf);
    dialog_button(0); assert(!ui.dialog.kind && state().width==w && state().height==h);
}
static void foreground(int sw) { at_click_widget(&ui,P_SWATCH+sw); }
static void background(int sw)
{ struct du_rect r=du_get(&ui,P_SWATCH+sw)->box; at_move(&ui,r.x+7,r.y+7); button(2,1); button(2,0); }
static void undo(void) { at_chord(&ui,DU_KEY_LCTRL,0x2c); }
static void redo(void) { at_chord(&ui,DU_KEY_LCTRL,0x15); }
static void transform_choice(int n)
{ paint_command(M_FLIP); struct du_rect r=ui.dialog.items[1+n].box; at_click(&ui,r.x+12,r.y+9); dialog_button(0); }
static void render_write(void *f,void *data,int n) { assert(fwrite(data,1,(size_t)n,f)==(size_t)n); }
static void render(const char *folder,const char *name)
{
    du_render(&ui); assert(pixels[PAINT_W*PAINT_H]==0xdeadbeefu);
    if(include_cursor) {
        struct desk_tool_cursor r=desk_tool_cursor(du_cursor_at(&ui));
        for(int y=0;y<32;y++)for(int x=0;x<32;x++) {
            int xx=ui.mouse_x-r.hot_x+x,yy=ui.mouse_y-r.hot_y+y; uint32_t bit=0x80000000u>>x;
            if(xx>=0 && yy>=0 && xx<ui.canvas.width && yy<ui.canvas.height && (r.mask[y]&bit))
                ui.canvas.pixels[(size_t)yy*ui.canvas.width+xx]=(r.image[y]&bit)?0xffffffu:0x101010u;
        }
    }
    char p[1100]; snprintf(p,sizeof(p),"%s/%s.png",folder,name);
    size_t n=(size_t)ui.canvas.width*(size_t)ui.canvas.height;
    uint8_t *data=malloc(n*3); assert(data);
    for (size_t i=0;i<n;i++) { data[i*3]=(uint8_t)(ui.canvas.pixels[i]>>16); data[i*3+1]=(uint8_t)(ui.canvas.pixels[i]>>8); data[i*3+2]=(uint8_t)ui.canvas.pixels[i]; }
    FILE *f=fopen(p,"wb"); assert(f);
    assert(stbi_write_png_to_func(render_write,f,ui.canvas.width,ui.canvas.height,3,data,ui.canvas.width*3));
    assert(!fclose(f)); free(data);
}
static void stroke_continuity(void)
{
    const char *folder="build/host/f2-71";
    assert(!mkdir(folder,0755) || errno==EEXIST);
    paint_command(M_Z1); attributes(96,72); paint_command(M_CLEARIMG); foreground(2);
    render(folder,"before");
    at_click_widget(&ui,P_TOOL+T_PENCIL); image_move(3,3); button(1,1); image_move(82,61); button(1,0);
    int x=3,y=3,dx=79,dy=-58,sx=1,sy=1,err=dx+dy;
    for (;;) {
        assert(paint_pixel(x,y)==state().foreground);
        if (x==82 && y==61) break;
        int e2=2*err;
        if (e2>=dy) { err+=dy; x+=sx; }
        if (e2<=dx) { err+=dx; y+=sy; }
    }
    render(folder,"after");
    puts("paint stroke continuity: PASS fast diagonal pencil segment has no gaps");
}
static void write_raw(const char *path,const void *data,size_t n)
{ FILE *f=fopen(path,"wb"); assert(f && fwrite(data,1,n,f)==n && !fclose(f)); }
static void lasso(void)
{
    at_click_widget(&ui,P_TOOL+T_SELECT); opt(1,20); assert(state().free_select);
    image_move(20,20); button(1,1); image_move(70,20); image_move(20,70); image_move(20,20); button(1,0);
    assert(state().selection && du_cursor_at(&ui)==DESK_CURSOR_CROSSHAIR);
}
static void freeform_and_text(const char *folder)
{
    paint_command(M_Z1); paint_command(M_CLEARIMG); foreground(2);
    uint8_t red=state().foreground,bg=state().background;
    at_click_widget(&ui,P_TOOL+T_RECT); opt(1,51); stroke(T_RECT,20,20,70,70,1);
    lasso(); render(folder,"paint-freeform-selection");
    paint_command(M_COPY); paint_command(M_CLEARSEL);
    assert(paint_pixel(25,25)==bg && paint_pixel(65,65)==red);
    undo(); assert(paint_pixel(25,25)==red);
    lasso(); image_move(25,25); button(1,1); image_move(35,35); button(1,0);
    assert(state().floating && paint_pixel(25,25)==bg && paint_pixel(35,35)==red && paint_pixel(65,65)==red);
    render(folder,"paint-freeform-move"); undo(); assert(paint_pixel(25,25)==red && !state().selection);
    paint_command(M_CLEARIMG); paint_command(M_PASTE);
    assert(paint_pixel(5,5)==red && paint_pixel(45,45)==bg);
    transform_choice(3); at_tap(&ui,DU_KEY_ESC); undo();
    lasso(); image_move(80,80); button(1,1); image_move(100,100);
    struct ciuki_input_event sync={.type=CIUKI_INPUT_RESYNC}; du_input(&ui,&sync); assert(!state().selection);
    at_click_widget(&ui,P_TOOL+T_TEXT); image_click(10,90,1);
    du_clipboard_set("alpha beta\nline"); at_chord(&ui,DU_KEY_LCTRL,0x2f); assert(state().text_bytes==15);
    at_chord(&ui,DU_KEY_LCTRL,0x1e); paint_command(M_COPY); assert(!strcmp(du_clipboard_get(),"alpha beta\nline"));
    paint_command(M_CUT); assert(!state().text_bytes); paint_command(M_PASTE); assert(state().text_bytes==15);
    ui.now+=1000000000ull; image_click(12,92,1); ui.now+=100000000ull; image_click(12,92,1);
    assert(state().text_anchor==0 && state().text_cursor==5);
    ui.now+=100000000ull; image_click(12,92,1); assert(state().text_anchor==0 && state().text_cursor==11);
    render(folder,"paint-text-selection");
    at_tap(&ui,0x2c); assert(state().text_bytes==5); at_tap(&ui,DU_KEY_ESC);
    puts("paint selection: PASS free-form mask, copy, clear, move, undo, transparent paste/rotate, capture cancellation, text clipboard and word/line selection");
}
static void cursors(const char *folder)
{
    static const unsigned expected[NTOOLS]={DESK_CURSOR_CROSSHAIR,DESK_CURSOR_ERASER,DESK_CURSOR_FILL,
        DESK_CURSOR_PICK,DESK_CURSOR_ZOOM,DESK_CURSOR_PENCIL,DESK_CURSOR_BRUSH,DESK_CURSOR_PRECISION,
        DESK_CURSOR_TEXT,DESK_CURSOR_CROSSHAIR,DESK_CURSOR_CROSSHAIR,DESK_CURSOR_CROSSHAIR,
        DESK_CURSOR_CROSSHAIR,DESK_CURSOR_CROSSHAIR,DESK_CURSOR_CROSSHAIR};
    paint_command(M_Z1); include_cursor=1;
    for(int t=0;t<NTOOLS;t++) {
        at_click_widget(&ui,P_TOOL+t); image_move(40,40); assert(du_cursor_at(&ui)==expected[t]);
        char name[64]; snprintf(name,sizeof(name),"paint-cursor-%02d",t); render(folder,name);
        at_move(&ui,10,15); assert(du_cursor_at(&ui)==DESK_CURSOR_ARROW);
    }
    at_click_widget(&ui,P_TOOL+T_PENCIL); image_move(40,40); button(1,1);
    at_move(&ui,10,15); assert(du_cursor_at(&ui)==DESK_CURSOR_ARROW); button(1,0);
    for(int z=1;z<=8;z*=2) {
        paint_command(z==1?M_Z1:z==2?M_Z2:z==4?M_Z4:M_Z8);
        for(int t=T_ERASER;t<=T_BRUSH;t+=T_BRUSH-T_ERASER) {
            at_click_widget(&ui,P_TOOL+t); image_move(40,40);
            uint8_t before[200*150]; struct paint_state s=state();
            assert(s.width==200 && s.height==150);
            for(int y=0;y<s.height;y++)for(int x=0;x<s.width;x++)before[y*s.width+x]=paint_pixel(x,y);
            char name[64]; snprintf(name,sizeof(name),"paint-footprint-%d-%dx",t,z); render(folder,name);
            for(int y=0;y<s.height;y++)for(int x=0;x<s.width;x++)assert(before[y*s.width+x]==paint_pixel(x,y));
            assert(s.dirty==state().dirty && s.can_undo==state().can_undo);
        }
    }
    paint_command(M_Z1); image_move(40,40); paint_command(M_ATTR);
    assert(du_cursor_at(&ui)!=DESK_CURSOR_BRUSH); dialog_button(1);
    include_cursor=0;
    puts("paint cursors: PASS every tool, arrow outside image including capture, modal override, exact raster hotspots, footprint leaves image and history unchanged at 1/2/4/8x");
}
static void footprint_matches_stamp(void)
{
    uint32_t *preview=malloc(sizeof(pixels)),*base=malloc(sizeof(pixels)); assert(preview && base); include_cursor=0;
    foreground(2); uint8_t bg=state().background;
    const int zooms[]={1,4,8};
    for(unsigned z=0;z<3;z++)for(int option=0;option<16;option++) {
        paint_command(zooms[z]==1?M_Z1:zooms[z]==4?M_Z4:M_Z8); at_tap(&ui,DU_KEY_HOME);
        paint_command(M_CLEARIMG);
        int eraser=option>=12; uint8_t under=bg;
        if(eraser) { at_click_widget(&ui,P_TOOL+T_FILL); image_click(0,0,1); under=state().foreground; }
        at_click_widget(&ui,P_TOOL+(eraser?T_ERASER:T_BRUSH));
        if(eraser)opt(1,(option-12)*19+1); else opt((option%3)*16+1,(option/3)*19+1);
        du_render(&ui); memcpy(base,pixels,sizeof(pixels));
        image_move(40,40); du_render(&ui); memcpy(preview,pixels,sizeof(pixels));
        button(1,1); button(1,0); struct paint_state s=state();
        for(int y=30;y<51;y++)for(int x=30;x<51;x++) {
            int member=paint_pixel(x,y)!=under;
            int edge[4]={paint_pixel(x-1,y)==under,paint_pixel(x+1,y)==under,paint_pixel(x,y-1)==under,paint_pixel(x,y+1)==under};
            int sx=s.view.x+4+(x-s.scroll_x)*s.zoom,sy=s.view.y+4+(y-s.scroll_y)*s.zoom;
            for(int yy=0;yy<s.zoom;yy++)for(int xx=0;xx<s.zoom;xx++) {
                if(!du_contains(du_get(&ui,P_CANVAS)->cursor_box,sx+xx,sy+yy))continue;
                int selected=member && ((edge[0] && !xx) || (edge[1] && xx==s.zoom-1) || (edge[2] && !yy) || (edge[3] && yy==s.zoom-1));
                unsigned at=(unsigned)((sy+yy)*PAINT_W+sx+xx);
                uint32_t expected=base[at]^(selected?0xffffffu:0);
                if(preview[at]!=expected) {
                    fprintf(stderr,"footprint option=%d zoom=%d image=%d,%d sub=%d,%d member=%d got=%06x expected=%06x\n",
                            option,s.zoom,x,y,xx,yy,member,preview[at],expected); abort();
                }
            }
        }
    }
    free(preview); free(base);
    puts("paint footprint: PASS preview boundary equals actual stamped pixels for all 12 brushes and four eraser sizes at 1/4/8x");
}
static void tools(void)
{
    attributes(96,72); foreground(2); uint8_t red=state().foreground, white=paint_pixel(0,0);
    stroke(T_PENCIL,2,2,12,2,1); for (int x=2;x<=12;x++) assert(paint_pixel(x,2)==red);
    undo(); assert(paint_pixel(2,2)==white && state().can_redo); redo(); assert(paint_pixel(12,2)==red);
    foreground(6); uint8_t blue=state().foreground;
    stroke(T_PENCIL,2,4,12,4,2); assert(paint_pixel(2,4)==state().background);
    background(16); uint8_t back=state().background;
    stroke(T_PENCIL,2,6,12,6,2); assert(paint_pixel(2,6)==back);
    /* Either button retains capture across the canvas edge and ignores an
     * unrelated button release. */
    image_move(2,8); button(2,1); button(1,1); button(1,0); assert(ui.capture>=0);
    image_move(20,8); button(2,0); assert(paint_pixel(20,8)==back && ui.capture<0);
    stroke(T_LINE,3,12,25,12,1); assert(paint_pixel(12,12)==blue);
    at_click_widget(&ui,P_TOOL+T_LINE); opt(10,31); /* width 3 */
    stroke(T_LINE,3,17,25,17,1); assert(paint_pixel(12,16)==blue && paint_pixel(12,18)==blue);
    /* Shift constrains a rectangle to a square. */
    at_key(&ui,DU_KEY_LSHIFT,1); stroke(T_RECT,30,3,50,12,1); at_key(&ui,DU_KEY_LSHIFT,0);
    assert(paint_pixel(39,12)==blue && paint_pixel(45,8)==white);
    at_click_widget(&ui,P_TOOL+T_LINE); opt(10,0); /* width 1 */
    at_click_widget(&ui,P_TOOL+T_RECT); opt(10,26); /* outline + background */
    stroke(T_RECT,30,18,45,28,1); assert(paint_pixel(30,18)==blue && paint_pixel(35,23)==back);
    opt(10,51); stroke(T_RECT,48,18,58,28,1); assert(paint_pixel(53,23)==blue);
    stroke(T_ELLIPSE,62,18,82,34,1); assert(paint_pixel(72,26)==blue && paint_pixel(62,18)==white);
    stroke(T_RRECT,62,38,86,56,1); assert(paint_pixel(72,46)==blue && paint_pixel(62,38)==white);
    /* Filled outline remains four-connected. */
    at_click_widget(&ui,P_TOOL+T_RECT); opt(10,0); stroke(T_RECT,3,32,20,49,1);
    foreground(2); image_click(0,0,1); /* Rectangle single point */
    at_click_widget(&ui,P_TOOL+T_FILL); image_click(10,40,1);
    assert(paint_pixel(10,40)==red && paint_pixel(2,40)==white && paint_pixel(3,40)==blue);
    undo(); assert(paint_pixel(10,40)==white); redo(); assert(paint_pixel(10,40)==red);
    stroke(T_BRUSH,30,40,40,40,1); assert(paint_pixel(35,40)==red && paint_pixel(35,39)==red);
    /* Square and both diagonal brush options are reachable. */
    opt(1,20); stroke(T_BRUSH,30,53,40,53,1); assert(paint_pixel(35,53)==red);
    opt(1,39); stroke(T_BRUSH,45,40,45,40,1); assert(paint_pixel(45,40)==red);
    opt(1,59); stroke(T_BRUSH,45,54,45,54,1); assert(paint_pixel(45,54)==red);
    stroke(T_ERASER,35,40,35,40,2); assert(paint_pixel(35,40)==back); /* replace foreground only */
    stroke(T_ERASER,3,32,3,32,2); assert(paint_pixel(3,32)==blue);
    stroke(T_ERASER,3,32,3,32,1); assert(paint_pixel(3,32)==back);
    /* Airbrush sprays while held, including without pointer motion. */
    at_click_widget(&ui,P_TOOL+T_AIR); image_move(35,64); button(1,1);
    unsigned before=0,after=0; for (int y=57;y<72;y++) for(int x=22;x<48;x++) before+=paint_pixel(x,y)==red;
    du_tick(&ui,ui.now+220000000ull); button(1,0);
    for (int y=57;y<72;y++) for(int x=22;x<48;x++) after+=paint_pixel(x,y)==red;
    assert(after>before);
    at_click_widget(&ui,P_TOOL+T_PICK); image_click(10,40,1); assert(state().foreground==red && state().tool==T_AIR);
    at_click_widget(&ui,P_TOOL+T_PICK); image_click(12,12,2); assert(state().background==blue);
    /* Three-stage cubic curve, one undo operation. */
    foreground(6); stroke(T_CURVE,48,4,80,4,1);
    image_move(56,12); button(1,1); image_move(56,12); button(1,0);
    image_move(72,12); button(1,1); image_move(72,12); button(1,0);
    assert(!state().operation && paint_pixel(48,4)==blue); undo(); assert(paint_pixel(48,4)==white); redo();
    /* Polygon closes on the starting point; Esc cancels a preview. */
    at_click_widget(&ui,P_TOOL+T_POLY); opt(10,51); stroke(T_POLY,60,58,85,58,1);
    image_click(85,69,1); image_click(60,58,1); assert(paint_pixel(78,61)==blue && !state().operation);
    stroke(T_LINE,3,60,20,60,1); undo();
    at_click_widget(&ui,P_TOOL+T_LINE); image_move(3,60); button(1,1); image_move(20,60);
    struct ciuki_input_event sync={.type=CIUKI_INPUT_RESYNC}; du_input(&ui,&sync);
    assert(!state().operation && paint_pixel(10,60)!=blue && ui.capture<0);
    /* One level: a later stroke replaces the previous history. */
    stroke(T_PENCIL,90,2,90,3,1); stroke(T_PENCIL,92,2,92,3,1); undo();
    assert(paint_pixel(90,2)==blue && paint_pixel(92,2)==white); undo(); assert(paint_pixel(90,2)==blue);
    redo(); assert(paint_pixel(92,2)==blue);
}
static void selection_and_text(void)
{
    background(14); paint_command(M_CLEARIMG); foreground(2); uint8_t red=state().foreground, white=state().background;
    at_click_widget(&ui,P_TOOL+T_RECT); opt(5,51); stroke(T_RECT,2,2,7,7,1);
    stroke(T_SELECT,2,2,7,7,1); assert(state().selection && state().selection_w==6);
    at_key(&ui,DU_KEY_LCTRL,1); image_move(3,3); button(1,1); image_move(13,13); button(1,0); at_key(&ui,DU_KEY_LCTRL,0);
    assert(state().floating && paint_pixel(2,2)==red && paint_pixel(12,12)==red);
    at_tap(&ui,DU_KEY_ESC); assert(!state().selection);
    undo(); assert(paint_pixel(12,12)==white && paint_pixel(2,2)==red); redo();
    stroke(T_SELECT,2,2,7,7,1); at_chord(&ui,DU_KEY_LCTRL,0x2e); /* copy */
    at_chord(&ui,DU_KEY_LCTRL,0x2d); assert(paint_pixel(2,2)==white && !state().selection);
    at_chord(&ui,DU_KEY_LCTRL,0x2f); assert(state().floating && paint_pixel(0,0)==red);
    at_tap(&ui,DU_KEY_RIGHT); at_tap(&ui,DU_KEY_DOWN); assert(state().selection_x==1 && state().selection_y==1);
    at_tap(&ui,DU_KEY_ESC); assert(paint_pixel(1,1)==red);
    undo(); assert(paint_pixel(1,1)==white); redo();
    /* Whole image flips and rotate, including a dimension-changing undo. */
    transform_choice(1); assert(paint_pixel(94,1)==red); undo(); assert(paint_pixel(1,1)==red);
    transform_choice(2); assert(paint_pixel(1,70)==red); undo();
    transform_choice(3); assert(state().width==72 && state().height==96 && paint_pixel(70,1)==red);
    undo(); assert(state().width==96 && state().height==72 && paint_pixel(1,1)==red); redo(); undo();
    transform_choice(4); assert(paint_pixel(94,70)==red); undo();
    transform_choice(5); assert(state().width==72 && state().height==96 && paint_pixel(1,94)==red); undo();
    uint32_t old=paint_rgb(paint_pixel(1,1)); paint_command(M_INVERT); assert(paint_rgb(paint_pixel(1,1))!=old); undo();
    /* Selection transform keeps the hole and can be cancelled/undone. */
    stroke(T_SELECT,1,1,6,6,1); transform_choice(3); assert(state().floating); at_tap(&ui,DU_KEY_ESC); undo();
    /* CFN text on a live preview, editing keys, opaque and transparent. */
    paint_command(M_CLEARIMG); at_click_widget(&ui,P_TOOL+T_TEXT); image_click(10,10,1);
    assert(state().text); at_tap(&ui,0x1e); at_tap(&ui,0x30); /* ab */
    unsigned count=0; for (int y=10;y<26;y++) for(int x=10;x<32;x++) count+=paint_pixel(x,y)==red; assert(count>0);
    at_tap(&ui,DU_KEY_ENTER); at_tap(&ui,0x2e); at_tap(&ui,DU_KEY_BACKSPACE); at_tap(&ui,0x20);
    at_chord(&ui,DU_KEY_LCTRL,0x1e); at_chord(&ui,DU_KEY_LCTRL,0x2e); at_tap(&ui,DU_KEY_END);
    paint_command(M_FONT); assert(ui.dialog.items[2].count>=1); du_set_text(&ui.dialog.items[4],"2");
    at_click(&ui,ui.dialog.items[5].box.x+5,ui.dialog.items[5].box.y+8); dialog_button(0);
    assert(state().text); image_click(75,60,1); assert(!state().text && state().can_undo);
    undo(); assert(paint_pixel(12,12)==white); redo();
    image_click(10,10,1); at_tap(&ui,0x1e); at_tap(&ui,DU_KEY_ESC); assert(!state().text);
    /* Size validation and Default never discard pixels on invalid input. */
    paint_command(M_ATTR); du_set_text(&ui.dialog.items[3],"0"); dialog_button(0); assert(ui.dialog.kind);
    dialog_button(2); assert(!strcmp(ui.dialog.items[3].text,"512")); dialog_button(1); assert(!ui.dialog.kind);
    attributes(112,80); assert(state().can_undo); undo(); assert(state().width==96 && state().height==72); redo();
    /* Last edit is still available across relayout, visible chrome toggles. */
    paint_command(M_Z4); at_chord(&ui,DU_KEY_LCTRL,0x22); assert(state().zoom==4);
    at_click_widget(&ui,P_TOOL+T_ZOOM); image_click(20,20,1); assert(state().zoom==6);
    at_click_widget(&ui,P_TOOL+T_ZOOM); image_click(20,20,2); assert(state().zoom==4);
    paint_command(M_TOOLBOX); assert(du_get(&ui,P_TOOL)->hidden); paint_command(M_TOOLBOX);
    paint_command(M_COLORBOX); assert(du_get(&ui,P_SWATCH)->hidden); paint_command(M_COLORBOX);
    paint_command(M_STATUS); assert(du_get(&ui,P_STATUS)->hidden); paint_command(M_STATUS);
    paint_command(M_Z8); at_tap(&ui,DU_KEY_END); assert(state().scroll_y>0); at_tap(&ui,DU_KEY_HOME);
}
static void put32(uint8_t *b,uint32_t n)
{ for(int i=0;i<4;i++) b[i]=(uint8_t)(n>>(i*8)); }
static void codecs(const char *scratch)
{
    uint8_t pal[PAINT_COLORS*3];
    for(int i=0;i<PAINT_COLORS;i++) { uint32_t c=paint_rgb((uint8_t)i); pal[i*3]=(uint8_t)(c>>18); pal[i*3+1]=(uint8_t)((c>>10)&63); pal[i*3+2]=(uint8_t)((c>>2)&63); }
    char path[1100]; snprintf(path,sizeof(path),"%s/fixture.bmp",scratch);
    /* Independently constructed BMPs: packed indices, padded rows, both row
     * orders, BGR24 and BGRX32. The legacy reader ignores the fourth byte. */
    const int depths[]={1,4,8,24,32};
    for(unsigned k=0;k<sizeof(depths)/sizeof(depths[0]);k++) for(int top=0;top<2;top++) {
        uint8_t bmp[1100]={0}; int bits=depths[k],colors=bits<=8?2:0,off=54+colors*4;
        unsigned stride=((3u*(unsigned)bits+31)/32)*4,size=(unsigned)off+stride*2;
        bmp[0]='B';bmp[1]='M';put32(bmp+2,size);put32(bmp+10,(uint32_t)off);put32(bmp+14,40);
        put32(bmp+18,3);put32(bmp+22,top?(uint32_t)-2:2);bmp[26]=1;bmp[28]=(uint8_t)bits;put32(bmp+46,(uint32_t)colors);
        if(colors) { bmp[54+4]=255;bmp[54+5]=255;bmp[54+6]=255; }
        for(int y=0;y<2;y++) for(int x=0;x<3;x++) {
            unsigned white=(unsigned)((x+y)&1),offset=(unsigned)off+(unsigned)(top?y:1-y)*stride;
            if(bits==1) bmp[offset]|=(uint8_t)(white<<(7-x));
            else if(bits==4) bmp[offset+(unsigned)x/2]|=(uint8_t)(white<<(x&1?0:4));
            else if(bits==8) bmp[offset+(unsigned)x]=(uint8_t)white;
            else { for(int j=0;j<3;j++) bmp[offset+(unsigned)x*(unsigned)(bits/8)+(unsigned)j]=(uint8_t)(white?255:0); if(bits==32) bmp[offset+(unsigned)x*4+3]=64; }
        }
        write_raw(path,bmp,size); struct paint_image im={0}; int result=paint_read_image(path,pal,&im);
        if(result) fprintf(stderr,"BMP%d top=%d: error %d (%s)\n",bits,top,result,stbi_failure_reason());
        assert(!result);
        assert(im.w==3 && im.h==2);
        for(int y=0;y<2;y++) for(int x=0;x<3;x++) {
            uint32_t c=paint_rgb(im.pixels[y*3+x]); assert(c==((x+y)&1?0xffffffu:paint_rgb(79)));
        }
        free(im.pixels);
        write_raw(path,bmp,size-1); assert(paint_read_image(path,pal,&im)==-EINVAL);
        put32(bmp+18,2049);write_raw(path,bmp,size);assert(paint_read_image(path,pal,&im)==-EFBIG);
    }
    /* PNG alpha is composited over white. */
    snprintf(path,sizeof(path),"%s/alpha.png",scratch); uint8_t rgba[]={255,0,0,0,255,255,255,255};
    FILE *f=fopen(path,"wb"); assert(f); assert(stbi_write_png_to_func(render_write,f,2,1,4,rgba,8));assert(!fclose(f));
    struct paint_image im={0}; assert(!paint_read_image(path,pal,&im) && paint_rgb(im.pixels[0])==0xffffffu && paint_rgb(im.pixels[1])==0xffffffu);free(im.pixels);
    /* Failure preserves an existing target and removes its temporary file. */
    snprintf(path,sizeof(path),"%s/keep.bmp",scratch);write_raw(path,"KEEP",4);
    uint8_t invalid=255;im=(struct paint_image){1,1,&invalid}; assert(paint_write_image(path,PAINT_BMP8,&im,pal)==-EINVAL);
    uint8_t head[4];f=fopen(path,"rb");assert(f && fread(head,1,4,f)==4 && !memcmp(head,"KEEP",4));fclose(f);
    /* Tiny default-format file is the legacy BITMAPINFOHEADER and BGR palette. */
    uint8_t two[]={80,79,80,79,80,79};im=(struct paint_image){3,2,two};assert(!paint_write_image(path,PAINT_BMP8,&im,pal));
    uint8_t bmp[70];f=fopen(path,"rb");assert(f && fread(bmp,1,70,f)==70 && fgetc(f)==EOF);fclose(f);
    assert(bmp[10]==62 && bmp[28]==8 && bmp[46]==2 && bmp[54]==paint_rgb(79)%256 && bmp[58]==255);
    assert(bmp[62]==0 && bmp[63]==1 && bmp[64]==0 && bmp[65]==0 && bmp[66]==1 && bmp[67]==0 && bmp[68]==1 && bmp[69]==0);
}
static void files(const char *scratch)
{
    char path[1100],bad[1100]; snprintf(bad,sizeof(bad),"%s/broken.bmp",scratch); write_raw(bad,"not a bitmap",12);
    const char *names[]={"original.bmp","rgb.bmp","drawing.png"};
    struct paint_state s=state(); size_t n=(size_t)s.width*(size_t)s.height;
    uint32_t *reference=malloc(n*4); assert(reference);
    for(size_t i=0;i<n;i++) reference[i]=paint_rgb(paint_pixel((int)(i%(size_t)s.width),(int)(i/(size_t)s.width)));
    for(int f=0;f<3;f++) {
        snprintf(path,sizeof(path),"%s/%s",scratch,names[f]); assert(!paint_save(path,(enum paint_format)f));
        assert(!state().dirty && !strcmp(paint_path(),path));
        FILE *file=fopen(path,"rb"); uint8_t head[54]; assert(file && fread(head,1,54,file)==54); fclose(file);
        if (f<2) assert(head[0]=='B' && head[1]=='M' && head[28]==(f?24:8)); else assert(!memcmp(head,"\x89PNG\r\n\x1a\n",8));
        assert(!paint_open(path)); assert(state().width==s.width && state().height==s.height && !state().can_undo);
        for(size_t i=0;i<n;i++) assert(paint_rgb(paint_pixel((int)(i%(size_t)s.width),(int)(i/(size_t)s.width)))==reference[i]);
    }
    assert(paint_open(bad)==-EINVAL && state().width==s.width); /* old image retained */
    snprintf(path,sizeof(path),"%s/no-directory/fail.png",scratch);
    foreground(2); stroke(T_PENCIL,1,1,4,1,1); assert(state().dirty);
    assert(paint_save(path,PAINT_PNG)==-ENOENT && state().dirty);
    /* Real Save As format picker, native file dialog and overwrite prompt. */
    paint_command(M_SAVEAS); assert(ui.dialog.kind && ui.dialog.items[3].kind==DU_RADIO);
    struct du_rect radio=ui.dialog.items[3].box; at_click(&ui,radio.x+5,radio.y+8); dialog_button(0);
    assert(ui.dialog.kind==DU_DLG_SAVE && strstr(ui.dialog.items[5].text,".bmp"));
    snprintf(path,sizeof(path),"%s/ui-export.bmp",scratch); du_set_text(&ui.dialog.items[5],path); dialog_button(0);
    assert(!ui.dialog.kind && !strcmp(paint_path(),path));
    FILE *exported=fopen(path,"rb"); uint8_t header[54];assert(exported && fread(header,1,54,exported)==54 && header[28]==24);fclose(exported);
    paint_command(M_SAVEAS); dialog_button(0); du_set_text(&ui.dialog.items[5],path); dialog_button(0);
    assert(ui.dialog.kind==DU_DLG_MESSAGE); dialog_button(0); assert(!ui.dialog.kind);
    /* Open and New from their menu accelerators. */
    at_chord(&ui,DU_KEY_LCTRL,0x18); assert(ui.dialog.kind==DU_DLG_OPEN);
    du_set_text(&ui.dialog.items[5],path); dialog_button(0); assert(!ui.dialog.kind && !state().dirty);
    at_chord(&ui,DU_KEY_LCTRL,0x31); assert(!paint_path()[0] && !state().dirty);
    stroke(T_PENCIL,1,1,4,1,1); assert(state().dirty);
    assert(!strcmp(desk_catalog_file("/Documents/Art.BmP")->name,"viewer"));
    assert(!strcmp(desk_catalog_file("picture.PNG")->name,"viewer"));
    assert(desk_catalog_supports(desk_catalog_find("paint"),"/Documents/Art.BmP"));
    assert(desk_catalog_supports(desk_catalog_find("paint"),"picture.PNG"));
    assert(!desk_catalog_supports(desk_catalog_find("paint"),"a.jpg"));
    assert(!strcmp(desk_catalog_file("a.jpg")->name,"viewer")); assert(!desk_catalog_file("a.png/noext"));
    /* Save/discard/cancel on close uses the live native modal. */
    assert(paint_request_close() && ui.dialog.kind); dialog_button(2); assert(state().dirty);
    assert(paint_request_close()); dialog_button(0); assert(ui.dialog.kind==DU_DLG_SAVE);
    snprintf(path,sizeof(path),"%s/close-save.bmp",scratch);du_set_text(&ui.dialog.items[5],path);dialog_button(0);assert(!state().dirty);
    free(reference);
}
int main(int argc,char **argv)
{
    assert(argc==3); pixels[PAINT_W*PAINT_H]=0xdeadbeefu;
    du_init(&ui,pixels,PAINT_W,PAINT_H); assert(!paint_attach(&ui));
    stroke_continuity();
    render(argv[2],"paint-initial"); tools(); render(argv[2],"paint-tools");
    selection_and_text(); render(argv[2],"paint-zoom-grid"); codecs(argv[1]); files(argv[1]);
    paint_command(M_Z1); attributes(200,150); paint_command(M_CLEARIMG);
    foreground(2); at_click_widget(&ui,P_TOOL+T_RECT); opt(1,51); stroke(T_RECT,20,20,80,60,1);
    foreground(24); stroke(T_ELLIPSE,100,10,178,70,1);
    paint_command(M_FONT); du_set_text(&ui.dialog.items[4],"1"); ui.dialog.items[5].value=0; dialog_button(0);
    foreground(0); at_click_widget(&ui,P_TOOL+T_TEXT); image_click(20,80,1);
    at_key(&ui,DU_KEY_LSHIFT,1); at_tap(&ui,0x2e); at_key(&ui,DU_KEY_LSHIFT,0);
    const unsigned keys[]={0x17,0x16,0x25,0x19,0x1e,0x17,0x31,0x14};
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++) { if(i==3) at_chord(&ui,DU_KEY_LSHIFT,(int)keys[i]); else at_tap(&ui,(int)keys[i]); }
    image_click(190,140,1); foreground(9); stroke(T_PENCIL,10,134,190,134,1); at_click_widget(&ui,P_TOOL+T_TEXT); render(argv[2],"paint-legacy-scene");
    /* Both growing and shrinking preserve the image/history and guards. */
    struct paint_state before=state(); uint8_t p=paint_pixel(20,20);
    uint32_t *large=calloc(800*600+1,4); assert(large); large[800*600]=0xdeadbeef;
    du_resize(&ui,large,800,600); du_render(&ui); assert(large[800*600]==0xdeadbeef && paint_pixel(20,20)==p && state().can_undo==before.can_undo);
    render(argv[2],"paint-resized"); du_resize(&ui,pixels,PAINT_W,PAINT_H); free(large);
    paint_command(M_HELP); render(argv[2],"paint-help"); dialog_button(0);
    paint_command(M_ABOUT); render(argv[2],"paint-about"); dialog_button(0);
    cursors(argv[2]); footprint_matches_stamp(); freeform_and_text(argv[2]); paint_detach(); assert(!ui.count); puts("paint: PASS"); return 0;
}
