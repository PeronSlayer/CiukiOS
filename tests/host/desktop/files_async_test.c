/* SPDX-License-Identifier: MIT
 * Real Files controller/client loop over a 300-entry, 25 ms/call volume. */
#include "replay_base.h"
#include "resize_transport.h"
#include "../../../apps/files/app.h"
#include <pthread.h>
#include <assert.h>
static struct files_app app;
static struct du_ui ui;
static struct du_window window;
static pthread_t frame;
static unsigned entries, calls, opens, closed, interrupted;
static uint64_t now(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void pause_io(void)
{
    assert(!pthread_equal(pthread_self(),frame));
    __atomic_add_fetch(&calls,1,__ATOMIC_RELAXED);
    struct timespec t={0,25000000};nanosleep(&t,NULL);
}
static int emit_dir(fm_emit emit,void *arg,const char *name)
{ struct fm_meta m={.type=FM_DIR};return emit(name,&m,arg); }
static int list(void *ctx,const char *path,unsigned flags,fm_emit emit,void *arg)
{
    (void)ctx; assert(flags==FM_LIST_NAMES_ONLY); pause_io();
    if (!strcmp(path,"/")) {
        int r=emit_dir(emit,arg,"mnt");return r ? r : emit_dir(emit,arg,"fast");
    }
    if (!strcmp(path,"/mnt")) return emit_dir(emit,arg,"e");
    if (!strcmp(path,"/fast")) return 0;
    if (strcmp(path,"/mnt/e")) return -ENOENT;
    for (unsigned i=0;i<300;i++) {
        pause_io();__atomic_add_fetch(&entries,1,__ATOMIC_RELAXED);
        char name[32];snprintf(name,sizeof(name),"program%03u",i);
        struct fm_meta m={.type=FM_FILE,.size=100,.modified=1792100000};
        int r=emit(name,&m,arg);if (r) return r;
    }
    return 0;
}
static int stat_path(void *ctx,const char *path,struct fm_meta *m)
{ (void)ctx;pause_io();if (!strcmp(path,"/Games"))return -ENOENT;*m=(struct fm_meta){.type=FM_DIR};return 0; }
static int open_file(void *ctx,const char *path,int create)
{ (void)ctx; assert(!create && !strcmp(path,"/mnt/e/program000"));pause_io();opens++;interrupted=0;return 7; }
static int read_file(void *ctx,int fd,void *out,unsigned size)
{
    (void)ctx;assert(fd==7);pause_io();
    if (!interrupted++) return -EINTR;
    static const unsigned char h[20]={0x7f,'E','L','F',1,1,1,0,0,0,0,0,0,0,0,0,2,0,3,0};
    unsigned at=(interrupted-2)*5;if (size>5)size=5;assert(at+size<=sizeof(h));memcpy(out,h+at,size);return (int)size;
}
static int close_file(void *ctx,int fd) { (void)ctx;assert(fd==7);pause_io();closed++;return 0; }
static unsigned launches;
static int launch(struct files_app *a,const char *path,char *const argv[])
{ (void)a;assert(!strcmp(path,"/mnt/e/program000") && argv[0] && !argv[1]);launches++;return 42; }
static const struct fm_vfs fs={.list=list,.stat_path=stat_path,.open=open_file,.read=read_file,.close=close_file};
static uint64_t ping_max,turn_max;
/* f2-89: every pixel of the shared surface is painted, never left transparent. */
static void assert_opaque(void)
{
    size_t n=(size_t)window.info.width*window.info.height;
    for (size_t i=0;i<n;i++) assert(window.pixels[i]!=0);
}
static unsigned serial;
static void turn(void)
{
    struct ciuki_message ping;desk_message(&ping,DESK_PING,8);desk_put32(ping.data+4,++serial);
    ping.data[0]=DESK_SHELL_VERSION;ping.data[2]=DESK_RESIZE_MINOR;ping.sender_pid=1;rt_push(&ping);
    uint64_t begin=now();assert(du_window_turn(&window,begin)>0);
    uint64_t elapsed=now()-begin;if(elapsed>turn_max)turn_max=elapsed;
    struct ciuki_message m;int replied=0;
    while(rt_pop(&m)) {
        if(m.data[1]==DESK_PONG) {assert(desk_get32(m.data+4)==serial);replied=1;}
        for(unsigned k=0;k<m.fd_count;k++)rt_close(m.fds[k]);
    }
    assert(replied);elapsed=now()-begin;if(elapsed>ping_max)ping_max=elapsed;
    assert(elapsed<1000000000ull);
}
static void wait_done(unsigned seconds)
{
    uint64_t deadline=now()+(uint64_t)seconds*1000000000ull;
    while(app.reading) {assert(now()<deadline);turn();struct timespec t={0,10000000};nanosleep(&t,NULL);}
}
int main(void)
{
    frame=pthread_self();rt_reset();
    assert(!du_window_open_resizable(&window,&ui,3,760,520,"Files",600,360));
    struct ciuki_message m;while(rt_pop(&m))for(unsigned k=0;k<m.fd_count;k++)rt_close(m.fds[k]);
    uint64_t begin=now();app.spawn=launch;
    assert(!files_setup(&app,&ui,&fs,"/mnt/e"));
    assert(app.loading && !strcmp(du_get(&ui,100)->text,"E:\\"));
    window.configured=1;turn();uint64_t first_paint=now()-begin;assert(first_paint<300000000ull);
    assert_opaque(); /* first frame of a slow-directory window */
    assert(!strcmp(du_get(&ui,107)->text,"Loading..."));
    struct du_event address={.type=DU_EV_KEY,.key=DU_KEY_F4};files_event(&ui,&address,&app);
    assert(du_focused(&ui)==100 && app.loading);
    struct ciuki_input_event motion={.type=CIUKI_INPUT_MOTION,.value=440,.value2=40};du_input(&ui,&motion);
    enum du_view view=du_get(&ui,107)->view;
    struct ciuki_input_event button={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};du_input(&ui,&button);
    button.value=0;du_input(&ui,&button);assert(du_get(&ui,107)->view!=view && app.loading);
    du_focus(&ui,107);
    while(__atomic_load_n(&entries,__ATOMIC_RELAXED)<3) {turn();struct timespec t={0,10000000};nanosleep(&t,NULL);}
    unsigned before=__atomic_load_n(&entries,__ATOMIC_RELAXED);
    begin=now();assert(!files_navigate(&app,"/fast"));wait_done(3);
    assert(!strcmp(app.model.path,"/fast") && !app.model.count);
    turn();assert_opaque(); /* empty directory */
    assert(__atomic_load_n(&entries,__ATOMIC_RELAXED)<before+3);
    assert(now()-begin<1000000000ull);
    assert(!files_navigate(&app,"/mnt/e"));wait_done(12);
    assert(app.model.count==300 && !opens && !closed && !app.model.entries[0].meta.exec);
    assert(app.model.entries[0].meta.size==100 && app.model.entries[0].meta.modified==1792100000);
    /* Refresh remains asynchronous and cancellable through the actual F5 path. */
    struct ciuki_input_event refresh={.type=CIUKI_INPUT_KEY,.code=DU_KEY_F5,.value=1};
    du_input(&ui,&refresh);refresh.value=0;du_input(&ui,&refresh);assert(app.loading);
    assert(!files_navigate(&app,"/fast"));wait_done(3);
    assert(!strcmp(app.model.path,"/fast"));
    assert(!files_navigate(&app,"/mnt/e"));wait_done(12);
    struct du_event open={.type=DU_EV_OPEN,.id=107,.index=0};files_event(&ui,&open,&app);
    assert(app.reading && !launches);turn();wait_done(3);
    assert(launches==1 && opens==1 && closed==1 && app.model.entries[0].meta.exec);
    /* Closing cancels an in-flight directory stream and releases its result. */
    assert(!files_navigate(&app,"/mnt/e"));turn();assert(files_close_request(&app));
    files_destroy(&app);du_window_destroy(&window);assert(!rt_handles() && !rt_mappings());
    printf("PASS Files worker: 300 entries, 25 ms/call, first_paint_ms=%llu, ping_max_ms=%llu turn_max_ms=%llu, revision cancellation, refresh, lazy ELF short-read/EINTR/open/close\n",(unsigned long long)(first_paint/1000000),(unsigned long long)(ping_max/1000000),(unsigned long long)(turn_max/1000000));
    return 0;
}
