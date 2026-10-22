/* Production desktop test loop with deterministic syscall boundary fakes.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "desktop.h"
#undef DT_UNKNOWN
#undef DT_CHR
#undef DT_DIR
#undef DT_REG
#include <dirent.h>
#include "gate.h"
#include <ciuki/input.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#undef ILL_ILLOPC
#undef FPE_INTDIV
#undef SEGV_MAPERR
#undef SEGV_ACCERR
#undef BUS_ADRALN
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
#define CU_PTR(p) ((uintptr_t)(p))
static int output_fd=-1;
static int ciuki_error(uint32_t r) { return r >= (uint32_t)-4095 ? -(int32_t)r : 0; }
static uint32_t ciuki_raw_probe_report(uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
static void *gate_mmap(void *,size_t,int,int,int,off_t);
#define CIUKI_RUNTIME_H
#define CIUKI_RAW_H
#define mmap gate_mmap
#define main desktop_main
#include "../../../apps/desktop/desktop.c"
/* The deterministic gate has no bundled-volume backend. */
static unsigned game_queries;
int ciuki_dosvm(uint32_t op,struct ciuki_dosvm_command *c)
{if(op==CIUKI_DOSVM_GAME_QUERY){game_queries++;c->state=1;return 0;}errno=ENOSYS;return -1;}
/* f4-18: desktop.c saves the master volume preference through libreg (sound_storage.c);
 * the gate exercises the display contract, so the harness supplies the persistence hook. */
int master_preference_save(const char *directory, unsigned left, unsigned right, unsigned mute)
{ (void)directory; (void)left; (void)right; (void)mute; return 0; }

