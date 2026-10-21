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
static nsurl *fault_url;
static struct ciuki_browser_surface test_surface;
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
    if(!ciuki_chrome_input(&e))ciuki_browser_input(&test_surface,&e);
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
    /* Wheel reaches actual page scrolling, in both directions and detents. */
    input(CIUKI_INPUT_MOTION,0,100,200);drain();
    struct browser_widget_s *bw=fbtk_get_userpw(window_list->browser);
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
        event->type=NSFB_EVENT_CONTROL;event->value.controlcode=NSFB_CONTROL_QUIT;return true;
    }
    return __real_nsfb_event(fb,event,timeout);
}
int main(int argc,char **argv)
{ int result=netsurf_entry(argc,argv);assert(finished);return result; }
