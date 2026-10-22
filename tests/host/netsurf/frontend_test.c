/* SPDX-License-Identifier: MIT
 * Real framebuffer frontend/chrome/fbtk on RAM, with only desktop transport
 * and the event source replaced. Include sources to inspect press/grab state. */
#include <assert.h>
#include "allocation_fail.h"
#include "utils/talloc.h"
#include "html/box.h"
#include "sdk/netsurf/port/image_memory.h"
#include <time.h>
unsigned test_clock_reads,test_expire_read;
static uint64_t test_clock_offset;
int __real_clock_gettime(clockid_t,struct timespec *);
int __wrap_clock_gettime(clockid_t clock,struct timespec *out)
{
    int result=__real_clock_gettime(clock,out);
    if(!result && clock==CLOCK_MONOTONIC) {
        if(++test_clock_reads==test_expire_read)test_clock_offset+=76000000u;
        uint64_t n=(uint64_t)out->tv_sec*1000000000u+out->tv_nsec+test_clock_offset;
        out->tv_sec=n/1000000000u;out->tv_nsec=n%1000000000u;
    }
    return result;
}
void amendment_layout_checks(struct content *);
#define NETSURF_HOMEPAGE "about:welcome"
#define main netsurf_entry
#include "framebuffer/gui.c"
#undef main
#include "html/box_manipulate.h"
#include "netsurf/bitmap.h"
#include "framebuffer/ciuki/chrome.c"
#include "framebuffer/fbtk/event.c"
static bool running,finished;
static unsigned damages,clicks;
static bool intercept_actions,navigation_done;
static unsigned navigations,backs,forwards,reloads,stops;
static char navigated[4096];
static enum browser_window_nav_flags navigate_flags;
static nsurl *fault_url;
static struct ciuki_browser_surface test_surface;
nserror __real_browser_window_navigate(struct browser_window *,nsurl *,nsurl *,enum browser_window_nav_flags,
    char *,struct fetch_multipart_data *,struct hlcache_handle *);