#undef main
#undef mmap
/* f2-33: only the Task Manager scenario lists processes; not exercised here. */
int ciuki_proc_list(void *buffer, uint32_t bytes) { (void)buffer; (void)bytes; errno = ENOSYS; return -1; }
/* Gate/replay hosts leave audio hooks unset; the worker has its own host harness. */
void desk_sound_init(struct desktop *d) { (void)d; }
void desk_sound_results(struct desktop *d) { (void)d; }
/* Shell services the gate harness does not model: mixer control and power. */
int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size) { (void)fd; (void)op; (void)record; (void)size; errno = ENOSYS; return -1; }
int ciuki_power(uint32_t action) { (void)action; errno = ENOSYS; return -1; }
int ciuki_launch_mark(uint32_t event, ciuki_pid_t pid, uint64_t click_us)
{ (void)event; (void)pid; (void)click_us; return 0; } /* quiet gate */
int ciuki_proc_terminate(ciuki_pid_t pid, int signo, int authority_fd)
{ (void)pid; (void)signo; (void)authority_fd; errno=ENOSYS; return -1; }
static jmp_buf done;
static struct gate_control page_storage, *control_page=&page_storage;
#define page (*control_page)
static uint32_t output_pixels[320*320];
static uint32_t client_pixels[(240*160*4+4095)/4096*1024];
static bool handshake;
static const char *first_frame_portrait, *first_frame_output;
static bool normal_first_frame;
static bool input_error, report_error;
static bool late_input, no_input, keyboard_attached;
static unsigned empty_reads, ipc_polls, presents_before_attach;
static unsigned idle_turns;
static bool inherit_error, spawn_error, normal_spawn;
static bool burst, burst_started, flush_timeout, bad_command;
static unsigned burst_at, blocked_sends, configure_attempts, interaction_reports;
static _Atomic uint64_t host_now=1000000;
static uint64_t burst_end, last_interaction_report;
static pthread_t gate_main_thread;
#define BURST_EVENTS (3000+200+74)
static FILE *client_messages, *server_messages;
static int pair_client[256],client_pid[256],hello[256];
void desktop_spawn_host_init(int);
int desktop_spawn_host_display_fd(void);
int desktop_spawn_host_input_fd(void);
void desktop_spawn_host_shift_grants(void);
int desktop_spawn_host_open_node(const char *);
unsigned desktop_spawn_host_node_opens(void);
int desktop_spawn_host_output(void);
uint32_t desktop_spawn_host_control(void);
void desktop_spawn_host_input(const struct input_event *,struct ciuki_input_event *);
void desktop_spawn_host_no_input(void);
void desktop_spawn_host_attach_keyboard(void);
int desktop_spawn_host_read_input(int,struct ciuki_input_event *,uint32_t);
int desktop_spawn_host_pair(int32_t [2]);
int desktop_spawn_host_fcntl(int,int,uint32_t);
int desktop_spawn_host_close(int);
int desktop_spawn_host_spawn(const char *,char *const [],char *const [],const struct ciuki_spawn_fd *,uint32_t,uint32_t);
void desktop_spawn_host_reap(int);
void desktop_spawn_host_finish(void);
static unsigned faults, pongs, remaining, running, reaped, presents, summary_count, interaction_sent;
static int victim_live, victim_dead, registered_victim;
static unsigned generation=1;
static bool interact_ready;
static void command(unsigned cmd) { page.generation=generation++;page.command=cmd;if(cmd==GATE_INTERACT)interact_ready=true; }
static void *gate_mmap(void *addr,size_t n,int prot,int flags,int fd,off_t off)
{
    assert(!addr && n==4096 && prot==(PROT_READ|PROT_WRITE) && flags==(MAP_PRIVATE|MAP_ANONYMOUS) && fd==-1 && !off);
    uintptr_t base=desktop_spawn_host_control();
    if (handshake) {
        /* Match the kernel's first-fit placement after TLS and the output
         * surface; the kernel harness checks this with ua_mmap/surface_map.
         * POSIX chooses placement for NULL, rather than promising the arena
         * base: https://pubs.opengroup.org/onlinepubs/9799919799/functions/mmap.html */
        control_page=mmap((void *)base,n,prot,flags|MAP_FIXED_NOREPLACE,fd,off);
        assert(control_page==(void *)base);
    }
    return control_page;
}
static uint32_t ciuki_raw_probe_report(uintptr_t address,uint32_t n,uint32_t a,uint32_t b,uint32_t c,uint32_t e)
{
    (void)a;(void)b;(void)c;(void)e;
    const char *line=(void *)address;assert(n==strlen(line) && n<=CIUKI_PROBE_REPORT_MAX);
    if (first_frame_output) return 0; /* no controller stimulus before capture */
    if (!strncmp(line,"case=native-setup ",18)) { puts(line); fflush(stdout); return 0; }
    if (input_error || inherit_error || spawn_error) { puts(line); return 0; }
    if (report_error) return (uint32_t)-EFAULT;
    if (handshake) {
        puts(line);
        if (strstr(line,"generation=0 ")) command(GATE_SNAPSHOT);
        else longjmp(done,1);
        return 0;
    }
    const char *published=strstr(line," victim=");assert(published);
    registered_victim=atoi(published+8);
    summary_count++;
    if (late_input || no_input) {
        if (strstr(line,"generation=0 ")) command(GATE_INTERACT);
        if (gate_keys==2) {
            assert(late_input && keyboard_attached && presents_before_attach && empty_reads>100 && ipc_polls>100);
            longjmp(done,1);
        }
        return 0;
    }
    if (bad_command && strstr(line,"generation=0 ")) { command(99); return 0; }
    if (burst && interact_ready) {
        interaction_reports++;
        assert(interaction_reports==1 || host_now-last_interaction_report>=GATE_REPORT_NS);
        last_interaction_report=host_now;
        /* Match the probe: stimulus starts only after GATE_INTERACT is
         * acknowledged and ARM can be emitted, never during its barrier. */
        if (!burst_started) {
            assert(!gate_keys && !gate_motion && !gate_buttons);
            burst_started=true;
            return 0;
        }
        if (gate_keys==200 && gate_motion==3000 && gate_buttons==74 && burst_at==BURST_EVENTS &&
            host_now>=burst_end+20000000ull) {
            assert(blocked_sends>0 && configure_attempts>=4 && interaction_reports>=2);
            assert(host_now<burst_end+250000000ull); /* report with a blocked client */
            printf("dense interaction keys=%u motion=%u buttons=%u reports=%u blocked_sends=%u presents=%u PASS\n",
                gate_keys,gate_motion,gate_buttons,interaction_reports,blocked_sends,presents);
            longjmp(done,1);
        }
        return 0;
    }
    if (strstr(line,"generation=0 ")) command(GATE_SPAWN);
    else if (victim_live && !victim_dead) command(GATE_RELEASE);
    else if (victim_dead) command(GATE_REAP);
    else if (reaped>running) command(GATE_RUN);
    else if (!remaining && pongs==running*100 && !strstr(line,"keys=2 motion=1 buttons=2")) {
        if (faults<5) command(GATE_SPAWN); else command(GATE_INTERACT);
    }
    if (strstr(line,"keys=2 motion=1 buttons=2")) longjmp(done,1);
    return 0;
}
int ciuki_channel_pair(int pair[2]) {
    int result=desktop_spawn_host_pair(pair);
    if (result<0) { errno=-result; return -1; }
    assert(pair[0]>=3 && pair[1]<256); /* above stdio and within the harness tables; host fd numbers depend on inherited descriptors and on what the shell opens before its first spawn */
    pair_client[pair[1]]=pair[0]; hello[pair[0]]=0; return 0;
}
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *mapping,uint32_t count,uint32_t flags)
{
    assert(!strcmp(path,"/Programs/demo") && !strcmp(argv[0],"demo") && !strcmp(argv[1],"--channel-fd=3") &&
        (normal_spawn ? !argv[3] : !strcmp(argv[3],"--test=crash-isolation")) && !argv[4]);
    assert(env && !strcmp(env[0],"LC_ALL=C") && !strcmp(env[1],"TZ=UTC0") && !env[2] &&
        mapping && count==1 && mapping->target==3 && flags==CIUKI_SPAWN_NEW_GROUP);
    if (spawn_error) assert(!desktop_spawn_host_fcntl(mapping->source,F_SETFD,FD_CLOEXEC));
    int pid=desktop_spawn_host_spawn(path,argv,env,mapping,count,flags);
    if (pid<0) { errno=-pid; return -1; }
    client_pid[pair_client[mapping->source]]=pid;
    if (strcmp(argv[2],"--fault=none")) {
        char expected[64];snprintf(expected,sizeof(expected),"--fault=%s",gate_faults[faults%5]);
        assert(!strcmp(argv[2],expected));faults++;victim_live=pid;
    }
    return pid;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    assert((flags & DONTWAIT) && !(flags & ~(DONTWAIT | CIUKI_CHANNEL_FOCUS | CIUKI_CHANNEL_UNFOCUS | CIUKI_CHANNEL_INPUT)) && fd>=3 && fd<256);
    if (burst && m->data[1]==DESK_CONFIGURE && configure_attempts++<4) {
        errno=EAGAIN; blocked_sends++; return -1;
    }
    if (burst && burst_started && burst_at && (!burst_end || host_now<burst_end+250000000ull)) {
        errno=blocked_sends++%2 ? EAGAIN : EINTR; return -1;
    }
    if (flush_timeout && m->data[1]==DESK_CONFIGURE) { errno=EAGAIN; return -1; }
    if (handshake) {
        struct ciuki_message delivered=*m; delivered.sender_pid=41;
        assert(fwrite(&delivered,sizeof(delivered),1,server_messages)==1);
        return 0;
    }
    if (m->length==12 && desk_get32(m->data)==GATE_MAGIC) {
        unsigned cmd=desk_get32(m->data+4);
        if (cmd==GATE_RELEASE) { assert(client_pid[fd]==victim_live);victim_dead=1; }
        else if (cmd==GATE_RUN) { assert(client_pid[fd]==42 && !remaining);remaining=100;running++; }
        else assert(cmd==GATE_SNAPSHOT && client_pid[fd]==42);
    } else if (m->data[1]==DESK_PONG) pongs++;
    return 0;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    ipc_polls++;
    assert(flags==DONTWAIT && fd>=3 && fd<256);
    if (normal_spawn) { assert(client_pid[fd]==42); longjmp(done,1); }
    if (handshake) {
        if (fread(m,sizeof(*m),1,client_messages)==1) return 1;
        assert(feof(client_messages)); errno=EAGAIN; return -1;
    }
    if(client_pid[fd]!=42) assert(client_pid[fd]==registered_victim);
    if (!hello[fd]++) desk_message(m,DESK_HELLO,4);
    else if ((burst || flush_timeout) && client_pid[fd]==42 && hello[fd]==2) {
        desk_message(m,DESK_CREATE_WINDOW,17);
        desk_put32(m->data+4,240); desk_put32(m->data+8,160);
        desk_put32(m->data+12,1); m->data[16]='T';
        m->fd_count=1; m->fds[0]=127;
    }
    else if (client_pid[fd]==42 && remaining) {desk_message(m,DESK_PING,8);desk_put32(m->data+4,101-remaining--);}
    else {errno=EAGAIN;return -1;}
    m->sender_pid=client_pid[fd];return 1;
}
int ciuki_display_info(int fd,struct ciuki_framebuffer_info *s)
{ assert(fd==desktop_spawn_host_display_fd());s->width=s->height=320;return 0; }
int ciuki_surface_create(uint32_t w,uint32_t h,uint32_t f)
{assert(w==320 && h==320 && f==CIUKI_SURFACE_XRGB8888);return output_fd=desktop_spawn_host_output();}
void *ciuki_surface_map(int fd,int prot) {
    if ((handshake && fd==7) || ((burst || flush_timeout) && fd==127)) { assert(prot==PROT_READ); return client_pixels; }
    assert(fd==output_fd && prot==(PROT_READ|PROT_WRITE));return output_pixels;
}
int ciuki_surface_info(int fd,struct ciuki_surface_info *s) {
    assert((handshake && fd==7) || ((burst || flush_timeout) && fd==127));
    *s=(struct ciuki_surface_info){sizeof(*s),240,160,240*4,CIUKI_SURFACE_XRGB8888,sizeof(client_pixels)};return 0;
}
int ciuki_present(int display,int surface,const struct ciuki_rect *r)
{
    assert(display==desktop_spawn_host_display_fd() && surface==output_fd && r);presents++;
    if (first_frame_output) {
        assert(presents==1 && !gate_keys && !gate_motion && !gate_buttons && !interaction_sent);
        FILE *out=fopen(first_frame_output,"wb"); assert(out);
        assert(fwrite(output_pixels,sizeof(*output_pixels),320*320,out)==320*320);
        assert(!fclose(out));
        longjmp(done,1);
    }
    return 0;
}
int ciuki_input_read(int fd,struct ciuki_input_event *events,uint32_t capacity)
{
    assert(fd==desktop_spawn_host_input_fd() && capacity==CIUKI_INPUT_READ_MAX);
    if (first_frame_output) { errno=EAGAIN; return -1; }
    if (late_input || no_input) {
        if (late_input && !keyboard_attached && host_now>=5000000000ull) {
            assert(presents && !gate_keys && ipc_polls>100);
            presents_before_attach=presents;
            desktop_spawn_host_attach_keyboard(); keyboard_attached=true;
        }
        int n=desktop_spawn_host_read_input(fd,events,capacity);
        if (n<0) { assert(n==-EAGAIN); empty_reads++; errno=-n; return -1; }
        for (int i=0;i<n;i++) assert(events[i].source==INPUT_USB && events[i].generation==23);
        return n;
    }
    if (burst && !burst_started) { errno=EAGAIN; return -1; }
    if (burst && burst_started) {
        unsigned n=0;
        while (n<capacity && burst_at<BURST_EVENTS) {
            struct input_event in={.source=INPUT_NATIVE,.generation=23,.sequence=burst_at+1};
            if (burst_at<3000) { in.type=INPUT_REL; in.code=INPUT_X; in.value=burst_at%2 ? -1 : 1; }
            else if (burst_at<3200) { in.type=INPUT_KEY; in.code=INPUT_KEY_A; in.value=burst_at%2 ? 0 : 1; }
            else { in.type=INPUT_BTN; in.code=INPUT_LEFT; in.value=burst_at%2 ? 0 : 1; }
            desktop_spawn_host_input(&in,&events[n++]); burst_at++;
        }
        if (n) { if (burst_at==BURST_EVENTS) burst_end=host_now; return (int)n; }
        errno=EAGAIN; return -1;
    }
    if (interact_ready && !interaction_sent++) {
        events[0]=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=0x1e,.value=1};
        events[1]=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=0x1e,.value=0};
        events[2]=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=-20,.value2=20};
        events[3]=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};
        events[4]=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=0};
        struct input_event wheel={.type=INPUT_REL,.code=INPUT_WHEEL,.value=-1,.source=INPUT_USB,.generation=23};
        desktop_spawn_host_input(&wheel,&events[5]);
        wheel.value=1; desktop_spawn_host_input(&wheel,&events[6]);
        return 7;
    }
    errno=EAGAIN;return -1;
}
int desk_load_portrait(uint32_t *out)
{
    if (!first_frame_portrait) { memset(out,0,256*256*4); return 0; }
    FILE *source=fopen(first_frame_portrait,"rb"); assert(source);
    assert(fread(out,sizeof(*out),256*256,source)==256*256);
    assert(!fclose(source)); return 0;
}
int open(const char *path,int flags,...) {
    (void)flags;
    int result=desktop_spawn_host_open_node(path);
    if (result<0) { errno=-result; return -1; }
    return result;
}
int fcntl(int fd,int cmd,...) {
    uint32_t arg=0;
    if (cmd==F_SETFL || cmd==F_SETFD) { va_list ap; va_start(ap,cmd); arg=(uint32_t)va_arg(ap,int); va_end(ap); }
    if (input_error && cmd==F_SETFL) { errno=EIO; return -1; }
    if (inherit_error && cmd==F_SETFD) { errno=EACCES; return -1; }
    int result=desktop_spawn_host_fcntl(fd,cmd,arg);
    if (result<0) { errno=-result; return -1; }
    return result;
}
int close(int fd) {
    if ((burst || flush_timeout) && fd==127) return 0;
    int result=desktop_spawn_host_close(fd);
    /* Cleanup must preserve the original setup error, even if close changes
     * errno. Native report files use stdio and bypass this guest-fd seam. */
    if (inherit_error || spawn_error) errno=EIO;
    if (result<0) { errno=-result; return -1; }
    return result;
}
int munmap(void *p,size_t n) { (void)p;(void)n;return 0; }
pid_t waitpid(pid_t pid,int *status,int flags) {assert(pid==victim_live && victim_dead && flags==WNOHANG);*status=(faults%5==2) ? SIGPIPE : SIGSEGV;desktop_spawn_host_reap(pid);victim_live=victim_dead=0;reaped++;return pid;}
int clock_gettime(clockid_t clock,struct timespec *out)
{
    assert(clock==CLOCK_MONOTONIC || clock==CLOCK_REALTIME);
    if (clock==CLOCK_REALTIME) { out->tv_sec=43210; out->tv_nsec=1000000; return 0; }
    uint64_t stamp=host_now;
    out->tv_sec=stamp/1000000000ull; out->tv_nsec=stamp%1000000000ull; return 0;
}
int nanosleep(const struct timespec *t,struct timespec *rest)
{
    /* Background observers must not advance the deterministic desktop clock
     * or longjmp through a checkpoint owned by the main thread. */
    if (!pthread_equal(pthread_self(),gate_main_thread)) return (int)syscall(SYS_nanosleep,t,rest);
    (void)rest;assert(!t->tv_sec && t->tv_nsec>0 && t->tv_nsec<=10000000);
    if (no_input && host_now>=3000000000ull && host_now<4000000000ull) {
        assert(t->tv_nsec==10000000); idle_turns++;
    }
    host_now+=(uint64_t)t->tv_nsec;assert(host_now<10000000000ull);
    if (no_input && host_now>=6000000000ull) {
        assert(presents && empty_reads>100 && ipc_polls>100 && summary_count>=2 && !gate_keys);
        assert(idle_turns && idle_turns<=100);
        printf("PASS F2-86 production idle: service_turns_per_second=%u\n",idle_turns);
        longjmp(done,1);
    }
    if (running && !remaining && pongs==running*100 && !page.command && !interact_ready) command(GATE_SNAPSHOT);
    return 0;
}
static int run_desktop(int argc,char **argv)
{
    char display[32],input[32];
    snprintf(display,sizeof(display),"--display-fd=%d",desktop_spawn_host_display_fd());
    snprintf(input,sizeof(input),"--input-fd=%d",desktop_spawn_host_input_fd());
    char *args[8]; assert(argc+2<(int)(sizeof(args)/sizeof(*args)));
    for (int i=0;i<argc;i++) args[i]=argv[i];
    args[argc]=display; args[argc+1]=input; args[argc+2]=NULL;
    return desktop_main(argc+2,args);
}
static atomic_uint gate_folder_entered, gate_folder_release;
static int gate_folder_list(void *ctx,const char *path,unsigned flags,fm_emit emit,void *arg)
{
    (void)ctx; struct fm_meta meta={.type=FM_DIR};
    if (!strcmp(path,"/")) return emit("Documents",&meta,arg);
    if (!strcmp(path,"/Documents")) return emit("Desktop",&meta,arg);
    assert(!strcmp(path,DESK_FOLDER));
    /* Initial/explicit browser reads are cheap too; stall only the poll. */
    if (flags==FM_LIST_NAMES_ONLY && !pthread_equal(pthread_self(),gate_main_thread)) {
        atomic_store(&gate_folder_entered,1);
        while (!atomic_load(&gate_folder_release)) { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
    }
    meta.type=FM_FILE; meta.size=10;
    return emit(atomic_load(&gate_folder_release) ? "External.txt" : "Original.txt",&meta,arg);
}
static int gate_folder_stat(void *ctx,const char *path,struct fm_meta *meta)
{ (void)ctx; assert(!strcmp(path,DESK_FOLDER)); *meta=(struct fm_meta){.type=FM_DIR}; return 0; }
static void gate_folder_poll(void)
{
    static struct desktop d; const struct desk_ops ops={0};
    static const struct fm_vfs fs={.list=gate_folder_list,.stat_path=gate_folder_stat};
    desk_init(&d,&ops,640,480); desk_shell_enable(&d);
    assert(!desk_folder_init(&d,&fs));
    d.now_ns=1000000000ull; desk_folder_service(&d);
    for (unsigned i=0;!atomic_load(&gate_folder_entered);i++) {
        desk_folder_service(&d); /* retry if startup briefly owned the mailbox */
        assert(i<3000); struct timespec pause={0,1000000}; syscall(SYS_nanosleep,&pause,NULL);
    }
    assert(!strcmp(desk_folder_activity(&d),"directory-compare") && !shutdown_wait(&d));
    for (unsigned i=0;i<100;i++) {
        uint64_t begin=monotonic(); desk_folder_service(&d);
        assert(monotonic()-begin<50000000ull);
        assert(!strcmp(d.sh.folder->model.entries[0].name,"Original.txt"));
    }
    desktop_watchdog.desktop=&d; desktop_watchdog.frame_ms=100; desktop_watchdog.loop_ms=700;
    desktop_watchdog.work=1; desktop_wait("host-wait");
    char line[512]; assert(desktop_stall_record(701,line,sizeof(line))>0);
    assert(strstr(line,"folder_wait=directory-compare") && strstr(line,"wait=host-wait"));
    memset(&desktop_watchdog,0,sizeof(desktop_watchdog));
    atomic_store(&gate_folder_release,1);
    for (unsigned i=0;desk_folder_busy(&d);i++) {
        assert(i<3000); struct timespec pause={0,1000000}; syscall(SYS_nanosleep,&pause,NULL);
    }
    /* A worker completion cannot publish icons until this frame consumes it. */
    assert(!strcmp(d.sh.folder->model.entries[0].name,"Original.txt"));
    desk_folder_service(&d); assert(!strcmp(d.sh.folder->model.entries[0].name,"External.txt"));
    assert(shutdown_wait(&d)); desk_folder_destroy(&d); desk_render_release(&d);
    free(d.sh.icons); free(d.sh.autoplay.workspace);
    puts("PASS F2-79 desktop gate: held folder I/O, frame-only publication, shutdown drain and observer phase");
}
static void idle_policy_test(void)
{
    struct desktop *d=calloc(1,sizeof(*d)); assert(d);
    struct desk_ops ops={0}; desk_init(d,&ops,320,320);
    memset(&d->damage,0,sizeof(d->damage)); d->cursor_dirty=0;
    uint64_t now=1000000000ull, wait;
    for(unsigned i=0;i<100;i++) { wait=desktop_pause_ns(d,now,0,UINT64_MAX); assert(wait==10000000ull); now+=wait; }
    assert(now==2000000000ull);
    /* A stationary held button/drag has no work or deadline of its own. */
    d->buttons=1; d->drag=0; d->sh.volume_drag=1;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==10000000ull);
    d->buttons=0; d->drag=-1; d->sh.volume_drag=0;
    assert(desktop_pause_ns(d,now,1,UINT64_MAX)==1000000ull);
    assert(desktop_pause_ns(d,now,0,now+500000ull)==500000ull);
    d->damage.count=1; d->next_frame_ns=now+2000000ull;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==2000000ull);
    d->next_frame_ns=now; assert(desktop_pause_ns(d,now,0,UINT64_MAX)==1000000ull); d->damage.count=0;
    d->animation.active=1; d->animation.started=now-22000000ull;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==3000000ull); d->animation.active=0;
    struct desk_client *c=&d->clients[0]; c->channel=3; c->hello=1; c->next_ping_ns=now+4000000ull;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==4000000ull);
    c->ping_pending=1; c->ping_sent_ns=now-4995000000ull;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==5000000ull);
    c->channel=-1;
    d->shell=1; d->sh.notification.active=1; d->sh.notification.started=now-6000000ull; d->sh.notification.expires=now+1000000000ull;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==2000000ull); d->sh.notification.active=0;
    d->sh.hover_target=1; d->sh.hover_since=now-599000000ull;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==1000000ull); d->sh.hover_since=0;
    d->children[0].pid=42; d->children[0].close_ns=now+7000000ull;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==7000000ull);
    /* Suspended shell work cannot keep a static shutdown panel polling. */
    d->power.state=DESK_POWER_BUSY; d->children[0].close_ns=now-1;
    d->sh.notification.active=1; d->sh.notification.expires=now-1;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==10000000ull);
    d->power.state=DESK_POWER_CLOSING; d->power.close_deadline=now+3000000ull;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==3000000ull);
    d->power.state=DESK_POWER_RETRY; d->power.next_try=now+2000000ull;
    assert(desktop_pause_ns(d,now,0,UINT64_MAX)==2000000ull);
    d->sh.gate=1; assert(desktop_pause_ns(d,now,0,UINT64_MAX)==10000000ull);
    /* Input arriving just after the last drain waits at most 10 ms. Its
     * presentation slot adds <=16.667 ms, within the existing 50 ms target. */
    d->shell=0; d->now_ns=now;
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.monotonic_ns=now+1,.value=10,.value2=10};
    wait=desktop_pause_ns(d,now,0,UINT64_MAX); d->now_ns+=wait; desk_input(d,&e);
    assert(d->stats.input_lag_ns==wait-1 && d->stats.input_lag_ns<=10000000ull);
    assert(d->stats.input_lag_ns+DESK_FRAME_NS<50000000ull);
    free(d);
    puts("PASS F2-86 idle policy: 100 turns/s, input lag <=10 ms, frame budget <50 ms, timer/frame/animation/heartbeat/shell deadlines");
}
/* f2-95: input arriving at any moment of the idle wait is read when the wait
 * ends (<=10 ms) and is shown by the next frame slot: the small-damage cap
 * never applies to a desktop with recent input. The stall observer counts a
 * hardware-cursor-only frame (no row written) as a frame. */
