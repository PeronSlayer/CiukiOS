/* SPDX-License-Identifier: MIT
 * Production Files worker/client and compositor, with a 1 MiB/s device.
 * The controller is included to drive job starts without clipboard I/O. */
#include "replay_base.h"
#include "resize_transport.h"
#include "../../../apps/files/app.c"
#include <assert.h>

#define SIZE (10u*1024u*1024u)
static pthread_t frame;
static int synchronous;
static struct {
    unsigned size, offset, output, reads, max_read, syncs, closes, retries, short_io;
    uint64_t retry_at;
    int source, target, temporary, fail_write, block_read, entered, release, cross_device;
    pthread_mutex_t lock;
    pthread_cond_t changed;
} disk;
static struct files_app app;
static struct du_ui ui;
static struct du_window window;
static struct desktop desktop;
static uint64_t mono(void) { return job_now(); }
static uint64_t cpu(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void delay(uint64_t ns)
{ struct timespec t={(time_t)(ns/1000000000ull),(long)(ns%1000000000ull)}; while(nanosleep(&t,&t) && errno==EINTR) {} }
static void io(void) { assert(synchronous || !pthread_equal(frame,pthread_self())); }
static void reset(unsigned size)
{
    disk.size=size; disk.offset=disk.output=disk.reads=disk.max_read=disk.syncs=disk.closes=disk.retries=disk.short_io=0;
    disk.retry_at=0;
    disk.source=1; disk.target=disk.temporary=disk.fail_write=disk.block_read=disk.entered=disk.release=disk.cross_device=0;
}
static int stat_path(void *ctx,const char *p,struct fm_meta *m)
{
    (void)ctx; io();
    *m=(struct fm_meta){.type=FM_DIR,.dev=1,.ino=1,.mode=0777};
    if (!strcmp(p,"/") || !strcmp(p,"/mnt") || !strcmp(p,"/mnt/e")) return 0;
    if ((!strcmp(p,"/mnt/e/large.mp3") && disk.source) || (!strcmp(p,"/large.mp3") && disk.target)) {
        *m=(struct fm_meta){.type=FM_FILE,.size=disk.size,.ino=!strcmp(p,"/large.mp3") ? 3 : 2,.dev=1,.mode=0666}; return 0;
    }
    return -ENOENT;
}
static int list(void *ctx,const char *p,unsigned flags,fm_emit emit,void *arg)
{
    (void)ctx; (void)flags; io();
    struct fm_meta m={.type=FM_DIR,.dev=1,.ino=1};
    if (!strcmp(p,"/")) return emit("mnt",&m,arg);
    if (!strcmp(p,"/mnt")) return emit("e",&m,arg);
    if (!strcmp(p,"/mnt/e")) {
        m=(struct fm_meta){.type=FM_FILE,.size=disk.size,.dev=1,.ino=2};
        return disk.source ? emit("large.mp3",&m,arg) : 0;
    }
    return -ENOENT;
}
static int open_file(void *ctx,const char *p,int create)
{
    (void)ctx; io();
    if (!create) { assert(!strcmp(p,"/mnt/e/large.mp3")); disk.offset=0; return 7; }
    assert(strstr(p,".ciuki-copy-")); assert(!disk.temporary); disk.temporary=1; disk.output=0; return 8;
}
static int read_file(void *ctx,int fd,void *out,unsigned n)
{
    (void)ctx; io(); assert(fd==7); disk.reads++; if(n>disk.max_read)disk.max_read=n;
    if (disk.retries) {
        uint64_t t=mono(); if(disk.retry_at)assert(t-disk.retry_at>=1000000ull);
        disk.retry_at=t;disk.retries--;return -EINTR;
    }
    pthread_mutex_lock(&disk.lock);
    if (disk.block_read) {
        disk.entered=1; pthread_cond_signal(&disk.changed);
        while(!disk.release) pthread_cond_wait(&disk.changed,&disk.lock);
    }
    pthread_mutex_unlock(&disk.lock);
    if (n>disk.size-disk.offset) n=disk.size-disk.offset;
    if (disk.short_io && n>7111) n=7111;
    if(n)delay((uint64_t)n*1000000000ull/1048576u+200000ull*((n+65535u)/65536u));
    for(unsigned i=0;i<n;i++) ((unsigned char *)out)[i]=(unsigned char)((disk.offset+i)%251);
    disk.offset+=n; return (int)n;
}
static int write_file(void *ctx,int fd,const void *in,unsigned n)
{
    (void)ctx; io(); assert(fd==8 && disk.temporary);
    if(disk.fail_write)return -ENOSPC;
    if(disk.short_io && n>3001)n=3001;
    for(unsigned i=0;i<n;i++)assert(((const unsigned char *)in)[i]==(unsigned char)((disk.output+i)%251));
    disk.output+=n; return (int)n;
}
static int sync_file(void *ctx,int fd) { (void)ctx;io();assert(fd==8);disk.syncs++;return 0; }
static int close_file(void *ctx,int fd) { (void)ctx;io();assert(fd==7 || fd==8);disk.closes++;return 0; }
static int remove_file(void *ctx,const char *p)
{
    (void)ctx;io();
    if(strstr(p,".ciuki-copy-")) { assert(disk.temporary);disk.temporary=0;return 0; }
    assert(!strcmp(p,"/mnt/e/large.mp3")); disk.source=0; return 0;
}
static int rename_file(void *ctx,const char *from,const char *to)
{
    (void)ctx;io(); assert(!strcmp(to,"/large.mp3"));
    if(!strcmp(from,"/mnt/e/large.mp3")) {
        if(disk.cross_device)return -EXDEV;
        disk.source=0;disk.target=1;return 0;
    }
    assert(disk.temporary && disk.output==disk.size && disk.syncs==1);
    disk.temporary=0;disk.target=1;return 0;
}
static struct fm_vfs fs={.list=list,.stat_path=stat_path,.open=open_file,.read=read_file,.write=write_file,
    .sync=sync_file,.close=close_file,.unlink=remove_file,.rename=rename_file};
static unsigned serial;
static uint64_t ui_cpu, desktop_cpu, latency, frame_max;
static unsigned damages, turns;
static int measuring;
static void turn(void)
{
    uint64_t begin=mono(), before=cpu();
    struct ciuki_message ping; desk_message(&ping,DESK_PING,8);desk_put32(ping.data+4,++serial);ping.data[0]=DESK_SHELL_VERSION;ping.data[2]=DESK_RESIZE_MINOR;rt_push(&ping);
    struct ciuki_input_event input={.type=CIUKI_INPUT_MOTION,.value=(turns%2) ? 1 : -1};du_input(&ui,&input);
    assert(du_window_turn(&window,mono())>0);
    uint64_t took=mono()-begin;if(measuring && took>latency)latency=took;
    if(measuring)ui_cpu+=cpu()-before;
    struct ciuki_message m;int pong=0;
    while(rt_pop(&m)) {
        if(m.data[1]==DESK_PONG) {assert(desk_get32(m.data+4)==serial);pong=1;}
        if(m.data[1]==DESK_DAMAGE) {
            struct desk_box content=desk_content(&desktop.clients[0]);
            int x=(int)desk_get32(m.data+4),y=(int)desk_get32(m.data+8);
            int w=(int)desk_get32(m.data+12),h=(int)desk_get32(m.data+16);
            if(measuring && app.job_active && ui.dialog.kind==DU_DLG_PROGRESS) {
                assert(x>=ui.dialog.box.x && y>=ui.dialog.box.y);
                assert(x+w<=ui.dialog.box.x+ui.dialog.box.w && y+h<=ui.dialog.box.y+ui.dialog.box.h);
                damages++;
            }
            desk_damage_add(&desktop.damage,(struct desk_box){content.x+x,content.y+y,w,h},desktop.width,desktop.height);
        }
        for(unsigned k=0;k<m.fd_count;k++)rt_close(m.fds[k]);
    }
    assert(pong);
    before=cpu();begin=mono();desktop.now_ns=begin;
    struct ciuki_input_event pointer={.type=CIUKI_INPUT_MOTION,.value=(turns%2) ? 1 : -1,.monotonic_ns=begin};
    desk_input(&desktop,&pointer);
    assert(!desk_redraw(&desktop));
    if(measuring){desktop_cpu+=cpu()-before;took=mono()-begin;if(took>frame_max)frame_max=took;}
    turns++;
}
static void wait_job(void)
{
    uint64_t deadline=mono()+20000000000ull;
    while(app.job_active) {assert(mono()<deadline);turn();delay(10000000);}
    while(app.reading) {assert(mono()<deadline);turn();delay(1000000);}
}
static void begin(enum fm_job_kind kind)
{ char *paths[]={"/mnt/e/large.mp3"};assert(!start_job(&app,kind,paths,1,kind==FM_COPY || kind==FM_MOVE ? "/" : NULL)); }
static void setup(void)
{
    assert(!du_window_open_resizable(&window,&ui,3,760,520,"Files",600,360));
    struct ciuki_message m;while(rt_pop(&m))for(unsigned k=0;k<m.fd_count;k++)rt_close(m.fds[k]);
    assert(!files_setup(&app,&ui,&fs,"/"));window.configured=1;
    desktop=(struct desktop){.width=1024,.height=768,.focus=0,.drag=-1,.mouse_x=900,.mouse_y=650,.ops=rt_ops,.time_ns=mono};
    desktop.pixels=calloc(1024u*768u,4);assert(desktop.pixels);
    desktop.clients[0]=(struct desk_client){.window=1,.hello=1,.order=1,.pixels=window.pixels,.info=window.info,.frame=desk_frame(760,520,16,48)};
    strcpy(desktop.clients[0].title,"Files");
    uint64_t deadline=mono()+2000000000ull;
    while(app.reading){assert(mono()<deadline);turn();delay(1000000);}
    turn();
}
static void benchmark_chunks(void)
{
    synchronous=1; unsigned char *buffer=malloc(131072);assert(buffer);
    for(unsigned chunk=32768;chunk<=131072;chunk*=2) {
        reset(1048576);uint64_t begin=mono();
        while(disk.offset<disk.size)assert(read_file(NULL,7,buffer,chunk)>0);
        printf("CHUNK bytes=%u calls=%u bulk_commands=%u ms=%.3f throughput_kbs=%.1f boundary_ms=%.3f\n",chunk,disk.reads,disk.reads*((chunk+65535u)/65536u),(mono()-begin)/1e6,1048576.0*1e9/(mono()-begin)/1024,chunk*1000.0/1048576);
    }
    free(buffer);synchronous=0;
}
static void baseline(void)
{
    synchronous=1;reset(SIZE);
    struct fm_job *j=calloc(1,sizeof(*j));assert(j);
    char *paths[]={"/mnt/e/large.mp3"};assert(!fm_job_start(j,&fs,FM_COPY,paths,1,"/"));j->counting=0;
    /* The old step read 16 KiB. Cap each read through a VFS wrapper below. */
    uint64_t begin=mono(),spent=0,scene=0,max=0;unsigned ticks=0;
    while(j->state==FM_RUNNING) {
        uint64_t before=cpu(),start=mono();
        for(unsigned k=0;k<24 && j->state==FM_RUNNING;k++)fm_job_step(j);
        du_progress(&ui,999,"Copying...","Copying: large.mp3\nFrom: E:\\\nTo: C:\\",j->completed,j->count);
        ui.dirty=1;du_render(&ui);window.damaged=0;du_window_present(&window);spent+=cpu()-before;
        before=cpu();
        if(window.damaged) {
            struct desk_box content=desk_content(&desktop.clients[0]);struct du_rect r=window.damage;
            desk_damage_add(&desktop.damage,(struct desk_box){content.x+r.x,content.y+r.y,r.w,r.h},desktop.width,desktop.height);
            assert(!desk_redraw(&desktop));window.damaged=0;
        }
        scene+=cpu()-before;
        uint64_t elapsed=mono()-start;if(elapsed>max)max=elapsed;ticks++;delay(2000000);
    }
    uint64_t elapsed=mono()-begin;
    printf("BEFORE bytes=%u wall_ms=%.3f ui_cpu_pct=%.3f desktop_cpu_pct=%.3f input_max_ms=%.3f timer_ticks=%u\n",SIZE,elapsed/1e6,spent*100.0/elapsed,scene*100.0/elapsed,max/1e6,ticks);
    fm_job_destroy(j);free(j);du_dialog_close(&ui);synchronous=0;
}
static int old_read(void *ctx,int fd,void *out,unsigned n)
{ if(n>16384)n=16384;return read_file(ctx,fd,out,n); }
static void capture_frames(const char *folder)
{
    assert(!app.job_active);
    for(unsigned theme=0;theme<2;theme++) {
        assert(!desk_theme_use(desk_theme_at(theme)->name));
        for(unsigned kind=FM_COPY;kind<=FM_PROPERTIES;kind++) {
            app.job.kind=(enum fm_job_kind)kind;
            pthread_mutex_lock(&app.transfer->lock);
            app.transfer->view=(struct transfer_view){.state=FM_RUNNING,.bytes=3u*1048576u,.total=SIZE,.file_bytes=3u*1048576u,.file_total=SIZE,.elapsed=3000000000ull,.rate=1048576,.items=1,.count=4,.files=2,.folders=1};
            strcpy(app.transfer->view.source,"/mnt/e/large.mp3");pthread_mutex_unlock(&app.transfer->lock);
            strcpy(app.job.destination,"/");app.transfer->started_at=mono()-3000000000ull;progress(&app);du_render(&ui);
            char path[1200];snprintf(path,sizeof(path),"%s/transfer-%u-%u.xrgb",folder,theme,kind);
            FILE *f=fopen(path,"wb");assert(f && fwrite(window.back,4,760u*520u,f)==760u*520u && !fclose(f));
            /* Clipped update must match an ordinary complete render exactly. */
            uint32_t *copy=malloc(760u*520u*4);assert(copy);memcpy(copy,window.back,760u*520u*4);
            ui.dirty=1;du_render(&ui);assert(!memcmp(copy,window.back,760u*520u*4));free(copy);du_dialog_close(&ui);
        }
    }
    assert(!desk_theme_use(desk_theme_at(0)->name));
}
int main(int argc,char **argv)
{
    assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);frame=pthread_self();assert(!pthread_mutex_init(&disk.lock,NULL));assert(!pthread_cond_init(&disk.changed,NULL));rt_reset();
    benchmark_chunks();reset(SIZE);setup();
    fs.read=old_read;baseline();fs.read=read_file;
    reset(SIZE);begin(FM_COPY);turn();
    ui_cpu=desktop_cpu=latency=frame_max=0;damages=turns=0;uint64_t started=mono();measuring=1;
    while(app.job_active) {
        /* Exclude terminal browser refresh from the transfer-only budget. */
        pthread_mutex_lock(&app.transfer->lock);int finished=app.transfer->finished;pthread_mutex_unlock(&app.transfer->lock);
        if(finished)break;
        turn();delay(10000000);
    }
    measuring=0;uint64_t elapsed=mono()-started;wait_job();
    assert(app.job.state==FM_DONE && app.job.bytes==SIZE && app.job.total_bytes==SIZE && app.job.items_done==1 && app.job.total_items==1);
    assert(disk.syncs==1 && disk.closes==2 && disk.target && !disk.temporary && disk.max_read==FM_CHUNK);
    assert(latency<50000000 && frame_max<50000000 && ui_cpu*100/elapsed<=5 && desktop_cpu*100/elapsed<=10);
    assert(damages<=elapsed/100000000ull+2 && damages>=60 && turns>500 && desktop.inputs>=turns);
    printf("AFTER bytes=%u wall_ms=%.3f ui_cpu_pct=%.3f desktop_cpu_pct=%.3f input_max_ms=%.3f frame_max_ms=%.3f progress_damage=%u input_turns=%u\n",SIZE,elapsed/1e6,ui_cpu*100.0/elapsed,desktop_cpu*100.0/elapsed,latency/1e6,frame_max/1e6,damages,turns);
    /* Worker blocks on overwrite; UI remains responsive and cancellation
     * removes a partially copied temporary, never the existing destination. */
    reset(50000);disk.target=1;begin(FM_COPY);
    uint64_t deadline=mono()+2000000000ull;
    while(du_dialog_id(&ui)!=D_REPLACE){assert(mono()<deadline);turn();delay(1000000);}
    unsigned reads=disk.reads;delay(30000000);assert(disk.reads==reads && !disk.temporary);
    transfer_command(&app,0,FM_YES);wait_job();assert(disk.target && app.job.state==FM_DONE);
    reset(50000);disk.block_read=1;begin(FM_COPY);
    pthread_mutex_lock(&disk.lock);while(!disk.entered)pthread_cond_wait(&disk.changed,&disk.lock);pthread_mutex_unlock(&disk.lock);
    assert(!pthread_mutex_trylock(&app.transfer->lock));pthread_mutex_unlock(&app.transfer->lock);
    /* Leave a VFS call held beyond five seconds: input, elapsed display and
     * the protected progress record must continue without waking that call. */
    started=mono();while(mono()-started<5200000000ull){turn();delay(10000000);}
    const char *elapsed_text=strstr(ui.dialog.text,"Elapsed: "); unsigned held_seconds=0;
    assert(elapsed_text && sscanf(elapsed_text,"Elapsed: %u s",&held_seconds)==1 && held_seconds>=5);
    turn();assert(!files_close_request(&app) && !app.close_pending && du_dialog_id(&ui)==D_CANCEL_JOB);
    struct du_event no={.type=DU_EV_DIALOG,.id=D_CANCEL_JOB,.index=1};du_dialog_close(&ui);files_event(&ui,&no,&app);
    assert(!app.close_pending);transfer_command(&app,1,-1);
    pthread_mutex_lock(&disk.lock);disk.release=1;pthread_cond_signal(&disk.changed);pthread_mutex_unlock(&disk.lock);
    wait_job();assert(app.job.state==FM_CANCELLED && !disk.temporary && !disk.target && disk.source && !disk.output);
    reset(50000);disk.fail_write=1;begin(FM_COPY);wait_job();assert(app.job.state==FM_FAILED && app.job.error==ENOSPC && !disk.temporary);du_dialog_close(&ui);
    reset(50000);disk.retries=3;disk.short_io=1;started=mono();begin(FM_COPY);wait_job();assert(app.job.state==FM_DONE && disk.syncs==1 && mono()-started>=3000000ull);
    reset(50000);disk.cross_device=1;begin(FM_MOVE);wait_job();assert(app.job.state==FM_DONE && !disk.source && disk.target && app.job.bytes==50000);
    reset(50000);begin(FM_MOVE);wait_job();assert(app.job.state==FM_DONE && !disk.source && disk.target && !disk.reads && app.job.bytes==50000);
    reset(50000);begin(FM_DELETE);wait_job();assert(app.job.state==FM_DONE && !disk.source && app.job.items_done==1);
    reset(50000);begin(FM_PROPERTIES);wait_job();assert(app.job.state==FM_DONE && app.job.bytes==50000);du_dialog_close(&ui);
    capture_frames(argv[1]);files_destroy(&app);desk_render_release(&desktop);free(desktop.pixels);du_window_destroy(&window);
    assert(!rt_handles() && !rt_mappings());pthread_cond_destroy(&disk.changed);pthread_mutex_destroy(&disk.lock);
    puts("PASS Files transfer worker: slow copy budgets, prompt sleep, cancel boundary/cleanup, disk error, short I/O/EINTR, move, delete, properties, clipped/full render equivalence");
    return 0;
}