nserror __wrap_browser_window_navigate(struct browser_window *bw,nsurl *url,nsurl *ref,
    enum browser_window_nav_flags flags,char *post,struct fetch_multipart_data *multi,struct hlcache_handle *parent)
{
    if(intercept_actions) {
        navigations++;snprintf(navigated,sizeof(navigated),"%s",nsurl_access(url));navigate_flags=flags;
        return NSERROR_OK;
    }
    return __real_browser_window_navigate(bw,url,ref,flags,post,multi,parent);
}
bool __real_browser_window_back_available(struct browser_window *);
bool __real_browser_window_forward_available(struct browser_window *);
bool __wrap_browser_window_back_available(struct browser_window *bw)
{ return intercept_actions || __real_browser_window_back_available(bw); }
bool __wrap_browser_window_forward_available(struct browser_window *bw)
{ return intercept_actions || __real_browser_window_forward_available(bw); }
nserror __real_browser_window_history_back(struct browser_window *,bool);
nserror __real_browser_window_history_forward(struct browser_window *,bool);
nserror __wrap_browser_window_history_back(struct browser_window *bw,bool tab)
{ if(intercept_actions) { backs++;return NSERROR_OK; }return __real_browser_window_history_back(bw,tab); }
nserror __wrap_browser_window_history_forward(struct browser_window *bw,bool tab)
{ if(intercept_actions) { forwards++;return NSERROR_OK; }return __real_browser_window_history_forward(bw,tab); }
nserror __real_browser_window_reload(struct browser_window *,bool);
nserror __wrap_browser_window_reload(struct browser_window *bw,bool all)
{ if(intercept_actions) { reloads++;return NSERROR_OK; }return __real_browser_window_reload(bw,all); }
void __real_browser_window_stop(struct browser_window *);
void __wrap_browser_window_stop(struct browser_window *bw)
{ if(intercept_actions)stops++;else __real_browser_window_stop(bw); }
void __real_browser_window_mouse_click(struct browser_window *,browser_mouse_state,int,int);
void __wrap_browser_window_mouse_click(struct browser_window *bw,browser_mouse_state state,int x,int y)
{
    if(state&(BROWSER_MOUSE_CLICK_1|BROWSER_MOUSE_CLICK_2))clicks++;
    __real_browser_window_mouse_click(bw,state,x,y);
}
nsurl *__real_browser_window_access_url(struct browser_window *);
nsurl *__wrap_browser_window_access_url(struct browser_window *bw)
{ return fault_url?fault_url:__real_browser_window_access_url(bw); }
static int send_fixture(int channel,const struct ciuki_message *m,uint32_t flags)
{ (void)channel;(void)flags;if(m->data[1]==DESK_DAMAGE)damages++;return 0; }
static void input(unsigned type,int code,int value,int value2)
{
    struct ciuki_input_event e={.type=type,.code=code,.value=value,.value2=value2};
    ciuki_chrome_dispatch(&test_surface,&e);
}
static void press_key(int code)
{ input(CIUKI_INPUT_KEY,code,1,0);input(CIUKI_INPUT_KEY,code,0,0); }
static void text(const char *s)
{ for(;*s;s++)input(CIUKI_INPUT_TEXT,(unsigned char)*s,0,0); }
static void click_at(int x,int y)
{
    input(CIUKI_INPUT_MOTION,0,x,y);
    input(CIUKI_INPUT_BUTTON,1,1,0);input(CIUKI_INPUT_BUTTON,1,0,0);
}
static void shortcut(int code)
{ input(CIUKI_INPUT_KEY,DU_KEY_LCTRL,1,0);press_key(code);input(CIUKI_INPUT_KEY,DU_KEY_LCTRL,0,0); }
static void drain(void);
static void chrome_checks(void)
{
    struct ciuki_browser_surface *s=&test_surface;
    intercept_actions=true;
    struct du_widget *url=du_get(&ui,URL);
    du_set_text(url,"http://old.example/");
    /* Page -> bar -> key: real production dispatcher, no stale page press. */
    input(CIUKI_INPUT_MOTION,0,100,200);drain();
    click_at(url->box.x+20,16);
    assert(toolbar_keys && du_focused(&ui)==URL && !s->buttons && !s->count);
    assert(s->mouse_y==16 && ui.mouse_y==16);
    assert(!url->anchor && url->cursor==strlen(url->text));
    press_key(0x1e);text("abc"); /* KEY must not also insert US 'a'. */
    assert(!strcmp(url->text,"abc"));
    press_key(DU_KEY_LEFT);press_key(DU_KEY_BACKSPACE);assert(!strcmp(url->text,"ac"));
    press_key(DU_KEY_DELETE);assert(!strcmp(url->text,"a"));
    press_key(DU_KEY_HOME);text("x");press_key(DU_KEY_END);text("y");
    assert(!strcmp(url->text,"xay"));
    press_key(DU_KEY_LEFT);press_key(DU_KEY_RIGHT);assert(url->cursor==3);
    shortcut(0x1e);input(CIUKI_INPUT_TEXT,0xe8,0,0);text("@");
    press_key(DU_KEY_HOME);press_key(DU_KEY_DELETE);assert(!strcmp(url->text,"@"));
    shortcut(0x1e);text("http://example.com");
    press_key(DU_KEY_ENTER);
    assert(navigations==1 && !strcmp(navigated,"http://example.com/") && (navigate_flags&BW_NAVIGATE_HISTORY));
    assert(!toolbar_keys);
    shortcut(0x26);drain();assert(toolbar_keys && du_focused(&ui)==URL);
    assert(!url->anchor && url->cursor==strlen(url->text));
    /* A late URL notification cannot replace the user's partially typed URL. */
    text("editing");nsurl *late;assert(nsurl_create("http://late.example/",&late)==NSERROR_OK);
    ciuki_chrome_url(late);nsurl_unref(late);assert(!strcmp(url->text,"editing"));
    /* Click the page, then each tool, without retaining a page button. */
    click_at(100,200);drain();assert(!toolbar_keys && !s->buttons);
    ciuki_chrome_loading(false);ciuki_chrome_render();
    click_at(108,16);assert(!stops); /* Stop is disabled on a finished page. */
    click_at(18,16);click_at(48,16);click_at(78,16);
    assert(backs==1 && forwards==1 && reloads==1);
    ciuki_chrome_loading(true);ciuki_chrome_render();click_at(78,16);assert(reloads==1);
    click_at(108,16);
    assert(stops==1 && !ciuki_chrome_busy());
    ciuki_chrome_render();click_at(138,16);
    assert(navigations==2 && !strcmp(navigated,nsoption_charp(homepage_url)?nsoption_charp(homepage_url):"about:welcome"));
    assert(!toolbar_keys && du_focused(&ui)!=URL);
    unsigned old_bookmarks=bookmarks;click_at(168,16);
    assert(bookmarks==old_bookmarks+1 || hotlist_has_url(browser_window_access_url(window_list->bw)));
    assert(strstr(du_get(&ui,STATUS)->text,"Bookmark saved"));
    /* Status clicks never become page clicks. Page input is restored by click. */
    click_at(20,470);assert(!s->buttons && !s->count);
    shortcut(0x26);click_at(100,200);drain();
    input(CIUKI_INPUT_TEXT,'z',0,0);assert(s->count==1);drain();
    assert(!toolbar_keys && !strcmp(url->text,"editing"));
    /* A page drag crossing chrome retains its release, then bar input works. */
    input(CIUKI_INPUT_BUTTON,1,1,0);drain();
    input(CIUKI_INPUT_MOTION,0,220,16);input(CIUKI_INPUT_BUTTON,1,0,0);drain();
    assert(!s->buttons);click_at(220,16);assert(toolbar_keys);
    /* Conversely, a URL selection drag never presses the page. */
    input(CIUKI_INPUT_BUTTON,1,1,0);input(CIUKI_INPUT_MOTION,0,400,200);
    input(CIUKI_INPUT_BUTTON,1,0,0);assert(!s->buttons && !s->count && ui.capture<0);
    ciuki_chrome_release();ciuki_browser_release(s);drain();clicks=0;
    intercept_actions=false;
    puts("PASS f6-23 chrome: routing, URL selection/editing/shortcuts, toolbar actions, drag ownership");
}
static void log_checks(void)
{
    unsigned before=input_records,lines=0,last=0;
    int saved=dup(2);FILE *log=tmpfile();assert(saved>=0 && log);
    fflush(stderr);assert(dup2(fileno(log),2)==2);
    for(unsigned i=0;i<600;i++)input(CIUKI_INPUT_KEY,0,0,0);
    fflush(stderr);assert(dup2(saved,2)==2);close(saved);
    assert(input_records==512);
    rewind(log);char line[512];
    while(fgets(line,sizeof(line),log)) {
        assert(strstr(line,"[netsurf] input target=") && strstr(line,"widget="));
        if(strstr(line,"last=1"))last++;
        lines++;
    }
    fclose(log);assert(lines==512-before && last==1);
    puts("PASS f6-23 input log: one record per edge up to 512, final marker, then silence");
}
bool __real_nsfb_event(nsfb_t *,nsfb_event_t *,int);
bool __wrap_nsfb_event(nsfb_t *fb,nsfb_event_t *event,int timeout);
static void drain(void)
{
    nsfb_event_t e;
    while(test_surface.count)fbtk_event(fbtk,&e,0);
}
static int files(void *ctx,const char *path,struct du_file_entry *out,unsigned cap,unsigned *count)
{ (void)ctx;(void)path;(void)cap;*count=1;snprintf(out[0].name,sizeof(out[0].name),"caff\xc3\xa8.html");out[0].directory=0;return 0; }
static void exercise(void)
{
    amendment_layout_checks(hlcache_handle_get_content(browser_window_get_content(window_list->bw)));
    int *budget=talloc_zero(NULL,int);assert(budget);
    *budget=8192;unsigned before_alloc=allocation_count;
    fail_allocation=allocation_count+1;
    assert(!box_create(NULL,NULL,false,NULL,NULL,NULL,NULL,budget));
    assert(allocation_count==before_alloc);fail_allocation=0;
    *budget=0;fail_allocation=allocation_count+1;
    assert(!box_create(NULL,NULL,false,NULL,NULL,NULL,NULL,budget));
    fail_allocation=0;assert(!*budget);talloc_free(budget);
    /* Fail every bitmap allocation (object/routines/plotters/pixels) and
     * verify reservation rollback in the actual framebuffer bitmap table. */
    allocation_count=0;
    void *bitmap=framebuffer_bitmap_table->create(32,32,BITMAP_OPAQUE);assert(bitmap);
    unsigned bitmap_allocs=allocation_count;framebuffer_bitmap_table->destroy(bitmap);
    size_t baseline=ciuki_image_used();
    for(unsigned i=1;i<=bitmap_allocs;i++) {
        allocation_count=0;fail_allocation=i;
        bitmap=framebuffer_bitmap_table->create(32,32,BITMAP_OPAQUE);
        fail_allocation=0;
        if(bitmap)framebuffer_bitmap_table->destroy(bitmap);
        assert(ciuki_image_used()==baseline);
    }
    struct ciuki_browser_surface *s=&test_surface;
    s->window=s->generation=1;s->ready=true;
    s->current.info.width=640;s->current.info.height=480;s->current.info.stride=2560;
    s->current.back=calloc(640*480,4);s->current.map=calloc(640*480,4);
    assert(s->current.back && s->current.map);
    s->connection.transport.send=send_fixture;
    ciuki_browser_active=s;ciuki_chrome_init(s);
    chrome_checks();
    /* Wheel reaches actual page scrolling, in both directions and detents. */
    input(CIUKI_INPUT_MOTION,0,100,200);drain();
    struct browser_widget_s *bw=fbtk_get_userpw(window_list->browser);
    bw->panx=bw->pany=bw->scrollx=bw->scrolly=0;
    input(CIUKI_INPUT_WHEEL,0,-3,0);drain();assert(bw->pany==300);
    bw->pany=0;bw->scrolly=400;
    input(CIUKI_INPUT_WHEEL,0,2,0);drain();assert(bw->pany==-200);
    bw->pany=0;bw->scrolly=0;
    input(CIUKI_INPUT_MOTION,0,100,10);drain();
    input(CIUKI_INPUT_WHEEL,0,-2,0);assert(!s->count && !bw->pany);
    /* Lose focus after press, and capture during a drag: neither clicks. */
    for(unsigned drag=0;drag<2;drag++) {
        input(CIUKI_INPUT_MOTION,0,20,60);drain();
        input(CIUKI_INPUT_BUTTON,1,1,0);drain();assert(gui_drag.state==GUI_DRAG_PRESSED);
        if(drag) {
            input(CIUKI_INPUT_MOTION,0,60,80);drain();
            assert(gui_drag.state==GUI_DRAG_DRAG && gui_drag.grabbed_pointer);
            assert(fbtk->u.root.grabbed);
        }
        struct desk_packet loss={.window_id=1,.generation=1,
            .opcode=drag?DESK_V3_POINTER_STATE:DESK_V3_ACTIVATE,.focused=0};
        ciuki_browser_packet(s,&loss);drain();
        assert(!s->buttons && gui_drag.state==GUI_DRAG_NONE && !gui_drag.grabbed_pointer && !fbtk->u.root.grabbed);
        input(CIUKI_INPUT_BUTTON,1,0,0);drain();assert(!clicks);
    }
    assert(!du_file_dialog(&ui,OPEN,0,"Open","/Documents","",files,NULL));
    /* Dialog keeps wheel ownership. */
    input(CIUKI_INPUT_WHEEL,0,-2,0);assert(!s->count);
    struct du_widget *name=&ui.dialog.items[ui.dialog.focus];assert(name->kind==DU_EDIT);
    /* Italian punctuation and accented filename are supplied by desktop TEXT.
     * US scancode punctuation and AltGr's US value must never be inserted. */
    input(CIUKI_INPUT_KEY,0x27,1,0);input(CIUKI_INPUT_TEXT,0x00e8,0,0);
    input(CIUKI_INPUT_KEY,0x138,1,0);input(CIUKI_INPUT_KEY,0x1d,1,0);
    input(CIUKI_INPUT_KEY,0x27,1,0);input(CIUKI_INPUT_TEXT,'@',0,0);
    input(CIUKI_INPUT_KEY,0x1d,0,0);input(CIUKI_INPUT_KEY,0x138,0,0);
    assert(!strcmp(name->text,"\xc3\xa8@"));
    input(CIUKI_INPUT_KEY,DU_KEY_LEFT,1,0);input(CIUKI_INPUT_KEY,DU_KEY_BACKSPACE,1,0);
    assert(!strcmp(name->text,"@"));
    input(CIUKI_INPUT_KEY,0x1d,1,0);input(CIUKI_INPUT_KEY,0x1e,1,0); /* Ctrl+A */
    input(CIUKI_INPUT_KEY,0x1d,0,0);input(CIUKI_INPUT_TEXT,0x00e0,0,0);
    assert(!strcmp(name->text,"\xc3\xa0"));
    input(CIUKI_INPUT_KEY,0x1d,1,0);input(CIUKI_INPUT_KEY,0x1e,1,0);
    input(CIUKI_INPUT_KEY,0x1d,0,0);
    const unsigned filename[]={'c','a','f','f',0xe8,'.','h','t','m','l'};
    for(unsigned i=0;i<sizeof(filename)/sizeof(*filename);i++)input(CIUKI_INPUT_TEXT,filename[i],0,0);
    assert(!strcmp(name->text,"caff\xc3\xa8.html"));
    ciuki_chrome_status("Loading local page");ciuki_chrome_render();
    assert(!ciuki_browser_present(s));unsigned before=damages;assert(before);
    for(unsigned i=0;i<5;i++) { ciuki_chrome_render();assert(!ciuki_browser_present(s)); }
    assert(damages==before && !status_dirty);
    du_dialog_close(&ui);
    /* History errors are visible to the user and do not mutate records. */
    assert(ciuki_profile_path("History.html"));
    /* A URL over the profile cap exercises completion error handling. */
    ciuki_chrome_complete(window_list); /* duplicate suppression */
    char address[4200];snprintf(address,sizeof(address),"data:text/plain,");
    memset(address+16,'x',sizeof(address)-17);address[sizeof(address)-1]=0;
    assert(nsurl_create(address,&fault_url)==NSERROR_OK);
    ciuki_chrome_complete(window_list);
    assert(strstr(du_get(&ui,STATUS)->text,"Unable to record"));
    nsurl_unref(fault_url);fault_url=NULL;
    /* End-to-end core navigation from the bar, completed by the event loop. */
    shortcut(0x26);text("data:text/html,<title>Bar navigation</title><p>Ciuki</p>");press_key(DU_KEY_ENTER);
    log_checks();
    free(s->current.back);free(s->current.map);s->current.back=s->current.map=NULL;
    active=false;surface=NULL;ciuki_browser_active=NULL;
    puts("NetSurf frontend: real chrome wheel/modal Unicode, idle DAMAGE, press/drag cancellation passed");fflush(stdout);
}
bool __wrap_nsfb_event(nsfb_t *fb,nsfb_event_t *event,int timeout)
{
    if(running)return ciuki_browser_event(&test_surface,event);
    if(!finished && window_list && browser_window_get_content(window_list->bw) &&
       content_get_status(browser_window_get_content(window_list->bw))==CONTENT_STATUS_DONE) {
        running=true;exercise();running=false;finished=true;
    }
    if(finished && window_list && browser_window_get_content(window_list->bw) &&
       content_get_status(browser_window_get_content(window_list->bw))==CONTENT_STATUS_DONE &&
       !strcmp(browser_window_get_title(window_list->bw),"Bar navigation")) {
        navigation_done=true;
        puts("PASS f6-23 navigation: address Enter loaded and laid out the page through the real core");
        fflush(stdout);
        event->type=NSFB_EVENT_CONTROL;event->value.controlcode=NSFB_CONTROL_QUIT;return true;
    }
    return __real_nsfb_event(fb,event,timeout);
}
int main(int argc,char **argv)
{ int result=netsurf_entry(argc,argv);assert(finished && navigation_done);return result; }