static void idle_wake_test(void)
{
    struct desktop *d=calloc(1,sizeof(*d)); assert(d);
    struct desk_ops ops={0}; desk_init(d,&ops,1024,768);
    memset(&d->damage,0,sizeof(d->damage)); d->cursor_dirty=0;
    struct desk_client *c=&d->clients[0]; c->channel=3; c->hello=1; c->window=1;
    uint64_t now=5000000000ull, worst_wait=0, worst_show=0;
    /* States: idle, an application's small continuous damage (30 fps), a
     * hover deadline, an animation and a pending heartbeat. */
    for (unsigned state=0;state<5;state++) for (uint64_t offset=0;offset<=10000000ull;offset+=250000ull) {
        memset(&d->damage,0,sizeof(d->damage)); d->animation.active=0; d->cursor_dirty=0; d->frame_input_ns=0;
        d->last_input_ns=now-1000000000ull; d->last_frame_ns=now-1000000ull; d->next_frame_ns=now+15666667ull;
        c->next_ping_ns=now+60000000000ull;
        if (state==1) desk_damage_add(&d->damage,(struct desk_box){100,100,20,20},d->width,d->height);
        if (state==2) { d->shell=1; d->sh.hover_target=1; d->sh.hover_since=now-100000000ull; }
        if (state==3) { d->animation.active=1; d->animation.started=now; d->animation.step=0; }
        if (state==4) c->next_ping_ns=now+3000000ull;
        uint64_t wait=desktop_pause_ns(d,now,0,UINT64_MAX);
        d->shell=0; d->sh.hover_target=0; d->sh.hover_since=0;
        assert(wait<=10000000ull);
        if (offset>wait) continue;
        /* The loop drains at now+wait; input that arrived at now+offset waits wait-offset. */
        if (wait-offset>worst_wait) worst_wait=wait-offset;
        uint64_t drained=now+wait;
        struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.monotonic_ns=now+offset,.value=1,.value2=0};
        d->now_ns=drained; desk_input(d,&e);
        assert(!desk_small_damage(d,drained));
        uint64_t due=desk_frame_time(d,drained);
        uint64_t show=(due>drained ? due-drained : 0)+(wait-offset);
        if (show>worst_show) worst_show=show;
        assert(wait-offset<=10000000ull && show<=10000000ull+DESK_FRAME_NS);
    }
    /* Without input the same small damage is spaced at 30 fps, never more. */
    memset(&d->damage,0,sizeof(d->damage)); d->cursor_dirty=0; d->frame_input_ns=0; c->next_ping_ns=now+60000000000ull;
    d->last_input_ns=now-1000000000ull; d->last_frame_ns=now; d->next_frame_ns=now+DESK_FRAME_NS;
    desk_damage_add(&d->damage,(struct desk_box){100,100,20,20},d->width,d->height);
    assert(desk_small_damage(d,now) && desk_frame_time(d,now)==now+DESK_SMALL_FRAME_NS);
    assert(!desk_frame_due(d,now+DESK_FRAME_NS) && desk_frame_due(d,now+DESK_SMALL_FRAME_NS));
    assert(desktop_pause_ns(d,now+DESK_FRAME_NS,0,UINT64_MAX)==10000000ull);
    assert(desktop_pause_ns(d,now+DESK_FRAME_NS+10000000ull,0,UINT64_MAX)==DESK_SMALL_FRAME_NS-DESK_FRAME_NS-10000000ull);
    /* An isolated update is not delayed: the previous frame is long past. */
    d->last_frame_ns=now-1000000000ull; assert(desk_frame_time(d,now)==d->next_frame_ns);
    /* Stall observer: a cursor-only frame (presents unchanged) clears the
     * pending input and renews the frame stamp; no record follows. */
    memset(&desktop_watchdog,0,sizeof(desktop_watchdog));
    memset(&d->damage,0,sizeof(d->damage)); d->cursor_dirty=0;
    uint32_t base=(uint32_t)(monotonic()/1000000ull);
    __atomic_store_n(&desktop_watchdog.frame_ms,base-600,__ATOMIC_RELEASE);
    __atomic_store_n(&desktop_watchdog.loop_ms,base,__ATOMIC_RELEASE);
    desktop_input_pending(); desktop_stage(STALL_IDLE);
    char line[512];
    assert(desktop_stall_record(base,line,sizeof(line))>0 && strstr(line,"stage=idle")); /* a stale stamp reports */
    d->now_ns=(uint64_t)base*1000000ull; errno=0; desktop_watchdog_frame(d,d->presents,0);
    assert(!__atomic_load_n(&desktop_watchdog.pending,__ATOMIC_ACQUIRE));
    assert(!desktop_stall_record(base+400,line,sizeof(line)));
    /* A frame not yet due keeps pending input; a stale errno is not a frame. */
    desktop_input_pending(); d->cursor_dirty=1; errno=0; desktop_watchdog_frame(d,d->presents,-1);
    assert(__atomic_load_n(&desktop_watchdog.pending,__ATOMIC_ACQUIRE));
    memset(&desktop_watchdog,0,sizeof(desktop_watchdog));
    free(d);
    printf("PASS F2-95 idle wake: input during the idle wait read within %llu us (<=10 ms), shown within %llu us; "
           "small damage 30 fps without input; cursor-only frame clears the stall observer\n",
           (unsigned long long)(worst_wait/1000),(unsigned long long)(worst_show/1000));
}
/* f2-95: shortcut scans. Temporary roots replace the four production trees. */
static void link_write(const char *path,const char *label,const char *target,time_t mtime)
{
    struct desk_launch_record r={0}; snprintf(r.label,sizeof(r.label),"%s",label);
    snprintf(r.target,sizeof(r.target),"%s",target); r.show=1;
    uint8_t bytes[DESK_LINK_BYTES]; unsigned n=desk_link_encode(&r,bytes,sizeof(bytes)); assert(n);
    FILE *f=fopen(path,"wb"); assert(f && fwrite(bytes,1,n,f)==n && !fclose(f));
    struct timespec times[2]={{mtime,0},{mtime,0}}; assert(!utimensat(AT_FDCWD,path,times,0));
}
static void links_policy_test(void)
{
    const char *tmp=getenv("TMPDIR"); char base[200];
    snprintf(base,sizeof(base),"%s/links-XXXXXX",tmp && *tmp ? tmp : "/tmp"); assert(mkdtemp(base));
    char roots[4][260], path[300];
    for (unsigned r=0;r<4;r++) { snprintf(roots[r],sizeof(roots[r]),"%s/r%u",base,r); assert(!mkdir(roots[r],0700)); }
    snprintf(path,sizeof(path),"%s/Tools",roots[0]); assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/Tools/Beta.lnk",roots[0]); link_write(path,"Beta","C:\\BETA.EXE",1700000000);
    snprintf(path,sizeof(path),"%s/Alpha.lnk",roots[1]); link_write(path,"Alpha","C:\\ALPHA.EXE",1700000000);
    snprintf(path,sizeof(path),"%s/readme.txt",roots[2]); FILE *f=fopen(path,"w"); assert(f && fputs("x",f)>=0 && !fclose(f));
    snprintf(path,sizeof(path),"%s/Gamma.ciuki",roots[3]); link_write(path,"Gamma","/Programs/gamma",1700000000);
    const char *saved[4]; memcpy(saved,links_roots,sizeof(saved));
    for (unsigned r=0;r<4;r++) links_roots[r]=roots[r];
    struct links_worker *w=calloc(1,sizeof(*w)); assert(w);
    assert(!pthread_mutex_init(&w->mutex,NULL) && !pthread_cond_init(&w->changed,NULL));
    unsigned queries=game_queries;
    links_scan_once(w,1);
    assert(w->count==3 && !strcmp(w->published[0].launch.label,"Alpha") && w->published[0].desktop==1);
    assert(!strcmp(w->published[1].launch.label,"Beta") && !strcmp(w->published[2].launch.label,"Gamma"));
    assert(game_queries==queries+CIUKI_DOSVM_GAME_COUNT && w->reads==3 && w->scans==1 && w->game_scans==1);
    struct desk_link_entry first[3]; memcpy(first,w->published,sizeof(first));
    /* Unchanged shortcuts keep their records: no read, no game query, same bytes. */
    links_scan_once(w,0);
    assert(w->count==3 && w->reads==3 && game_queries==queries+CIUKI_DOSVM_GAME_COUNT && !memcmp(first,w->published,sizeof(first)));
    /* A rewritten shortcut (new time) is read again. */
    snprintf(path,sizeof(path),"%s/Alpha.lnk",roots[1]); link_write(path,"Alpha 2","C:\\ALPHA.EXE",1700000100);
    links_scan_once(w,0); assert(w->reads==4 && !strcmp(w->published[0].launch.label,"Alpha 2"));
    /* Request policy: 2 s snapshots never rescan by themselves before 10 s. */
    struct desktop *d=calloc(1,sizeof(*d)); assert(d);
    w->scanned_ns=1000000000ull;
    links_request(w,d,w->scanned_ns+DESK_LINKS_RESCAN_NS-1); assert(!w->scan_requested);
    links_request(w,d,w->scanned_ns+DESK_LINKS_RESCAN_NS); assert(w->scan_requested && !w->games_requested);
    w->scan_requested=0; w->scanning=1; d->v3_links_force=DESK_LINKS_SCAN|DESK_LINKS_GAMES;
    links_request(w,d,w->scanned_ns); assert(!w->scan_requested && d->v3_links_force); /* a scan in progress may predate it */
    w->scanning=0; links_request(w,d,w->scanned_ns);
    assert(w->scan_requested && w->games_requested && !d->v3_links_force);
    /* One minute of 2 s snapshots without events: 6 scans and no game query
     * (the a4d9454e policy: 30 scans, 90 queries and a stat per entry). */
    w->scan_requested=w->games_requested=0; unsigned scans=0, games=0; uint64_t t=w->scanned_ns;
    for (uint64_t now=t+2000000000ull;now<=t+60000000000ull;now+=2000000000ull) { /* 30 snapshots */
        links_request(w,d,now);
        if (w->scan_requested) { scans++; games+=w->games_requested; w->scan_requested=w->games_requested=0; w->scanned_ns=now; }
    }
    assert(scans==6 && !games);
    for (unsigned r=0;r<4;r++) links_roots[r]=saved[r];
    pthread_mutex_destroy(&w->mutex); pthread_cond_destroy(&w->changed); free(w); free(d);
    snprintf(path,sizeof(path),"%s/Tools/Beta.lnk",roots[0]); assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/Tools",roots[0]); assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/Alpha.lnk",roots[1]); assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/readme.txt",roots[2]); assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/Gamma.ciuki",roots[3]); assert(!unlink(path));
    for (unsigned r=0;r<4;r++) assert(!rmdir(roots[r]));
    assert(!rmdir(base));
    printf("PASS F2-95 shortcut scan: records reused when unchanged (reads=4 over 3 scans), games queried only on request, "
           "60 s idle: scans=%u (was 30) game_queries=%u (was 90)\n",scans,games);
}

