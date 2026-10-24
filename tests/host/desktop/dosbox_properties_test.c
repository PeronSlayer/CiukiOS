/* G17 production client/model tests; only VM/channel hardware is replaced.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../../../apps/desktop/protocol.h"
#undef _SC_PAGESIZE
#undef _SC_OPEN_MAX
#undef _SC_ARG_MAX
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
#undef ILL_ILLOPC
#undef FPE_INTDIV
#undef SEGV_MAPERR
#undef SEGV_ACCERR
#undef BUS_ADRALN
#include <signal.h>
#include <assert.h>
#include <time.h>
#include "../../../apps/dosbox/program.h"
static int model_clock(clockid_t,struct timespec *);
#define clock_gettime model_clock
#define main dosbox_program_main
#include "../../../apps/dosbox/dosbox.c"
#undef main
#undef clock_gettime

/* G5's independent RPC/selection implementation has its own production
 * fixture. Model its lifecycle here to exercise toolbar arbitration. */
static int system_marking,system_pasting;
int dos_clip_marking(void) { return system_marking; }
int dos_clip_pasting(void) { return system_pasting; }
int dos_clip_action(unsigned action)
{ assert(action==0 || action==2); system_marking=0; return 0; }
static uint64_t tick;
static unsigned input_events, queue_full, focus_refused, calls, marking, sensitivity, background, fs;
static struct dosbox_key observed[10000];
static int graphics;
static int model_clock(clockid_t id,struct timespec *t)
{ assert(id==CLOCK_MONOTONIC); t->tv_sec=(time_t)(tick/1000); t->tv_nsec=(long)(tick%1000)*1000000; return 0; }
int ciuki_dosvm(uint32_t op,struct ciuki_dosvm_command *c)
{
    assert(c->version==1 && c->size==136 && c->id==7 && c->generation==3); calls++;
    if (op==CIUKI_DOSVM_POLICY) { marking=c->policy.marking; sensitivity=c->policy.idle_sensitivity; return 0; }
    if (op==CIUKI_DOSVM_BACKGROUND) { background=c->focused; return 0; }
    if (op==CIUKI_DOSVM_FULLSCREEN) { fs=c->focused; return 0; }
    if (op==CIUKI_DOSVM_TEXT_CHUNK) {
        if (graphics) { errno=EOPNOTSUPP; return -1; }
        assert(marking); c->text_chunk.columns=80; c->text_chunk.rows=25;
        for (unsigned i=0;i<48;i++) {
            unsigned x=c->text_chunk.column+i;
            c->text_chunk.characters[i]=(uint8_t)(x<4 ? (c->text_chunk.row==1 ? "Hi! "[x] : "DOS "[x]) : ' ');
            if (c->text_chunk.row==2 && x==3) c->text_chunk.characters[i]=0x82;
        }
        return 0;
    }
    assert(op==CIUKI_DOSVM_INPUT && c->event.type==CIUKI_INPUT_KEY);
    if (queue_full) { queue_full--; errno=EAGAIN; return -1; }
    if (focus_refused) { focus_refused--; errno=EACCES; return -1; }
    observed[input_events++]=(struct dosbox_key){(uint16_t)c->event.code,(uint8_t)c->event.value,0,0,0}; return 0;
}
static const struct desk_theme test_theme={.face=0xc0c0c0,.ink=0,.select=0x112299,.select_ink=0xffffff};
const struct desk_theme *desk_theme=&test_theme;
void du_fill(struct du_canvas *c,struct du_rect r,uint32_t colour)
{
    assert(r.x>=0 && r.y>=0 && r.x+r.w<=640 && r.y+r.h<=480);
    for (int y=r.y;y<r.y+r.h;y++) for (int x=r.x;x<r.x+r.w;x++) c->pixels[y*640+x]=colour;
}
void du_edge(struct du_canvas *c,struct du_rect r,enum du_edge edge) { (void)c;(void)r;(void)edge; }
void du_icon_draw(struct du_canvas *c,int x,int y,const struct du_icon *i,enum du_icon_mode m)
{ (void)c;(void)x;(void)y;(void)i;(void)m; }
const struct du_icon *du_icon(const char *name,unsigned size) { assert(name && size==16); return NULL; }
int du_text_width(const char *s) { return (int)strlen(s)*7; }
static unsigned labels;
void du_text_fit(struct du_canvas *c,int x,int y,int width,const char *s,uint32_t ink)
{ (void)c;(void)x;(void)y;(void)width;(void)ink; assert(s); labels++; }
int du_text_wrap(struct du_canvas *c,struct du_rect r,const char *s,uint32_t ink)
{ du_text_fit(c,r.x,r.y,r.w,s,ink); return 1; }
static void write_record(const char *path,const void *record,size_t bytes)
{ FILE *f=fopen(path,"wb"); assert(f && fwrite(record,1,bytes,f)==bytes && !fclose(f)); }
static void profile_tests(void)
{
    struct dosbox_profile p,read;
    dosbox_profile_default(&p,"\\CUSTOM\\GAME.EXE -x"); assert(dosbox_profile_valid(&p));
    assert(!dosbox_profile_load(&p));
    char key[97]; assert(!dosbox_program_key("/custom/game.exe -other",key) && !strcmp(key,p.key));
    p.zoom=2; p.memory=1; p.screen=2; p.background=0; p.quickedit=1; p.idle=100;
    strcpy(p.directory,"C:\\DATA"); strcpy(p.batch,"C:\\START.BAT");
    assert(!dosbox_profile_save(&p));
    dosbox_profile_default(&read,"\\custom\\game.exe -y"); assert(dosbox_profile_load(&read)==1 && !memcmp(&read,&p,sizeof(p)));
    char path[192],temp[224]; assert(!dosbox_profile_path(p.key,path,sizeof(path)));
    snprintf(temp,sizeof(temp),"%s.tmp.%lu",path,(unsigned long)getpid());
    write_record(temp,"busy",4); p.zoom=1; assert(dosbox_profile_save(&p)<0 && errno==EEXIST); assert(!unlink(temp));
    dosbox_profile_default(&read,p.command); assert(dosbox_profile_load(&read)==1 && read.zoom==2);
    struct dosbox_profile corrupt=p; corrupt.idle=101;
    write_record(path,&corrupt,sizeof(corrupt));
    dosbox_profile_default(&read,p.command); assert(dosbox_profile_load(&read)<0 && read.idle==50);
    memset(corrupt.command,'x',sizeof(corrupt.command)); write_record(path,&corrupt,sizeof(corrupt));
    assert(dosbox_profile_load(&read)<0 && read.zoom==0);
    write_record(path,&p,sizeof(p)-1); assert(dosbox_profile_load(&read)<0);
    assert(!dosbox_profile_save(&p));
    FILE *f=fopen(path,"ab"); assert(f && fputc('x',f)=='x' && !fclose(f)); assert(dosbox_profile_load(&read)<0);
    corrupt=p; strcpy(corrupt.key,"OTHER.EXE"); write_record(path,&corrupt,sizeof(corrupt));
    assert(dosbox_profile_load(&read)<0 && dosbox_profile_save(&p)<0 && errno==EEXIST);
    assert(!unlink(path));
    strcpy(p.directory,"C:\\DATA & DIR"); assert(!dosbox_profile_valid(&p));
    puts("PASS G17 profiles: arbitrary executables, case/slash/arguments identity, atomic save, defaults, corrupt/oversized/collision rejection");
}
static void launch_tests(void)
{
    struct dosbox_profile p; dosbox_profile_default(&p,"\\CUSTOM\\GAME.EXE -x");
    strcpy(p.directory,"C:\\DATA"); strcpy(p.batch,"C:\\START.BAT");
    struct dosbox_key *keys=calloc(DOSBOX_PROFILE_KEYS,sizeof(*keys)); assert(keys);
    int n=dosbox_profile_keys(&p,keys,DOSBOX_PROFILE_KEYS); assert(n>0);
    unsigned ends=0; for (int i=0;i<n;i++) {
        if (keys[i].line_end) ends++;
        assert(keys[i].group<=i && keys[i].main<(unsigned)n && keys[i].main>=keys[i].group);
        unsigned resume=dosbox_resume(keys,(unsigned)n,(unsigned)i); assert(resume<=(unsigned)n);
    }
    assert(ends==4); /* drive, CD, batch, executable */
    assert(dosbox_profile_keys(&p,keys,4)<0);
    free(keys);
    puts("PASS G17 launch: working drive/folder, startup batch, final command, bounded input, rebased focus resume groups");
}
static int event(unsigned type,int code,int value,int value2)
{
    struct ciuki_input_event e={.type=type,.code=code,.value=value,.value2=value2};
    int action=-1; assert(db_input(&window,&e,&action)); return action;
}
static void ui_tests(void)
{
    dosbox_profile_default(&window.profile,"CUSTOM.EXE"); db_properties(&window);
    assert(window.properties && window.page==0);
    assert(event(CIUKI_INPUT_TEXT,'X',0,0)==DB_NONE); assert(strstr(window.draft.command,"EXEX"));
    event(CIUKI_INPUT_KEY,CIUKI_KEY_ESCAPE,1,0); assert(!window.properties && !strcmp(window.profile.command,"CUSTOM.EXE"));
    db_properties(&window);
    for (unsigned i=0;i<DB_FIELD_COUNT;i++) if (!db_text_field(i)) {
        db_page(&window,(int)db_fields[i].page); window.field=(int)i;
        uint32_t before=*db_value(&window.draft,i);
        event(CIUKI_INPUT_KEY,CIUKI_KEY_RIGHT,1,0); assert(*db_value(&window.draft,i)==(before+1)%(db_fields[i].maximum+1));
    }
    assert(event(CIUKI_INPUT_KEY,CIUKI_KEY_ENTER,1,0)==DB_APPLY && !window.properties);
    assert(!window_action(DB_APPLY,7,3,0) && sensitivity==51 && background==1);
    static uint32_t guest[640*480],pixels[640*480];
    for (unsigned i=0;i<640*480;i++) guest[i]=i;
    struct du_canvas canvas={pixels,640,480,{0,0,640,480}};
    for (unsigned page=0;page<5;page++) { db_properties(&window); db_page(&window,(int)page); db_paint(&window,&canvas,guest); assert(!window.dirty); }
    assert(labels>100); window.properties=0; window.notice[0]=0; window.profile.zoom=0;
    db_paint(&window,&canvas,guest); assert(pixels[28*640]==guest[0] && pixels[479*640+639]==guest[(451*480/452)*640+639]);
    uint32_t src[15]={0},damage[15]={0}; src[17/32]|=1u<<(17%32);
    guest[17*640]=0xabcdef; db_paint_rows(&window,&canvas,guest,src,damage);
    for (unsigned y=28;y<480;y++) assert(!!(damage[y/32]&(1u<<(y%32)))==((y-28)*480/452==17));
    window.properties=1; memset(damage,0,sizeof(damage)); db_paint_rows(&window,&canvas,guest,src,damage);
    for (unsigned i=0;i<15;i++) assert(!damage[i]);
    /* Win98-style toolbar: ids by position, hover tooltip, sheet hit-testing. */
    assert(db_tool_at(4,10)==DB_TOOL_ZOOM && db_tool_at(112,10)==DB_TOOL_MARK && db_tool_at(185,10)==-1);
    assert(db_tool_at(224,10)==DB_TOOL_PROPS && db_tool_at(290,10)==DB_TOOL_FONT && db_tool_at(300,10)==-1);
    window.properties=0; window.dirty=0; event(CIUKI_INPUT_MOTION,0,136,10); assert(window.hover==DB_TOOL_COPY+1 && window.dirty && db_tip_visible(&window));
    event(CIUKI_INPUT_MOTION,0,300,10); assert(!window.hover);
    window.properties=1; window.page=3; window.field=0;
    {   int x=db_button_rect(1).x+3,y=db_button_rect(1).y+3;
        event(CIUKI_INPUT_MOTION,0,x,y); event(CIUKI_INPUT_BUTTON,CIUKI_BUTTON_LEFT,1,0); assert(!window.properties);
        db_properties(&window); event(CIUKI_INPUT_MOTION,0,DB_TAB_X+DB_TAB_W*4+5,DB_TAB_Y+5);
        event(CIUKI_INPUT_BUTTON,CIUKI_BUTTON_LEFT,1,0); assert(window.page==4);
        unsigned idle=0; for (unsigned i=0;i<DB_FIELD_COUNT;i++) if (db_kind(i)==DB_KIND_SLIDER) idle=i;
        struct du_rect t=db_control(idle);
        event(CIUKI_INPUT_MOTION,0,t.x+6+(t.w-12)/2,t.y+10); event(CIUKI_INPUT_BUTTON,CIUKI_BUTTON_LEFT,1,0);
        assert(window.draft.idle==50);
        event(CIUKI_INPUT_MOTION,0,t.x+t.w,t.y+10); event(CIUKI_INPUT_BUTTON,CIUKI_BUTTON_LEFT,1,0); assert(window.draft.idle==100);
        window.properties=0;
    }
    window.properties=0;
    window.mouse_x=20; window.mouse_y=10;
    event(CIUKI_INPUT_BUTTON,CIUKI_BUTTON_LEFT,1,0);
    event(CIUKI_INPUT_BUTTON,CIUKI_BUTTON_LEFT,0,0);
    assert(window.font_menu && window.font_choice==0);
    event(CIUKI_INPUT_KEY,CIUKI_KEY_DOWN,1,0);
    assert(event(CIUKI_INPUT_KEY,CIUKI_KEY_ENTER,1,0)==DB_APPLY && !window.font_menu && window.profile.zoom==1);
    event(CIUKI_INPUT_WHEEL,0,INT32_MIN,0); assert(window.scroll_y==28);
    event(CIUKI_INPUT_WHEEL,0,INT32_MAX,0); assert(!window.scroll_y);
    assert(!window_action(DB_FULLSCREEN,7,3,0) && fs==1);
    char path[192]; assert(!dosbox_profile_path(window.profile.key,path,sizeof(path)) && !unlink(path));
    puts("PASS G17 UI: all five pages, every editable setting, Cancel/Apply/OK, modal input, full/incremental scaled view, exclusive policy");
}
static void clipboard_tests(void)
{
    dosbox_profile_default(&window.profile,"CUSTOM.EXE");
    window.mouse_x=16; window.mouse_y=48;
    assert(!db_mark(&window,7,3) && marking && window.columns==80 && window.rows==25);
    window.x0=0; window.y0=1; window.x1=79; window.y1=2;
    assert(!db_copy(&window,7,3) && !marking && !strcmp(du_clipboard_get(),"Hi!\r\nDOS\xc3\xa9"));
    graphics=1; assert(db_mark(&window,7,3)<0 && !marking); graphics=0;
    assert(db_paste_keys("bad\xc3\xa9",paste_keys,DOSBOX_PASTE_KEYS)<0);
    assert(db_paste_keys("a\x01",paste_keys,DOSBOX_PASTE_KEYS)<0);
    assert(db_paste_keys("abc",paste_keys,4)<0);
    paste_count=(unsigned)db_paste_keys("A\r\nb\tc",paste_keys,DOSBOX_PASTE_KEYS); paste_next=0;
    assert(paste_count==12); tick=100;
    system_pasting=1; assert(!paste_step(7,3,1) && !paste_next && !input_events);
    system_pasting=0; system_marking=1;
    assert(!paste_step(7,3,1) && !paste_next && !input_events);
    assert(!window_action(DB_COPY,7,3,0) && !system_marking);
    queue_full=1;
    assert(!paste_step(7,3,1) && paste_next==0 && input_events==0);
    focus_refused=1; assert(!paste_step(7,3,1) && !paste_next);
    assert(!paste_step(7,3,0) && !paste_next);
    window.profile.fast_paste=1; assert(!paste_step(7,3,1) && paste_next==paste_count);
    assert(input_events==paste_count && observed[0].code==CIUKI_KEY_LEFTSHIFT);
    unsigned enters=0; for (unsigned i=0;i<input_events;i++) if (observed[i].code==CIUKI_KEY_ENTER && observed[i].down) enters++;
    assert(enters==1);
    puts("PASS G17 clipboard: Mark/freeze, graphics refusal, chunked rectangular CP437 to UTF-8, CRLF/trim, ASCII paste, queue/focus backpressure");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    char clip[192]; snprintf(clip,sizeof(clip),DOSBOX_SETTINGS_DIR "/clipboard");
    assert(!mkdir(DOSBOX_SETTINGS_DIR,0755) || errno==EEXIST); assert(!du_clipboard_path(clip));
    profile_tests(); launch_tests(); ui_tests(); clipboard_tests();
    assert(calls>0);
    assert(!unlink(clip)); du_clipboard_path(NULL); assert(!rmdir(DOSBOX_SETTINGS_DIR));
    puts("PASS G17 DOS program properties and window controls"); return 0;
}
