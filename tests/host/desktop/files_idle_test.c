/* SPDX-License-Identifier: MIT
 * Real writer process, production Files client/controller/worker and renderer.
 * Portable readdir lacks Ciuki's metadata extension; request stat metadata only
 * in this host adapter. The guest obtains size/mtime from the directory entry. */
#include "replay_base.h"
#include "resize_transport.h"
#include "../../../apps/files/app.c"
#include <assert.h>
#include <fcntl.h>
#include <sys/wait.h>
static struct files_app app;
static struct du_ui ui;
static struct du_window window;
static pthread_t frame;
static char directory[FM_PATH];
static unsigned listings, ui_lists;
static int baseline;
static uint64_t clock_ns(clockid_t id)
{ struct timespec t; assert(!clock_gettime(id,&t)); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void pause_ms(unsigned ms)
{ struct timespec t={ms/1000,(long)(ms%1000)*1000000}; while(nanosleep(&t,&t) && errno==EINTR) {} }
static int list_host(void *ctx,const char *path,unsigned flags,fm_emit emit,void *arg)
{
    if (pthread_equal(pthread_self(),frame)) { assert(baseline); ui_lists++; }
    if (!strcmp(path,directory)) __atomic_add_fetch(&listings,1,__ATOMIC_RELAXED);
    return fm_native_vfs()->list(ctx,path,flags|FM_LIST_METADATA,emit,arg);
}
static int open_host(void *ctx,const char *path,int create)
{
    char clip[FM_PATH]; snprintf(clip,sizeof(clip),"%s.clip",directory);
    return fm_native_vfs()->open(ctx,!strcmp(path,"/system/settings/files.clip") ? clip : path,create);
}
static int media(struct ciuki_media_state *state) { (void)state; return 0; }
static unsigned serial, turns, paints;
static uint64_t cpu_spent, ping_max;
static void turn(int stress)
{
    struct ciuki_message m; desk_message(&m,DESK_PING,8); m.data[0]=2; m.data[2]=1; m.sender_pid=1;
    desk_put32(m.data+4,++serial); rt_push(&m);
    uint64_t started=clock_ns(CLOCK_MONOTONIC), cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);
    if (stress) files_event(&ui,&(struct du_event){.type=DU_EV_TICK,.id=T_MEDIA},&app);
    assert(du_window_turn(&window,started)>0);
    int pong=0;
    while (rt_pop(&m)) {
        if (m.data[1]==DESK_PONG) { assert(desk_get32(m.data+4)==serial); pong=1; }
        if (m.data[1]==DESK_DAMAGE) paints++;
        for (unsigned i=0;i<m.fd_count;i++) rt_close(m.fds[i]);
    }
    assert(pong); turns++;
    cpu_spent+=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
    uint64_t elapsed=clock_ns(CLOCK_MONOTONIC)-started; if (elapsed>ping_max) ping_max=elapsed;
}
static void finish_scan(void)
{ uint64_t end=clock_ns(CLOCK_MONOTONIC)+3000000000ull; while(app.reading) { assert(clock_ns(CLOCK_MONOTONIC)<end); turn(0); pause_ms(2); } }
static void run_period(unsigned seconds,int old)
{
    baseline=old; turns=paints=0; cpu_spent=ping_max=0;
    unsigned before=__atomic_load_n(&listings,__ATOMIC_RELAXED), old_ui=ui_lists;
    uint64_t begin=clock_ns(CLOCK_MONOTONIC), next=begin;
    while (clock_ns(CLOCK_MONOTONIC)-begin<(uint64_t)seconds*1000000000ull) {
        if (old && clock_ns(CLOCK_MONOTONIC)>=next) {
            /* The removed files.c:idle path, including tree/model/full paint. */
            uint64_t cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);
            assert(!fm_refresh(&app.model)); app.tree_dirty=1; files_update(&app);
            cpu_spent+=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;
            next+=1000000000ull;
        }
        turn(!old); pause_ms(10);
    }
    finish_scan(); uint64_t elapsed=clock_ns(CLOCK_MONOTONIC)-begin;
    unsigned count=__atomic_load_n(&listings,__ATOMIC_RELAXED)-before;
    printf("%s EXTERNAL-WRITER interval_ms=100 entries=1024 wall_ms=%.3f ui_cpu_pct=%.3f input_max_ms=%.3f listings=%u ui_listings=%u paints=%u turns=%u\n",old ? "BEFORE" : "AFTER",elapsed/1e6,cpu_spent*100.0/elapsed,ping_max/1e6,count,ui_lists-old_ui,paints,turns);
    if (!old) { assert(cpu_spent*100<=elapsed*5 && ping_max<50000000ull && count<=seconds+1 && !ui_lists && paints<=seconds+1); }
}
int main(int argc,char **argv)
{
    assert(argc==2); snprintf(directory,sizeof(directory),"%s",argv[1]); frame=pthread_self();
    char path[FM_PATH];
    for(unsigned i=0;i<1024;i++) {
        char name[40]; snprintf(name,sizeof(name),"%04u-copy.bin",i); assert(!fm_join(path,directory,name));
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0); assert(!close(fd));
    }
    rt_reset(); assert(!du_window_open_resizable(&window,&ui,3,760,520,"Files",600,360));
    struct ciuki_message msg; while(rt_pop(&msg)) for(unsigned i=0;i<msg.fd_count;i++) rt_close(msg.fds[i]);
    struct fm_vfs fs=*fm_native_vfs(); fs.list=list_host; fs.open=open_host; app.media_read=media; app.poll_clipboard=1;
    assert(!files_setup(&app,&ui,&fs,directory)); window.configured=1; finish_scan();
    W(&app,W_LIST)->view=DU_DETAILS; files_update(&app); turn(0);
    pid_t writer=fork(); assert(writer>=0);
    if (!writer) {
        assert(!fm_join(path,directory,"0000-copy.bin")); int fd=open(path,O_WRONLY); assert(fd>=0);
        for(unsigned i=1;i<=95;i++) { assert(!ftruncate(fd,(off_t)i*65536)); pause_ms(100); }
        close(fd); _exit(0);
    }
    du_timer_stop(&ui,T_MEDIA); run_period(3,1); finish_scan();
    baseline=0; ui_lists=0; app.model.active=3; app.model.anchor=7; app.model.entries[8].selected=1;
    files_update(&app); turn(0);
    struct fm_entry *entries=app.model.entries; struct files_node *tree=app.tree; struct du_item *rows=app.rows;
    app.media_polled=0; du_timer_start(&ui,T_MEDIA,1000000000ull,1);
    run_period(4,0);
    assert(app.model.entries==entries && app.tree==tree && app.rows==rows && app.model.active==3 && app.model.anchor==7 && app.model.entries[8].selected);
    assert(app.model.entries[0].meta.size>0);
    /* One size change's clipped render must equal a complete reference. */
    uint32_t *pixels=malloc(760*520*4); assert(pixels); memcpy(pixels,ui.canvas.pixels,760*520*4);
    ui.dirty=1; du_render(&ui); assert(!memcmp(pixels,ui.canvas.pixels,760*520*4)); free(pixels);
    int status; assert(waitpid(writer,&status,0)==writer && WIFEXITED(status) && !WEXITSTATUS(status));
    /* No change produces no paint; structural changes remain visible. */
    finish_scan(); app.media_polled=0; assert(!scan_submit(&app,SCAN_MEDIA,NULL,0)); finish_scan(); turn(0);
    unsigned before=paints; app.media_polled=0; assert(!scan_submit(&app,SCAN_MEDIA,NULL,0)); finish_scan(); turn(0); assert(paints==before);
    assert(!fm_join(path,directory,"new.txt")); int fd=open(path,O_CREAT|O_WRONLY,0600); assert(fd>=0 && !close(fd));
    app.media_polled=0; assert(!scan_submit(&app,SCAN_MEDIA,NULL,0)); finish_scan(); assert(app.model.count==1025);
    assert(!unlink(path)); app.media_polled=0; assert(!scan_submit(&app,SCAN_MEDIA,NULL,0)); finish_scan(); assert(app.model.count==1024);
    /* External clipboard publication updates Paste without a synchronous idle scan. */
    snprintf(path,sizeof(path),"%s.clip",directory); FILE *clip=fopen(path,"wb"); assert(clip);
    assert(fputs("C\n/Programs/example\n",clip)>=0 && !fclose(clip));
    app.media_polled=0; assert(!scan_submit(&app,SCAN_MEDIA,NULL,0)); finish_scan();
    assert(app.model.clipboard_count==1 && !W(&app,C_PASTE)->disabled);
    files_destroy(&app); du_window_destroy(&window); assert(!rt_handles() && !rt_mappings());
    puts("PASS Files idle: external process writes every 100 ms, one listing/s, in-place metadata, changed rows, unchanged no-paint, add/remove");
    return 0;
}