int main(int argc,char **argv)
{
    gate_main_thread=pthread_self();
    idle_policy_test();
    idle_wake_test();
    if (argc==2 && !strcmp(argv[1],"--links")) { links_policy_test(); return 0; }
    if (argc==2 && !strcmp(argv[1],"--folder-poll")) { gate_folder_poll(); return 0; }
    if (argc==4 && (!strcmp(argv[1],"--first-frame") || !strcmp(argv[1],"--normal-first-frame"))) {
        first_frame_portrait=argv[2]; first_frame_output=argv[3];
        normal_first_frame=!strcmp(argv[1],"--normal-first-frame");
    } else if (argc==4 && !strcmp(argv[1],"--handshake")) {
        handshake=true;
        client_messages=fopen(argv[2],"rb"); server_messages=fopen(argv[3],"wb");
        assert(client_messages && server_messages);
    } else if (argc==2 && !strcmp(argv[1],"--input-error")) input_error=true;
    else if (argc==2 && !strcmp(argv[1],"--late-input")) late_input=true;
    else if (argc==2 && !strcmp(argv[1],"--no-input")) no_input=true;
    else if (argc==2 && !strcmp(argv[1],"--report-error")) report_error=true;
    else if (argc==2 && !strcmp(argv[1],"--inherit-error")) inherit_error=true;
    else if (argc==2 && !strcmp(argv[1],"--spawn-error")) spawn_error=true;
    else if (argc==2 && !strcmp(argv[1],"--burst")) burst=true;
    else if (argc==2 && !strcmp(argv[1],"--flush-timeout")) flush_timeout=true;
    else if (argc==2 && !strcmp(argv[1],"--bad-command")) bad_command=true;
    else if (argc==2 && (!strcmp(argv[1],"--normal-spawn") || !strcmp(argv[1],"--capacity"))) normal_spawn=true;
    else assert(argc==1);
    desktop_spawn_host_init(!normal_spawn && !normal_first_frame);
    if (getenv("CIUKI_DESKTOP_HOST_SHIFTED")) desktop_spawn_host_shift_grants();
    if (first_frame_output) {
        char *args[]={"desktop","--test=crash-isolation","--gate",NULL};
        if (!setjmp(done)) { (void)run_desktop(normal_first_frame ? 1 : 3,args); assert(0); }
        desktop_spawn_host_finish();
        puts("desktop first frame: no input, production present captured PASS"); return 0;
    }
    if (late_input || no_input) desktop_spawn_host_no_input();
    if (normal_spawn) {
        if (!strcmp(argv[1],"--capacity")) {
            struct desktop *d=calloc(1,sizeof(*d)); assert(d);
            const struct desk_ops ops={.close=close}; desk_init(d,&ops,320,320);
            assert(ciuki_surface_create(320,320,CIUKI_SURFACE_XRGB8888)>=3);
            for (int i=0;i<DESK_CLIENTS;i++) d->clients[i].channel=64+i;
            errno=EBADF; assert(spawn_demo(d,"none",0)==-1 && errno==ENOSPC);
            free(d);
        } else {
            char *args[]={"desktop","--test=crash-isolation","--demo=none",NULL};
            if (!setjmp(done)) { (void)run_desktop(3,args); assert(0); }
        }
        desktop_spawn_host_finish(); return 0;
    }
    char *invalid[]={"desktop","--gate",NULL};assert(desktop_main(2,invalid)==2);
    char *args[]={"desktop","--test=crash-isolation","--gate",NULL};
    if (!setjmp(done)) {
        int result=run_desktop(3,args);
        if (input_error || inherit_error || spawn_error || flush_timeout || bad_command) { assert(result==1); desktop_spawn_host_finish(); return result; }
        fprintf(stderr,"desktop_main returned %d errno=%d\n",result,errno);
        assert(0);
    }
    if (handshake) {
        assert(!fclose(client_messages) && !fclose(server_messages));
        desktop_spawn_host_finish();
        puts("desktop production HELLO/CREATE_WINDOW -> CONFIGURE/FOCUS/snapshot PASS");return 0;
    }
    if (late_input || no_input) {
        assert(desktop_spawn_host_node_opens()>0);
        desktop_spawn_host_finish();
        printf("desktop production main/supervisor/grant: %s draws and services IPC PASS\n",
               late_input ? "no input at start, USB keyboard attaches at 5 s, keys arrive" : "no input for 6 s");
        return 0;
    }
    assert(faults==5 && reaped==5 && running==5 && pongs==500 && presents>0 && summary_count>=26);
    desktop_spawn_host_finish();
    puts("desktop production gate: five fault kinds, spawn mappings/groups, controller pacing, flush barriers, input summaries PASS");
    return 0;
}

int ciuki_display_query(struct ciuki_display_info *i) { (void)i; return -ENOSYS; }
int ciuki_display_set(uint16_t id) { (void)id; return -EPERM; }
int ciuki_display_accel(int fd,const struct ciuki_accel_request *q) { (void)fd;(void)q;return -ENOSYS; }
int ciuki_display_accel_info(int fd,uint32_t generation,struct ciuki_accel_info *out) { (void)fd;(void)generation;(void)out;return -ENOSYS; }

int ap_preference_get(const char *dir,const char *id) { (void)dir; (void)id; return -1; }
int ap_preference_set(const char *dir,const char *id,const char *label,int action)
{ (void)dir; (void)id; (void)label; (void)action; return -1; }
