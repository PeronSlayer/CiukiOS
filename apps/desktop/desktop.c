/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "../gfxdemo/benchmark.h"
#include "sound_runtime.h"
#include "sound_storage.h"
#include "media_notice.h"
#include "settings_io.h"
#include "wallpaper_io.h"
#ifdef __i386__
#include <ciuki/devices.h>
#endif
#include "catalog.h"
#include "gate.h"
#include "../libdeskui/deskui.h"
#include <ciuki/raw.h>
#include <ciuki/runtime.h>
#include <ciuki/channel.h>
#include <ciuki/spawn.h>
#include <ciuki/surface.h>
#include <ciuki/display.h>
#include <ciuki/proc.h>
#include <ciuki/audio.h>
#include <ciuki/net.h>
#include <ciuki/media.h>
#include <ciuki/power.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
static uint64_t monotonic(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000000000ull+(uint32_t)t.tv_nsec;
}
static uint64_t startup_begin;
static uint32_t startup_us[CFG_STARTUP_STAGES];
static unsigned startup_cached;
static unsigned startup_sent;
static void startup_stage(const char *name)
{
    if (!strcmp(name,"wallpaper_cache_read")) {
        for (unsigned i=4;i<=6;i++) if (!__atomic_load_n(&startup_us[i],__ATOMIC_ACQUIRE)) {
            __atomic_fetch_or(&startup_cached,1u<<i,__ATOMIC_RELAXED);
            startup_stage(cfg_startup_stage_names[i]);
        }
    }
    for (unsigned i=0;i<CFG_STARTUP_STAGES;i++) if (!strcmp(name,cfg_startup_stage_names[i])) {
        if (__atomic_load_n(&startup_us[i],__ATOMIC_ACQUIRE)) return;
        uint64_t elapsed=(monotonic()-startup_begin)/1000;
        uint32_t value=elapsed>=UINT32_MAX-1 ? UINT32_MAX : (uint32_t)elapsed+1, empty=0;
        __atomic_compare_exchange_n(&startup_us[i],&empty,value,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
        return;
    }
}
static void wallpaper_update(struct desktop *);
static int number(const char *s, int max)
{
    char *end;
    long n = strtol(s,&end,10);
    return *s && !*end && n >= 0 && n <= max ? (int)n : -1;
}
static int fault_kind(const char *s)
{
    return !strcmp(s,"none") || !strcmp(s,"bad-pointer") || !strcmp(s,"closed-peer") ||
           !strcmp(s,"forged-fd") || !strcmp(s,"grant-fd") || !strcmp(s,"handler-fault");
}
static unsigned gate_keys,gate_motion,gate_buttons;
#define GATE_REPORT_NS 100000000ull
#define GATE_BARRIER_NS 1000000000ull
static int gate_error(const char *path, int error, unsigned generation)
{
    fprintf(stderr,"desktop: gate failure path=%s error=%d generation=%u\n",path,error,generation);
    return 1;
}
/* F12 shell replay: QMP supplies relative mouse input, not window targets.
 * After the 100 fault windows the cascade need not put the menu demo under
 * the script's old (60,80) title / (332,142) close positions. Resolve those
 * two scenario targets from the live frame at the press, then let the real
 * input path perform the drag, close and reap. This never changes a window's
 * geometry/focus or qualifies a stage by itself. Ordinary input is unchanged. */
static void gate_shell_pointer(struct desktop *d, const struct ciuki_input_event *event)
{
    if (!d->shell_test || (d->shell_stage!=3 && d->shell_stage!=4) || d->drag>=0 ||
        event->type!=CIUKI_INPUT_BUTTON || event->code!=CIUKI_BUTTON_LEFT || event->value!=1) return;
    for (int i=0; i<DESK_CLIENTS; i++) {
        const struct desk_client *c=&d->clients[i];
        if (!c->window || c->pid!=d->shell_pid) continue;
        struct desk_box target=d->shell_stage==3 ?
            (struct desk_box){c->frame.x,c->frame.y,c->frame.w,DESK_TITLE} : desk_close_box(c);
        struct ciuki_input_event motion=*event;
        motion.type=CIUKI_INPUT_MOTION; motion.code=0;
        motion.value=target.x+target.w/2-d->mouse_x;
        motion.value2=target.y+target.h/2-d->mouse_y;
        desk_input(d,&motion);
        break;
    }
}
static void gate_input(struct desktop *d, const struct ciuki_input_event *event)
{
    gate_shell_pointer(d,event);
    desk_input(d,event);
    /* f2-30 shell: the A edges count whenever they follow F12; gate_report publishes them only at stage 6
     * (launch, drag, close, reap). Their arrival is not ordered after the close click: the last shell step
     * is a mouse click, and the 8042 hands out a pending keyboard byte before pending aux bytes (QEMU
     * hw/input/pckbd.c kbd_update_irq), so on the firmware input path of qemu-e500 the A edges reached
     * the desktop before the close click (image 639bcca5: stage 6 with keys=0). f2-33 tasks and the app
     * scenarios end with keyboard steps and keep the arrival rule (stage 5 / last checkpoint first; the
     * final report requires DESK_TASKS_DONE / app_done). */
    if (event->type==CIUKI_INPUT_KEY && event->code==0x1e &&
        (event->value==0 || event->value==1) && (!d->tasks_test || d->tasks_stage>=5) &&
        (!d->app_test || d->app_checkpoint>=DESK_APP_CHECKPOINTS(d->app_test))) gate_keys++;
    if (event->type==CIUKI_INPUT_MOTION) gate_motion++;
    /* The input grant delivers separate button edges, not a motion mask.
     * Count consumption even when the click focuses the background/title. */
    if (event->type==CIUKI_INPUT_BUTTON && event->code>=1 && event->code<=3 &&
        (event->value==0 || event->value==1)) gate_buttons++;
}
/* Gate scenarios: the desktop reads input into a FIFO. While a graphical program it launched has not sent
 * its first frame, the scripted input stays there; then it is replayed with its recorded spacing, shifted
 * by the wait. A scripted case then no longer depends on how long the program takes to start: on image
 * 2fdd515a Files was not ready (not yet at its --test frame) when files-qemu-t23 clicked File at (27,70),
 * the click raised the survivor demo's window (20,52) behind it, the typed keys went to the survivor and
 * the shell's Alt+F4 closed it (survivor_exit:0, keys=0). The hold ends after GATE_HOLD_MAX_NS at most. */
#define GATE_INPUT_FIFO 512
#define GATE_HOLD_MAX_NS 30000000000ull
static struct ciuki_input_event input_fifo[GATE_INPUT_FIFO];
static unsigned fifo_head, fifo_count;
static int scenario_hold(const struct desktop *d, uint64_t now)
{
    for (int i=0; i<DESK_CLIENTS; i++) {
        const struct desk_client *c=&d->clients[i];
        if (c->channel>=0 && c->awaiting_frame && now-c->launched_ns<GATE_HOLD_MAX_NS) return 1;
    }
    return 0;
}
static int gate_setup_error(int gate, unsigned step, int error)
{
    if (gate) {
        char line[96];
        int n=snprintf(line,sizeof(line),"case=native-setup step=%u error=%u",step,(unsigned)error);
        if (n>0 && n<(int)sizeof(line)) (void)ciuki_raw_probe_report(CU_PTR(line),n,0,0,0,0);
        fprintf(stderr,"desktop: gate setup failed step=%u error=%d\n",step,error);
    }
    return 1;
}
static void gate_report(struct desktop *d, uint32_t control, unsigned generation, int survivor, int victim, unsigned cycle)
{
    char line[CIUKI_PROBE_REPORT_MAX+1];
    unsigned keys=gate_keys;
    if (d->shell_test) keys=d->shell_stage==6 && gate_keys>=2 ? gate_keys+d->shell_keys : 0;
    if (d->tasks_test) keys=d->tasks_stage==DESK_TASKS_DONE && gate_keys>=2 ? gate_keys+d->shell_keys : 0;
    if (d->app_test) keys=d->app_done && gate_keys>=2 ? gate_keys+d->shell_keys : 0;
    int n=snprintf(line,sizeof(line),"case=native-desktop control=%u generation=%u survivor=%d victim=%d cycle=%u replies=%llu keys=%u motion=%u buttons=%u",
        control,generation,survivor,victim,cycle,(unsigned long long)d->replied,keys,gate_motion,gate_buttons);
    if (d->shell_test && n>0 && n<(int)sizeof(line))
        n+=snprintf(line+n,sizeof(line)-(size_t)n," shell=1 shell_stage=%u",(unsigned)d->shell_stage);
    /* f2-31/f2-32: the Files and CiukNote cases ask the controller for the
     * storage write gate and a framebuffer digest at each presented checkpoint. */
    if (d->app_test && n>0 && n<(int)sizeof(line))
        n+=snprintf(line+n,sizeof(line)-(size_t)n," %s=1 checkpoint=%d",d->app_test==DESK_APP_NOTE ? "note" : "files",d->app_reported);
    int error=n<=0 || n>CIUKI_PROBE_REPORT_MAX ? EINVAL : ciuki_error(ciuki_raw_probe_report(CU_PTR(line),n,0,0,0,0));
    if (error) { fprintf(stderr,"desktop: gate report failed length=%d error=%d\n",n,error); _exit(126); }
}
static int gate_send(struct desktop *d, int pid, unsigned command, unsigned generation)
{
    for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].channel>=0 && d->clients[i].pid==(uint32_t)pid) {
        struct ciuki_message m={.length=12};
        desk_put32(m.data,GATE_MAGIC); desk_put32(m.data+4,command); desk_put32(m.data+8,generation);
        return ciuki_channel_send(d->clients[i].channel,&m,DONTWAIT);
    }
    errno=ESRCH;
    return -1;
}
static int spawn_demo(struct desktop *d, const char *kind, int gate)
{
    int pair[2];
    if (ciuki_channel_pair(pair)) return -1;
    char fault[48];
    snprintf(fault,sizeof(fault),"--fault=%s",kind);
    char *argv[] = {"demo","--channel-fd=3",fault,gate ? "--test=crash-isolation" : NULL,NULL};
    char *env[] = {"LC_ALL=C","TZ=UTC0",NULL};
    struct ciuki_spawn_fd mapping = {pair[1],3};
    int i = -1;
    ciuki_pid_t pid = -1;
    /* channel_pair creates CLOEXEC endpoints. Ciuki's explicit spawn mapping
     * rejects CLOEXEC sources, so admit only the child's channel endpoint;
     * display/input grants and the desktop endpoint keep their flags. */
    if (!fcntl(pair[1],F_SETFD,0)) {
        i = desk_add_client(d,pair[0]);
        if (i >= 0) pid = ciuki_spawn("/Programs/demo",argv,env,&mapping,1,CIUKI_SPAWN_NEW_GROUP);
        else errno = ENOSPC;
    }
    int error = errno;
    close(pair[1]);
    if (pid < 0) {
        if (i >= 0) desk_drop_client(d,i); else close(pair[0]);
        errno = error;
        return -1;
    }
    return (int)pid;
}
static void scan_programs(struct desktop *d)
{
    d->program_count=0;
    DIR *dir=opendir("/Programs");
    if (!dir) { strcpy(d->notice,"Programs are unavailable"); return; }
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        char path[sizeof(d->programs[0].path)];
        int len=snprintf(path,sizeof(path),"/Programs/%s",entry->d_name);
        if (len<=0 || len>=(int)sizeof(path)) continue;
        struct stat st;
        if (stat(path,&st) || !S_ISREG(st.st_mode)) continue;
        FILE *file=fopen(path,"rb");
        if (!file) continue;
        uint8_t header[52]; size_t n=fread(header,1,sizeof(header),file);
        int ok=!ferror(file); if (fclose(file)) ok=0;
        if (ok) desk_program_add(d,entry->d_name,header,n);
    }
    closedir(dir);
}
static int terminate_authority=-1;
static int terminate_child(uint32_t pid, int sig)
{ return ciuki_proc_terminate((ciuki_pid_t)pid,sig,terminate_authority); }
static int launch_failed(const struct desk_program *program, int error, const char *operation)
{
    if (!error) error=EIO;
    /* Native debug_write accepts at most 240 bytes. Inventory paths fit in
     * full; mark a longer Documents executable path's bounded prefix. */
    char line[241];
    int n=snprintf(line,sizeof(line),"[launch] name=%.63s error=%d path=%.108s%s operation=%s",
                   program->name,error,program->path,strlen(program->path)>108 ? "..." : "",operation);
    if (n>0 && (size_t)n<sizeof(line)) {
#ifdef __i386__
        (void)ciuki_raw_debug_write(CU_PTR(line),(uint32_t)n,0,0,0,0);
#else
        (void)write(STDERR_FILENO,line,(size_t)n);
        (void)write(STDERR_FILENO,"\n",1);
#endif
    }
    errno=error; return -1;
}
static int launch_program(struct desktop *d, const struct desk_program *program)
{
    if (d->power.state) return launch_failed(program,EBUSY,"power");
    /* Recheck each launch: the operator can change console verbosity at run time. */
    d->launch_mark = ciuki_launch_mark(CIUKI_LAUNCH_STATUS,0,0) > 0 ? ciuki_launch_mark : NULL;
    uint64_t click_ns = d->launch_click_ns ? d->launch_click_ns : monotonic();
    d->launch_click_ns = 0;
    int child=-1;
    for (int j=0; j<DESK_CLIENTS; j++) if (!d->children[j].pid) { child=j; break; }
    if (child<0) return launch_failed(program,EAGAIN,"child-slot");
    int pair[2];
    if (ciuki_channel_pair(pair)) return launch_failed(program,errno,"channel-pair");
    int index=-1;
    ciuki_pid_t pid=-1;
    int graphical=desk_program_graphical(program->name), kind=desk_program_kind(program->name);
    /* f2-33: only the Task Manager receives the delegated termination
     * authority, explicitly mapped to fd 4; it also may send task requests. */
    int tasks=!strcmp(program->name,"tasks");
    /* Accessories that open a file (Image Viewer, Help) take the LAUNCH argument as CiukNote does. */
    int takes_file=kind==DESK_APP_NOTE || desk_catalog_takes_file(program->name);
    char *argv[]={(char *)program->name,NULL,NULL,NULL,NULL,NULL};
    unsigned at=1;
    if (graphical) argv[at++]="--channel-fd=3";
    /* The scenario's program gets --test=<its name> (apps/desktop/gate.h scenarios). */
    if (graphical && d->app_test && kind==d->app_test) argv[at++]=kind==DESK_APP_NOTE ? "--test=ciuknote" : "--test=files";
    if (tasks && d->authority_fd>=0) argv[at++]="--authority-fd=4";
    if (tasks && d->tasks_test) argv[at++]="--test=tasks";
    /* LAUNCH from Files: the path of the text file CiukNote opens (the client validated it).
     * The shell also opens Files on a folder and the Control Panel on a page. */
    if (graphical && (kind || takes_file) && d->launch_arg) argv[at++]=(char *)d->launch_arg;
    char *full[12];
    if (d->launch_argv[0]) {   /* Run dialog: the typed arguments after the desktop's own */
        unsigned n=0;
        for (unsigned k=0; k<at; k++) full[n++]=argv[k];
        for (unsigned k=0; k<6 && d->launch_argv[k] && n<11; k++) full[n++]=(char *)d->launch_argv[k];
        full[n]=NULL;
    }
    /* Window size hint for graphical clients; the protocol has no query. */
    char display[40];
    snprintf(display,sizeof(display),"CIUKI_DISPLAY=%ux%u",(unsigned)d->width,(unsigned)d->height);
    char keymap[24];   /* the Control Panel's keyboard layout, for libdeskui programs */
    snprintf(keymap,sizeof(keymap),"CIUKI_KEYMAP=%s",cfg_keyboard_names[d->sh.cfg.keyboard==CFG_KEYS_IT ? CFG_KEYS_IT : CFG_KEYS_US]);
    char *env[]={"LC_ALL=C","TZ=UTC0","HOME=/Documents","TMPDIR=/tmp","PATH=/Programs",display,keymap,NULL};
    struct ciuki_spawn_fd mapping[2]={{pair[1],3},{d->authority_fd,4}};
    uint32_t mappings=tasks && d->authority_fd>=0 ? 2 : 1;
    const char *operation="channel-inherit";
    if (!fcntl(pair[1],F_SETFD,0)) {
        index=desk_add_client(d,pair[0]);
        if (index>=0) {
            if (d->launch_mark) d->launch_mark(CIUKI_LAUNCH_BEGIN,0,click_ns/1000);
            operation="spawn";
            pid=ciuki_spawn(program->path,d->launch_argv[0] ? full : argv,env,mapping,mappings,0);
        }
        else { errno=ENOSPC; operation="client-slot"; }
    }
    int error=errno;
    close(pair[1]);
    if (pid<0) {
        if (index>=0) desk_drop_client(d,index); else close(pair[0]);
        return launch_failed(program,error,operation);
    }
    /* Bind launch ownership before accepting the first kernel-stamped message.
     * This also permits cleanup if a child exits without ever sending HELLO. */
    d->clients[index].pid=(uint32_t)pid;
    snprintf(d->clients[index].application,sizeof(d->clients[index].application),"%s",program->name);
    d->clients[index].program=kind;
    d->clients[index].taskman=tasks;
    d->clients[index].gfxdemo=!strcmp(program->name,"gfxdemo");
    d->clients[index].dosbox=!strcmp(program->name,"dosbox");
    d->clients[index].mixer=!strcmp(program->name,"mixer") || !strcmp(program->name,"music") || !strcmp(program->name,"terminal");
    d->clients[index].awaiting_frame=graphical;
    d->clients[index].launched_ns=monotonic();
    d->clients[index].launch_input_ns=click_ns;
    d->children[child].pid=(uint32_t)pid;
    return pid;
}
/* f2-33 scenario: the ended PID has no kernel record any more (reaped, not
 * just exited), while the Task Manager and this desktop are still live. */
static int tasks_verify(const struct desktop *d)
{
    static uint8_t buffer[CIUKI_PROC_LIST_MIN+CIUKI_PROCESS_MAX*sizeof(struct ciuki_proc_record)];
    int n=ciuki_proc_list(buffer,sizeof(buffer));
    if (n<=0) return 0;
    const struct ciuki_proc_system *sys=(const struct ciuki_proc_system *)buffer;
    if (sys->version!=CIUKI_PROC_VERSION || sys->records!=(uint32_t)n || sys->processes!=(uint32_t)n) return 0;
    int demo=0, tasks=0, self=0;
    for (int i=0; i<n; i++) {
        struct ciuki_proc_record r;
        memcpy(&r,buffer+sizeof(*sys)+(size_t)i*sizeof(r),sizeof(r));
        demo|=r.pid==d->tasks_demo_pid;
        tasks|=r.pid==d->tasks_pid && r.state==CIUKI_PROC_LIVE;
        self|=r.pid==d->self_pid && r.state==CIUKI_PROC_LIVE;
    }
    return !demo && tasks && self;
}
/* Start menu and taskbar icons: the Tango store through icon_map.c. */
static const uint32_t *store_icon(const char *object, unsigned size) { return du_icon_argb(du_icon_name(object),size); }
static struct cfg_writer settings_writer;
static int settings_writer_started, verbose_boot;
static int settings_reload_pending, settings_reload_request, settings_reload_wallpaper;
static int theme_read_pending;
static struct desk_wall_worker wallpaper_worker;
static int wallpaper_worker_started, wallpaper_worker_failed;
static struct desk_wall_request wallpaper_request;
static unsigned wallpaper_serial;
enum desktop_stage { STALL_IDLE, STALL_INPUT, STALL_IPC, STALL_DISPLAY,
    STALL_THEME, STALL_LAUNCH, STALL_SHELL, STALL_RENDER, STALL_REPORT };
static struct {
    pthread_t thread;
    unsigned live, stop, stage, pending, work, frame_ms, loop_ms;
    const char *wait;
} desktop_watchdog;
static const char *const desktop_stages[]={"idle","input","ipc","display",
    "theme","launch","shell","render","report"};
static void desktop_wait(const char *name)
{ __atomic_store_n(&desktop_watchdog.wait,name,__ATOMIC_RELEASE); }
static void desktop_stage(unsigned stage)
{
    desktop_wait(stage<sizeof(desktop_stages)/sizeof(*desktop_stages) ? desktop_stages[stage] : "unknown");
    __atomic_store_n(&desktop_watchdog.stage,stage,__ATOMIC_RELEASE);
}
static void desktop_input_pending(void)
{ __atomic_store_n(&desktop_watchdog.pending,1,__ATOMIC_RELEASE); }
static int desktop_stall_record(uint32_t now,char *line,size_t size)
{
    uint32_t frame=__atomic_load_n(&desktop_watchdog.frame_ms,__ATOMIC_ACQUIRE);
    unsigned pending=__atomic_load_n(&desktop_watchdog.pending,__ATOMIC_ACQUIRE);
    unsigned work=__atomic_load_n(&desktop_watchdog.work,__ATOMIC_ACQUIRE);
    uint32_t loop=__atomic_load_n(&desktop_watchdog.loop_ms,__ATOMIC_ACQUIRE);
    /* A blocked loop cannot discover input arriving later in its kernel grant.
     * Also report a lost loop heartbeat, without claiming that input was read. */
    if ((int32_t)(now-frame)<=500 || (!pending && !work && (int32_t)(now-loop)<=500)) return 0;
    unsigned stage=__atomic_load_n(&desktop_watchdog.stage,__ATOMIC_ACQUIRE);
    const char *wait=__atomic_load_n(&desktop_watchdog.wait,__ATOMIC_ACQUIRE);
    const char *wall="idle", *settings="idle", *wall_wait="idle";
    if (wallpaper_worker_started) {
        wall_wait=__atomic_load_n(&wallpaper_worker.activity,__ATOMIC_ACQUIRE);
        if (pthread_mutex_trylock(&wallpaper_worker.lock)) wall="wallpaper-lock";
        else {
            if (wallpaper_worker.ready) wall="wallpaper-ready";
            else if (wallpaper_worker.busy || wallpaper_worker.pending) wall="wallpaper";
            pthread_mutex_unlock(&wallpaper_worker.lock);
        }
    }
    if (settings_writer_started) {
        if (pthread_mutex_trylock(&settings_writer.lock)) settings="settings-lock";
        else {
            if (settings_writer.activity==3 || settings_writer.job_pending) settings="mixer";
            else if (settings_writer.activity==2 || settings_writer.read_pending) settings="settings-read";
            else if (settings_writer.activity==4 || settings_writer.stage_pending || settings_writer.log_pending) settings="settings-log";
            else if (settings_writer.busy || settings_writer.pending) settings="settings-write";
            pthread_mutex_unlock(&settings_writer.lock);
        }
    }
    if (pending!=__atomic_load_n(&desktop_watchdog.pending,__ATOMIC_ACQUIRE) ||
        work!=__atomic_load_n(&desktop_watchdog.work,__ATOMIC_ACQUIRE) ||
        frame!=__atomic_load_n(&desktop_watchdog.frame_ms,__ATOMIC_ACQUIRE) ||
        loop!=__atomic_load_n(&desktop_watchdog.loop_ms,__ATOMIC_ACQUIRE) ||
        wait!=__atomic_load_n(&desktop_watchdog.wait,__ATOMIC_ACQUIRE)) return 0;
    int n=snprintf(line,size,"[desktop] stall stage=%s worker=%s/%s no_frame_ms=%u input_pending=%u work_pending=%u wait=%s wallpaper_wait=%s\n",
        stage<sizeof(desktop_stages)/sizeof(*desktop_stages) ? desktop_stages[stage] : "unknown",
        wall,settings,now-frame,pending,work,wait ? wait : "unknown",wall_wait ? wall_wait : "starting");
    return n>0 && (size_t)n<size ? n : 0;
}
static void *desktop_watchdog_run(void *unused)
{
    (void)unused; uint32_t reported=0;
    while (!__atomic_load_n(&desktop_watchdog.stop,__ATOMIC_ACQUIRE)) {
        uint32_t now=(uint32_t)(monotonic()/1000000ull);
        char line[320]; int n;
        if (now-reported>=500 && (n=desktop_stall_record(now,line,sizeof(line)))>0) {
            /* Native diagnostic call 2 bypasses both FILE and VFS locks and
             * feeds the kernel console/serial/disk bootlog directly. */
#ifdef __i386__
            if (line[n-1]=='\n') --n;
            (void)ciuki_raw_debug_write(CU_PTR(line),(uint32_t)n,0,0,0,0);
#else
            (void)write(STDERR_FILENO,line,(size_t)n);
#endif
            reported=now;
        }
        struct timespec pause={.tv_sec=0,.tv_nsec=100000000}; nanosleep(&pause,NULL);
    }
    return NULL;
}
static void desktop_watchdog_start(struct desktop *d)
{
    __atomic_store_n(&desktop_watchdog.frame_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
    __atomic_store_n(&desktop_watchdog.loop_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
    int error=pthread_create(&desktop_watchdog.thread,NULL,desktop_watchdog_run,NULL);
    if (error) { desk_shell_notify_error(d,"Desktop watchdog unavailable",error); return; }
    desktop_watchdog.live=1; d->input_pending=desktop_input_pending;
}
static void desktop_watchdog_frame(struct desktop *d,uint64_t before,int result)
{
    unsigned waiting=d->sh.wallpaper_pending || settings_reload_pending;
    __atomic_store_n(&desktop_watchdog.work,waiting,__ATOMIC_RELEASE);
    if (result && errno==EBUSY) {
        __atomic_store_n(&desktop_watchdog.frame_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
        __atomic_store_n(&desktop_watchdog.pending,0,__ATOMIC_RELEASE);
        return;
    }
    if (!result && d->presents!=before) {
        __atomic_store_n(&desktop_watchdog.frame_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
        __atomic_store_n(&desktop_watchdog.pending,0,__ATOMIC_RELEASE);
    } else if (!d->damage.count && !d->cursor_dirty && !settings_reload_pending)
        __atomic_store_n(&desktop_watchdog.pending,0,__ATOMIC_RELEASE);
}
static void startup_flush(void)
{
    if (!verbose_boot || !settings_writer_started) return;
    for (unsigned i=0;i<CFG_STARTUP_STAGES;i++) {
        uint32_t value=__atomic_load_n(&startup_us[i],__ATOMIC_ACQUIRE);
        if (value && !(startup_sent&(1u<<i))) {
            if (!cfg_writer_stage(&settings_writer,i,(uint64_t)value-1,!!(__atomic_load_n(&startup_cached,__ATOMIC_RELAXED)&(1u<<i))))
                startup_sent|=1u<<i;
        }
    }
}
static uint32_t mixer_serial;
static int32_t last_ec_preference=-1;
struct mixer_job { uint32_t serial; int fd; struct ciuki_mixer_master master; int save_only; };
static int mixer_job_run(void *record, size_t n)
{
    if (n!=sizeof(struct mixer_job)) return EINVAL;
    struct mixer_job job; memcpy(&job,record,n);
    int error=0;
    if (!job.save_only) error=ciuki_devctl(job.fd,MIXER_SET_MASTER,&job.master,sizeof(job.master)) ? errno : 0;
    if (!error) {
        int saved=master_preference_save(REG_DIRECTORY,job.master.left,job.master.right,job.master.mute);
        job.master.save_state=saved ? CIUKI_MIXER_UNSAVED : CIUKI_MIXER_SAVED;
        job.master.save_error=saved ? -errno : 0;
    }
    memcpy(record,&job,n); return error;
}
/* Volume Control requests (DESK_MIXER_SET): the desktop holds the delegated
 * /dev/mixer control handle (F4 contract, Volume and persistence); the kernel
 * validates the record and saves C:\\SYSTEM\\AUDIO.CFG; this worker saves
 * the authoritative System\\Audio registry preference with libreg. */
#ifdef __i386__   /* the SDK target; host replays (x86-64) have no mixer device */
#include <ciuki/audio.h>
#endif
static int mixer_apply(struct desktop *d, uint32_t left, uint32_t right, uint32_t mute)
{
    if (d->power.state) return -EBUSY;
    struct ciuki_mixer_master m;
    if (d->mixer_fd<0) return -ENODEV;
    memset(&m,0,sizeof(m)); m.version=1; m.size=sizeof(m);
    if (ciuki_devctl(d->mixer_fd,MIXER_GET_MASTER,&m,sizeof(m))) return -errno;
    m.left=left; m.right=right; m.mute=mute; m.save_state=0; m.save_error=0; m.reserved=0;
    if (!settings_writer_started) return -EIO;
    struct mixer_job job={.serial=++mixer_serial,.fd=d->mixer_fd,.master=m};
    return cfg_writer_job(&settings_writer,&job,sizeof(job)) ? -errno : 0;
}
/* ---- desktop shell services: the file and kernel side of shell.c ---- */
static int read_file(const char *path, size_t limit, uint8_t **out, size_t *size)
{
    *out=NULL; *size=0;
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t cap=4096, n=0;
    uint8_t *buffer=malloc(cap);
    int ok=buffer!=NULL;
    while (ok) {
        if (n==cap) {
            if (cap>=limit) { ok=0; errno=EFBIG; break; }
            size_t next=cap*2>limit ? limit : cap*2;
            uint8_t *grown=realloc(buffer,next);
            if (!grown) { ok=0; break; }
            buffer=grown; cap=next;
        }
        size_t got=fread(buffer+n,1,cap-n,f);
        n+=got;
        if (got==0) { ok=!ferror(f); break; }
    }
    if (fclose(f)) ok=0;
    if (!ok) { free(buffer); return -1; }
    *out=buffer; *size=n;
    return 0;
}
static void settings_load(struct desktop *d)
{
    cfg_defaults(&d->sh.cfg);
    uint8_t *text; size_t n;
    if (read_file(CFG_FILE,CFG_TEXT_MAX,&text,&n)) return;   /* first boot: defaults */
    int rejected=cfg_parse(&d->sh.cfg,(const char *)text,n);
    free(text);
    if (rejected) snprintf(d->notice,sizeof(d->notice),"desktop.cfg: %d line(s) ignored",rejected);
}
static int (*audio_shutdown_wait)(struct desktop *);
static int theme_poll_async(uint64_t now)
{
    static uint64_t next;
    static char text[CFG_TEXT_MAX]; size_t n; int error;
    if (!settings_writer_started || settings_reload_pending) return 0;
    if (!theme_read_pending && now>=next && !cfg_writer_read(&settings_writer)) {
        theme_read_pending=1; next=now+2000000000ull;
    }
    if (!cfg_writer_read_result(&settings_writer,text,&n,&error)) return 0;
    theme_read_pending=0;
    if (error && error!=ENOENT) return 0;
    char name[32]="";
    if (!error && desk_settings_theme(text,n,name,sizeof(name))) name[0]=0;
    const struct desk_theme *before=desk_theme;
    if (!name[0] || desk_theme_use(name)) desk_theme_use(DESK_THEME_DEFAULT);
    return desk_theme!=before;
}
static int settings_save(struct desktop *d)
{
    /* A queued Control Panel reload owns the newest disk snapshot. Do not
     * overwrite it with the pre-reload theme/wallpaper while its read waits. */
    if (settings_reload_pending || (d->sh.want_reload&1)) { d->sh.want_save=1; return 0; }
    static char text[CFG_TEXT_MAX];
    size_t n=cfg_format(&d->sh.cfg,text,sizeof(text));
    if (!n) { errno=ENOSPC; return -1; }
    if (!settings_writer_started) { errno=EIO; return -1; }
    int result=cfg_writer_try_submit(&settings_writer,text,n);
    if (result && errno==EAGAIN) { d->sh.want_save=1; return 0; }
    return result;
}
static int shutdown_wait(struct desktop *d)
{
    if (wallpaper_worker_started && desk_wall_worker_busy(&wallpaper_worker)) return 0;
    if (settings_writer_started && cfg_writer_busy(&settings_writer)) return 0;
    return !audio_shutdown_wait || audio_shutdown_wait(d);
}
struct gfx_display_target { struct desktop *desktop; int surface; };
static uint64_t gfx_clock(void *ctx) { (void)ctx; return monotonic(); }
static int gfx_info(void *ctx, struct ciuki_accel_info *out)
{
    struct desktop *d=((struct gfx_display_target *)ctx)->desktop;
    return d->accel_info ? d->accel_info(d->display,d->accel_generation,out) : -ENOSYS;
}
static int gfx_accel(void *ctx, const struct ciuki_accel_request *q)
{
    struct desktop *d=((struct gfx_display_target *)ctx)->desktop;
    return d->accel ? d->accel(d->display,q) : -ENODEV;
}
static int gfx_upload(void *ctx, const struct ciuki_rect *r)
{
    struct gfx_display_target *t=ctx;
    return t->desktop->ops.present(t->desktop->display,t->surface,r) ? -errno : 0;
}
static int gfx_benchmark(struct desktop *d, unsigned index, uint64_t values[3])
{
    /* The composite remains authoritative. This separate surface is only the
     * software baseline; restore scanout on success and partial failure alike. */
    struct gfx_display_target ctx={d,-1};
    struct gfx_bench_target t={.ctx=&ctx,.width=d->width,.height=d->height,
        .bpp=d->lfb_bytes_per_pixel*8,.generation=d->accel_generation,
        .now=gfx_clock,.info=gfx_info,.accel=gfx_accel,.upload=gfx_upload};
    struct ciuki_surface_info info={0};
    if (index>=3) {
        ctx.surface=ciuki_surface_create(d->width,d->height,CIUKI_SURFACE_XRGB8888);
        if (ctx.surface<0) return -errno;
        if (ciuki_surface_info(ctx.surface,&info)) { int e=errno; close(ctx.surface); return -e; }
        t.pixels=ciuki_surface_map(ctx.surface,PROT_READ|PROT_WRITE);
        if (t.pixels==MAP_FAILED) { int e=errno; close(ctx.surface); return -e; }
        memcpy(t.pixels,d->pixels,(size_t)d->width*d->height*4);
    }
    struct gfx_bench_result result;
    int e=gfx_bench_sample(&t,index,&result);
    struct ciuki_rect full={0,0,0,0,d->width,d->height};
    if (d->ops.present(d->display,d->output,&full)) {
        e=-errno;
        desk_damage_add(&d->present_damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
    }
    d->scanout_valid=0; d->copy_pending=0;
    if (ctx.surface>=0) { munmap(t.pixels,info.allocation_bytes); close(ctx.surface); }
    if (!e) { values[0]=result.bytes; values[1]=result.ns; values[2]=result.operations; }
    return e;
}
static void performance_sample(struct desktop *d, uint64_t now)
{
    if (!d->sample_begin_ns) { d->sample_begin_ns=now; return; }
    if (now-d->sample_begin_ns<2000000000ull) return;
    desk_stats_sample(d); d->sample_ns=now-d->sample_begin_ns;
    d->sample_begin_ns=now;
    if (verbose_boot) {
        const struct desk_frame_stats *s=&d->sample;
        char line[CFG_LOG_MAX];
        snprintf(line,sizeof(line),"[display] frames=%llu damage_px=%llu presents=%llu lfb_bytes=%llu present_us=%llu composite_us=%llu input_us=%llu input_lag_us=%llu interval_us=%llu frame_p95_us=%llu frame_max_us=%llu completed=%llu overruns=%llu dropped=%llu\n",
                (unsigned long long)s->frames,(unsigned long long)s->damage_px,(unsigned long long)s->presents,
                (unsigned long long)s->lfb_bytes,(unsigned long long)(s->present_ns/1000),
                (unsigned long long)(s->composite_ns/1000),(unsigned long long)(s->input_ns/1000),
                (unsigned long long)(s->input_lag_ns/1000),(unsigned long long)(d->sample_ns/1000),
                (unsigned long long)(s->frame_p95_ns/1000),(unsigned long long)(s->frame_max_ns/1000),
                (unsigned long long)s->completed,(unsigned long long)s->overruns,(unsigned long long)s->dropped);
        if (d->gfx_sample.serial) {
            const struct desk_gfx_stats *g=&d->gfx_sample;
            size_t used=strlen(line);
            snprintf(line+used,sizeof(line)-used,
                "[display] gfx_serial=%u app_serial=%u render_us=%u submit_us=%u wait_us=%u window=%ux%u source=%ux%u viewport=%ux%u convert_us=%llu copy_us=%llu slot_us=%llu refresh_ns=%llu refresh_hz=60 assumed=1 clock_res_us=1000\n",
                g->serial,g->app_serial,g->render_us,g->submit_us,g->wait_us,g->width,g->height,
                g->source_w,g->source_h,g->view_w,g->view_h,(unsigned long long)(g->convert_ns/1000),
                (unsigned long long)(g->copy_ns/1000),(unsigned long long)(g->slot_ns/1000),
                (unsigned long long)DESK_FRAME_NS);
        }
        if (settings_writer_started) cfg_writer_log(&settings_writer,line);
    }
    d->gfx_sample.serial=0; /* no stale demo record after pause/close */
}
static int display_apply(struct desktop *d, uint16_t id)
{
    int e = ciuki_display_set(id);
    if (!e && id != d->display_mode) d->display_pending = id;
    return e;
}
/* Read the cheap notification counter on service turns; copy the inventory
 * only when it changes. Allocation failure keeps the old
 * surface private and lets the kernel watchdog restore its previous mode. */
static void display_poll(struct desktop *d)
{
    uint32_t counter;
    if (read(d->display, &counter, sizeof(counter)) != sizeof(counter)) return;
    if (counter==d->display_counter) return;
    struct ciuki_display_info info = {.version = CIUKI_DISPLAY_VERSION, .size = sizeof(info)};
    if (ciuki_display_query(&info)) return;
    uint16_t mode = 0;
    for (unsigned i = 0; i < info.mode_count && i < 32; i++) if (info.modes[i].current) mode = info.modes[i].id;
    if (counter == d->display_counter && mode == d->display_mode) return;
    if (!info.width || !info.height || info.width > 1600 || info.height > 1200) return;
    int output = ciuki_surface_create(info.width, info.height, CIUKI_SURFACE_XRGB8888);
    uint32_t *pixels = output < 0 ? MAP_FAILED : ciuki_surface_map(output, PROT_READ | PROT_WRITE);
    if (pixels == MAP_FAILED) { if (output >= 0) close(output); return; }
    munmap(d->pixels, ((size_t)d->width * d->height * 4 + CIUKI_PAGE_SIZE - 1) & ~(size_t)(CIUKI_PAGE_SIZE - 1));
    close(d->output);
    d->pixels = pixels; d->output = output;
    desk_display_relayout(d, info.width, info.height);
    struct ciuki_framebuffer_info fb;
    if (!ciuki_display_info(d->display,&fb)) desk_accel_reset(d,fb.generation);
    if (d->shell) wallpaper_update(d);
    d->display_counter = counter;
    d->display_mode = mode;
    d->lfb_bytes_per_pixel=(info.bpp+7)/8;
    /* Includes rollback: save that confirmed frame's mode, never the failed
     * candidate. The existing cfg retains its previous id until then. */
    d->display_pending = mode;
}
static void display_start(struct desktop *d)
{
    d->display_set = display_apply;
    struct ciuki_display_info info = {.version = CIUKI_DISPLAY_VERSION, .size = sizeof(info)};
    if (ciuki_display_query(&info)) return;
    for (unsigned i = 0; i < info.mode_count && i < 32; i++) if (info.modes[i].current) d->display_mode = info.modes[i].id;
    uint32_t counter;
    if (read(d->display, &counter, sizeof(counter)) == sizeof(counter)) d->display_counter = counter;
    uint16_t wanted = d->sh.cfg.display_mode;
    d->sh.cfg.display_mode = d->display_mode;
    if (d->sh.gate || !wanted || wanted == d->display_mode) return;
    for (unsigned i = 0; i < info.mode_count && i < 32; i++) if (info.modes[i].id == wanted) {
        int e = display_apply(d, wanted);
        if (e) desk_shell_notify_error(d,"Display mode unavailable",e);
        break;
    }
}

static void wallpaper_load(struct desktop *d)
{
    desk_image_free(&d->sh.image);
    memset(&wallpaper_request,0,sizeof(wallpaper_request));
    snprintf(wallpaper_request.path,sizeof(wallpaper_request.path),"%s",d->sh.cfg.wallpaper);
    wallpaper_request.width=d->width; wallpaper_request.height=d->height;
    wallpaper_request.top=wallpaper_request.bottom=DESK_BAR;
    wallpaper_request.mode=d->sh.cfg.wallpaper_mode;
    wallpaper_request.colour=d->sh.cfg.desktop_color==CFG_DESKTOP_COLOR ? desk_theme->desktop : d->sh.cfg.desktop_color;
    wallpaper_request.portrait=d->portrait;
    wallpaper_request.caption_font=desk_theme->font_large;
    wallpaper_request.caption_colour=desk_theme->desktop_label;
    wallpaper_request.caption_aa=desk_theme->text_aa;
    d->sh.wallpaper_pending=1;
    __atomic_store_n(&desktop_watchdog.work,1,__ATOMIC_RELEASE);
    /* Keep the last complete image visible while its replacement is prepared.
     * In particular a theme colour change must not erase the photograph. */
    if (d->sh.wallpaper_pending && d->sh.wall && d->sh.wall_w==d->width && d->sh.wall_h==d->height) {
        d->sh.wall_valid=1;
    } else {
        d->sh.wall_valid=0; desk_shell_wallpaper_update(d);
    }
    d->sh.image_failed=0;
    if (wallpaper_worker_failed) { d->sh.wallpaper_pending=0; d->sh.image_failed=1; return; }
    /* The thread may exist before the first frame; decoding never starts before it. */
    wallpaper_serial=0;
    if (wallpaper_worker_started && __atomic_load_n(&startup_us[3],__ATOMIC_ACQUIRE))
        wallpaper_serial=desk_wall_worker_submit(&wallpaper_worker,&wallpaper_request);
}
static void wallpaper_update(struct desktop *d)
{
    if (!d->shell) return;
    if (d->sh.gate) { desk_shell_wallpaper_update(d); return; }
    uint32_t colour=d->sh.cfg.desktop_color==CFG_DESKTOP_COLOR ? desk_theme->desktop : d->sh.cfg.desktop_color;
    if (strcmp(wallpaper_request.path,d->sh.cfg.wallpaper) || wallpaper_request.width!=d->width ||
        wallpaper_request.height!=d->height || wallpaper_request.mode!=d->sh.cfg.wallpaper_mode || wallpaper_request.colour!=colour ||
        (!strcmp(wallpaper_request.path,"ciuki") && (wallpaper_request.caption_font!=desk_theme->font_large ||
            wallpaper_request.caption_colour!=desk_theme->desktop_label || wallpaper_request.caption_aa!=desk_theme->text_aa))) {
        wallpaper_load(d); return;
    }
    if (!d->sh.wall_valid && d->sh.wall &&
        d->sh.wall_w==d->width && d->sh.wall_h==d->height) {
        /* A theme/icon-only invalidation retains the already rendered frame. */
        d->sh.wall_valid=1; d->sh.background_valid=0;
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    } else if (!d->sh.wall_valid) desk_shell_wallpaper_update(d);
}
/* Create the idle wallpaper thread during initialisation: the kernel's crash-isolation
 * probe compares the desktop's thread count across its cycles, and a thread born after the
 * first frame made the ledgers differ (processes_equal=0, f1-usb 2026-10-13). The thread
 * waits on its mailbox; requests are submitted only after the first frame. */
static void wallpaper_worker_prepare(struct desktop *d)
{
    if (!d->shell || d->sh.gate || wallpaper_worker_started || wallpaper_worker_failed) return;
    int e=desk_wall_worker_start(&wallpaper_worker,DESK_WALL_CACHE,startup_stage);
    if (e) { wallpaper_worker_failed=1; desk_shell_notify_error(d,"Wallpaper worker unavailable",e); return; }
    wallpaper_worker_started=1;
}
static void wallpaper_poll(struct desktop *d)
{
    if (!d->shell || d->sh.gate || d->power.state || !__atomic_load_n(&startup_us[3],__ATOMIC_ACQUIRE)) return;
    wallpaper_update(d);
    if (!d->sh.wallpaper_pending) return;
    if (!wallpaper_worker_started && !wallpaper_worker_failed) {
        int e=desk_wall_worker_start(&wallpaper_worker,DESK_WALL_CACHE,startup_stage);
        if (e) {
            wallpaper_worker_failed=1; d->sh.wallpaper_pending=0;
            desk_shell_notify_error(d,"Wallpaper worker unavailable",e); return;
        }
        wallpaper_worker_started=1;
    }
    if (wallpaper_worker_started && !wallpaper_serial) {
        desktop_wait("wallpaper-submit");
        wallpaper_serial=desk_wall_worker_submit(&wallpaper_worker,&wallpaper_request);
    }
    uint32_t *pixels=NULL; int error=0, hit=0;
    desktop_wait("wallpaper-take");
    if (!wallpaper_worker_started || !desk_wall_worker_take(&wallpaper_worker,wallpaper_serial,&pixels,&error,&hit)) return;
    (void)hit;
    d->sh.wallpaper_pending=0; d->sh.image_failed=error!=0;
    int changed=1;
    if (!error) {
        /* Colour-only changes to an opaque photograph often produce the
         * identical work area. Chrome has already repainted for the theme;
         * do not upload that same photograph a second time. */
        desktop_wait("wallpaper-compare");
        if (d->scanout_valid && d->sh.wall_valid && d->sh.background_valid && d->sh.wall &&
            d->sh.wall_w==d->width && d->sh.wall_h==d->height && d->height>2*DESK_BAR)
            changed=memcmp(d->sh.wall+(size_t)DESK_BAR*d->width,pixels+(size_t)DESK_BAR*d->width,
                           (size_t)d->width*(d->height-2*DESK_BAR)*4)!=0;
        desktop_wait("wallpaper-publish");
        free(d->sh.wall); d->sh.wall=pixels; d->sh.wall_w=d->width; d->sh.wall_h=d->height;
        d->sh.wall_valid=1;
        if (changed) d->sh.background_valid=0;
        startup_stage("wallpaper_applied");
    } else desk_shell_notify_error(d,"Wallpaper cannot be shown",error);
    if (changed) desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
}
static void network_read(struct desktop *d)
{
    int fd = open("/dev/net", O_RDONLY | O_CLOEXEC);
    struct ciuki_net_state state = { .version = CIUKI_NET_VERSION, .size = sizeof(state) };
    if (fd >= 0) {
        if (ciuki_devctl(fd, NET_GET_STATE, &state, sizeof(state)) < 0)
            state = (struct ciuki_net_state){ .version = CIUKI_NET_VERSION, .size = sizeof(state) };
        close(fd);
    }
    desk_shell_network_update(d, &state);
}
static int autoplay_preference(struct desktop *d,const char *id,const char *label,int action)
{
    (void)d;
    return action<0 ? ap_preference_get(NULL,id) : ap_preference_set(NULL,id,label,action);
}
static void media_sound(struct desktop *d,int connected)
{
    const char *key=connected ? "device_connect" : "device_disconnect";
    for (unsigned i=0;i<SOUND_EVENTS;i++) if (!strcmp(sound_keys[i],key)) { desk_sound(d,i); break; }
}
static int media_read(struct desktop *d)
{
    struct ciuki_media_state s={.version=CIUKI_MEDIA_VERSION,.size=sizeof(s)};
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC);
    int observed=0;
    if (fd>=0) {
        observed=!ciuki_devctl(fd,MEDIA_GET_STATE,&s,sizeof(s));
        close(fd);
    }
    if (!observed) return 0; /* An unavailable observer is not a physical unplug. */
    desk_shell_media_update(d,&s);
    /* A failed observation preserves prior state, never fabricates an eject. */
    static struct ciuki_media_volume volumes[26];
    int observe=open("/dev/media",O_RDONLY|O_CLOEXEC);
    if (observe>=0) {
        int ok=1;
        for (unsigned i=3;i<26;i++) {
            struct ciuki_media_volume v={.version=CIUKI_MEDIA_VERSION,.size=sizeof(v),.drive=i};
            if (ciuki_devctl(observe,MEDIA_GET_VOLUME,&v,sizeof(v))) { ok=0; break; }
            volumes[i]=v;
        }
        close(observe);
        if (ok) { desk_shell_media_labels(d,volumes); desk_autoplay_update(d,volumes); }
        return ok;
    }
    return 0;
}
static void media_remove(struct desktop *d)
{
    struct ciuki_media_remove r={CIUKI_MEDIA_VERSION,sizeof(r),(uint32_t)d->sh.want_remove,d->sh.remove_generation};
    int optical=r.drive<26 && d->sh.media.type[r.drive]==CIUKI_MEDIA_OPTICAL;
    d->sh.want_remove=0;
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC), e=fd<0 ? errno : 0;
    struct ciuki_media_blockers blockers={.version=CIUKI_MEDIA_VERSION,.size=sizeof(blockers),.drive=r.drive,.generation=r.generation};
    char reason[320]; desk_media_blocker_text(reason,sizeof(reason),NULL,desk_media_notice_body(d->width,d->height));
    if (fd>=0) {
        if (ciuki_devctl(fd,MEDIA_SAFE_REMOVE,&r,sizeof(r))) e=errno;
        if (e==EBUSY && !ciuki_devctl(fd,MEDIA_GET_BLOCKERS,&blockers,sizeof(blockers)))
            desk_media_blocker_text(reason,sizeof(reason),&blockers,desk_media_notice_body(d->width,d->height));
        close(fd);
    }
    d->now_ns=monotonic();
    media_read(d);
    d->now_ns=monotonic();
    if (e==EBUSY) desk_shell_notify(d,"Drive is in use",reason,DU_DLG_ICON_WARNING,SH_NOTIFY_DETAILS);
    else if (e) desk_shell_notify_error(d,"Media removal failed",e);
    else desk_shell_notify(d,"Removable media",optical ? "The disc has been ejected." : "It is safe to unplug the USB device.",DU_DLG_ICON_INFO,SH_NOTIFY_MEDIA);
}
static void mixer_read(struct desktop *d)
{
    int old_volume=d->sh.volume, old_muted=d->sh.muted, old_state=d->sh.mixer_state;
    int fd=d->mixer_fd, own=0;
    if (fd<0) { fd=open("/dev/mixer",O_RDONLY|O_CLOEXEC); own=1; }   /* public, read-only */
    struct ciuki_mixer_master m={.version=CIUKI_AUDIO_VERSION,.size=sizeof(m)};
    if (fd<0 || ciuki_devctl(fd,MIXER_GET_MASTER,&m,sizeof(m))<0) d->sh.mixer_state=-1;
    else {
        d->sh.mixer_state=d->mixer_fd>=0 ? 2 : 1;
        if (m.save_state!=CIUKI_MIXER_UNSAVED || m.save_error) last_ec_preference=-1;
        if (!d->sh.volume_drag && !d->sh.want_mixer) {
            d->sh.volume=(int)((m.left+m.right)/2); d->sh.muted=m.mute!=0;
            int32_t packed=ciuki_master_pack(m.left,m.right,m.mute);
            if (m.save_state==CIUKI_MIXER_UNSAVED && !m.save_error && packed!=last_ec_preference &&
                settings_writer_started && !cfg_writer_busy(&settings_writer) && !d->power.state) {
                struct mixer_job job={.serial=++mixer_serial,.fd=fd,.master=m,.save_only=1};
                if (!cfg_writer_job(&settings_writer,&job,sizeof(job))) last_ec_preference=packed;
            }
        }
    }
    if (own && fd>=0) close(fd);
    if (old_volume!=d->sh.volume || old_muted!=d->sh.muted || old_state!=d->sh.mixer_state) {
        if (old_state>0 && d->sh.mixer_state>0 && (old_volume!=d->sh.volume || old_muted!=d->sh.muted)) {
            char text[64]; snprintf(text,sizeof(text),"Volume: %d%%%s",d->sh.volume,d->sh.muted ? " (muted)" : "");
            desk_shell_notify(d,"Volume",text,DU_DLG_ICON_INFO,SH_NOTIFY_VOLUME);
        }
        desk_shell_volume_damage(d);
    }
}
static void mixer_write(struct desktop *d)
{
    if (d->mixer_fd<0) { mixer_read(d); return; }
    int e=mixer_apply(d,(uint32_t)d->sh.volume,(uint32_t)d->sh.volume,(uint32_t)(d->sh.muted!=0));
    if (e==-EAGAIN) { d->sh.want_mixer=1; return; }
    if (e) {
        desk_shell_notify_error(d,"Volume change failed",e);
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,DESK_BAR},d->width,d->height);
    }
}
static void mixer_poll(struct desktop *d,uint64_t now,uint64_t *next)
{
    if (d->shell && !d->sh.gate && !d->power.state && now>=*next) {
        *next=now+250000000ull;
        d->sh.want_reload|=4;
    }
}
static void system_read(struct desktop *d)
{
    static uint8_t buffer[CIUKI_PROC_LIST_MIN+CIUKI_PROCESS_MAX*sizeof(struct ciuki_proc_record)];
    int n=ciuki_proc_list(buffer,sizeof(buffer));
    d->sh.system_valid=0;
    if (n>0) {
        struct ciuki_proc_system sys;
        memcpy(&sys,buffer,sizeof(sys));
        if (sys.version==CIUKI_PROC_VERSION && sys.tick_hz) {
            d->sh.mem_total_k=(uint64_t)sys.usable_pages*4u; d->sh.mem_free_k=(uint64_t)sys.free_pages*4u;
            d->sh.processes=sys.processes; d->sh.uptime_s=(uint32_t)(sys.total_ticks/sys.tick_hz);
            d->sh.system_valid=1;
        }
    }
    struct utsname u;
    if (!uname(&u)) snprintf(d->sh.cpu_name,sizeof(d->sh.cpu_name),"%.40s",u.machine);
}
static void storage_poll(struct desktop *d, uint64_t now)
{
#ifdef __i386__
    static uint64_t next;
    static char reported[26][80];
    static struct ciuki_devices_info info;
    if (d->sh.gate || now<next) return;
    next=now+2000000000ull;
    info.version=CIUKI_DEVICES_VERSION; info.size=sizeof(info);
    if (ciuki_devices(&info)) return;
    uint32_t present=0;
    for (unsigned i=0;i<info.count && i<CIUKI_DEVICES_MAX;i++) {
        const struct ciuki_device_record *v=&info.devices[i];
        if (v->id>=0x41002 && v->id<0x4101a && !strcmp(v->driver,"fat"))
            present|=1u<<(v->id-0x41000);
    }
    for (unsigned drive=2;drive<26;drive++) if (!(present&(1u<<drive))) reported[drive][0]=0;
    for (unsigned i=0;i<info.count && i<CIUKI_DEVICES_MAX;i++) {
        const struct ciuki_device_record *v=&info.devices[i];
        if (v->id<0x41002 || v->id>=0x4101a || strcmp(v->driver,"fat")) continue;
        unsigned drive=v->id-0x41000;
        if (!strcmp(reported[drive],v->reason)) continue;
        snprintf(reported[drive],sizeof(reported[drive]),"%s",v->reason);
        char text[200];
        snprintf(text,sizeof(text),"%s\n%s\nChanges cannot be saved to this volume.",v->name,v->reason);
        desk_shell_notify(d,"Storage is read-only",text,DU_DLG_ICON_ERROR,SH_NOTIFY_DETAILS);
        break;
    }
#else
    (void)d; (void)now;
#endif
}
static int run_command(struct desktop *d, char *line)
{
    /* "name arg..." or "/Programs/name arg...": spaces separate, double quotes group. */
    char *words[8]; unsigned count=0;
    for (char *p=line; *p && count<8;) {
        while (*p==' ') p++;
        if (!*p) break;
        if (*p=='"') { words[count++]=++p; while (*p && *p!='"') p++; }
        else { words[count++]=p; while (*p && *p!=' ') p++; }
        if (*p) *p++=0;
    }
    if (!count) return 0;
    const char *name=!strncmp(words[0],"/Programs/",sizeof("/Programs/")-1) ? words[0]+sizeof("/Programs/")-1 : words[0];
    for (unsigned k=0; k<d->program_count; k++) if (!strcmp(d->programs[k].name,name)) {
        for (unsigned a=0; a<6; a++) d->launch_argv[a]=a+1<count ? words[a+1] : NULL;
        int pid=d->launch ? d->launch(d,&d->programs[k]) : -1;
        int error=errno;
        for (unsigned a=0; a<6; a++) d->launch_argv[a]=NULL;
        if (pid<0) {
            char text[160]; snprintf(text,sizeof(text),"'%s' could not be started.\n%s (%d)",name,strerror(error),error);
            desk_shell_message(d,"Run",text,1);
        } else snprintf(d->notice,sizeof(d->notice),"Started %.32s (PID %d)",name,pid);
        return pid;
    }
    char text[200];
    snprintf(text,sizeof(text),"CiukiOS cannot find '%.60s'.\nType the name of a program in /Programs.",words[0]);
    desk_shell_message(d,"Run",text,1);
    return -1;
}
static int power_request(int action)
{ return ciuki_power(action==1 ? CIUKI_POWER_REBOOT : CIUKI_POWER_OFF); }
static int power_alive(struct desktop *d, uint32_t pid)
{
    /* One syscall snapshot per loop, shared by all targets. Disconnecting
     * the desktop channel does not imply exit/VM teardown. */
    static uint8_t buffer[CIUKI_PROC_LIST_MIN+CIUKI_PROCESS_MAX*sizeof(struct ciuki_proc_record)];
    static uint64_t sampled=UINT64_MAX;
    static int count=-1;
    uint64_t now=d->now_ns;
    if (sampled!=now) { sampled=now; count=ciuki_proc_list(buffer,sizeof(buffer)); }
    if (count<0 || count>CIUKI_PROCESS_MAX) return -1;
    struct ciuki_proc_system sys;
    memcpy(&sys,buffer,sizeof(sys));
    if (sys.version!=CIUKI_PROC_VERSION || sys.records!=(uint32_t)count) return -1;
    for (int i=0; i<count; i++) {
        struct ciuki_proc_record r;
        memcpy(&r,buffer+CIUKI_PROC_LIST_MIN+(size_t)i*sizeof(r),sizeof(r));
        if (r.pid==pid) return r.state!=CIUKI_PROC_ZOMBIE;
    }
    return 0;
}
static void power_prepare(struct desktop *d)
{
    if (d->mixer_fd>=0) { close(d->mixer_fd); d->mixer_fd=-1; d->sh.mixer_state=1; }
    /* Settings, wallpaper and icon readers use scoped FILE/DIR handles.
     * Also release inherited file-backed logs, preserving input, display,
     * output, IPC and the termination authority for retry/Cancel. */
    fflush(NULL);
    for (int fd=0; fd<CIUKI_OPEN_MAX; fd++) {
        if (fd==d->input || fd==d->display || fd==d->output || fd==d->authority_fd) continue;
        int ipc=0;
        for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].channel==fd || d->clients[i].surface==fd) ipc=1;
        if (ipc) continue;
        struct stat st;
        if (!fstat(fd,&st) && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) close(fd);
    }
}
/* Serves the requests that shell.c raised while handling input. */
static void shell_service(struct desktop *d)
{
    struct desk_shell *sh=&d->sh;
    if (!d->shell) return;
    desktop_wait("mixer-result");
    struct mixer_job job; size_t job_size; int job_error;
    if (settings_writer_started && cfg_writer_job_result(&settings_writer,&job,&job_size,&job_error)) {
        if (job_size!=sizeof(job)) job_error=EIO;
        if (!job_error && job.master.save_state==CIUKI_MIXER_SAVED)
            last_ec_preference=ciuki_master_pack(job.master.left,job.master.right,job.master.mute);
        if (job_error) desk_shell_notify_error(d,"Volume change failed",job_error);
        else if (job.master.save_state==CIUKI_MIXER_UNSAVED)
            desk_shell_notify_error(d,"Volume applied but not saved",(int)job.master.save_error);
        if (job_error || job.master.save_state==CIUKI_MIXER_UNSAVED)
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,DESK_BAR},d->width,d->height);
        if (!job_error && !job.save_only) desk_sound(d,SOUND_VOLUME);
        if (job.serial == mixer_serial && !sh->volume_drag && !sh->want_mixer) sh->want_reload|=4;
    }
    int save_error;
    desktop_wait("settings-save-result");
    if (settings_writer_started && cfg_writer_result(&settings_writer,&save_error) && save_error) {
        desk_shell_notify_error(d,"Settings not saved",save_error);
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,DESK_BAR},d->width,d->height);
    }
    if (sh->gate) { sh->want_save=sh->want_mixer=sh->want_power=0; sh->want_reload&=~1; sh->want_run[0]=0; }
    if (d->power.state) { desk_power_tick(d); return; }
    desktop_wait("folder-service"); desk_folder_service(d);
    if (sh->want_power) {
        int action=sh->want_power; sh->want_power=0;
        if (settings_save(d)) { /* still try: settings are a convenience */ }
        desk_power_begin(d,action);
        return;
    }
    if (sh->want_reload & 1) {
        settings_reload_pending=settings_reload_request=1;
        theme_read_pending=0;
        settings_reload_wallpaper|=sh->want_wallpaper;
        sh->want_wallpaper=0;
    }
    if (settings_reload_pending && !settings_writer_started) {
        settings_reload_pending=settings_reload_request=settings_reload_wallpaper=0;
        snprintf(d->notice,sizeof(d->notice),"Settings worker unavailable");
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,DESK_BAR},d->width,d->height);
    }
    desktop_wait("settings-read-submit");
    if (settings_reload_request && settings_writer_started && !cfg_writer_read(&settings_writer))
        settings_reload_request=0;
    static char reload_text[CFG_TEXT_MAX]; size_t reload_size; int reload_error;
    desktop_wait("settings-read-result");
    if (settings_reload_pending && !settings_reload_request && settings_writer_started &&
        cfg_writer_read_result(&settings_writer,reload_text,&reload_size,&reload_error)) {
        settings_reload_pending=0;
        if (reload_error && reload_error!=ENOENT) {
            desk_shell_notify_error(d,"Settings not loaded",reload_error);
            settings_reload_wallpaper=0;
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,DESK_BAR},d->width,d->height);
        } else {
            int old_tray_width = desk_shell_tray_box(d).w;
            char old_wall[CFG_PATH_MAX], old_theme[sizeof(sh->cfg.theme)];
            memcpy(old_wall,sh->cfg.wallpaper,sizeof(old_wall)); memcpy(old_theme,sh->cfg.theme,sizeof(old_theme));
            int old_mode=sh->cfg.wallpaper_mode, old_icons=sh->cfg.desktop_icons;
            uint32_t old_colour=sh->cfg.desktop_color;
            uint16_t confirmed = sh->cfg.display_mode;
            cfg_defaults(&sh->cfg);
            int rejected=reload_error ? 0 : cfg_parse(&sh->cfg,reload_text,reload_size);
            if (rejected) snprintf(d->notice,sizeof(d->notice),"desktop.cfg: %d line(s) ignored",rejected);
            sh->cfg.display_mode = confirmed;
            /* Explicit desktop Refresh also updates the launch inventory;
             * Control Panel display settings must not rescan its ELF files. */
            if (settings_reload_wallpaper) { desktop_wait("program-scan"); scan_programs(d); }
            int image_changed=strcmp(old_wall,sh->cfg.wallpaper) || settings_reload_wallpaper;
            int visual=image_changed || strcmp(old_theme,sh->cfg.theme) || old_mode!=sh->cfg.wallpaper_mode ||
                       old_icons!=sh->cfg.desktop_icons || old_colour!=sh->cfg.desktop_color;
            if (visual) {
                /* A wallpaper-only request leaves the retained desktop
                 * untouched until its complete replacement is ready. */
                if (strcmp(old_theme,sh->cfg.theme) || old_icons!=sh->cfg.desktop_icons || settings_reload_wallpaper) {
                    desktop_wait("theme-apply"); sh->icon_count=0; desk_shell_apply(d);
                }
                if (image_changed) wallpaper_load(d); else wallpaper_update(d);
            }
            desk_shell_tray_damage(d,old_tray_width);
            settings_reload_wallpaper=0;
        }
    }
    if (sh->want_reload & 2) { desktop_wait("system-read"); system_read(d); if (d->about>=0) desk_damage_add(&d->damage,d->clients[d->about].frame,d->width,d->height); }
    if (sh->want_reload & 4) { desktop_wait("mixer-read"); mixer_read(d); }
    if (sh->want_reload & 16) { desktop_wait("network-read"); network_read(d); }
    if (sh->want_reload & 32) { desktop_wait("media-read"); media_read(d); }
    if (sh->want_remove) { desktop_wait("media-remove"); media_remove(d); }
    desktop_wait("autoplay-check");
    if (sh->autoplay.want_commit && !media_read(d)) {
        sh->autoplay.want_commit=0;
        desk_shell_message(d,"AutoPlay","The media could not be checked. Please try again.",1);
    }
    desktop_wait("autoplay-service"); desk_autoplay_service(d);
    sh->want_reload=0;
    wallpaper_poll(d);
    if (!sh->wall_valid) wallpaper_update(d);
    if (sh->want_save) {
        desktop_wait("settings-save-submit");
        sh->want_save=0;
        if (settings_save(d)) {
            snprintf(d->notice,sizeof(d->notice),"Settings not saved (errno %d)",errno);
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,DESK_BAR},d->width,d->height);
        }
    }
    if (sh->want_mixer) { desktop_wait("mixer-submit"); sh->want_mixer=0; mixer_write(d); }
    if (sh->want_run[0]) {
        char line[sizeof(sh->want_run)];
        memcpy(line,sh->want_run,sizeof(line)); sh->want_run[0]=0;
        desktop_wait("run-command"); run_command(d,line);
    }
    desktop_wait("shell-done");
}
static void shell_setup(struct desktop *d, int gate)
{
    d->launch=launch_program; d->terminate=terminate_child; d->icon=store_icon; d->mixer_set=mixer_apply;
    d->autoplay_fs=fm_native_vfs(); d->autoplay_preference=autoplay_preference; d->media_sound=media_sound;
    d->self_pid=(uint32_t)getpid();
    const char *authority=getenv(CIUKI_TASKS_ENV);
    int fd=authority ? number(authority,CIUKI_OPEN_MAX-1) : -1;
    d->authority_fd=fd>=3 && fcntl(fd,F_GETFD)>=0 ? fd : -1;
    terminate_authority=d->authority_fd;
    d->power_request=power_request; d->power_prepare=power_prepare; d->power_alive=power_alive;
    const char *mixer=getenv("CIUKI_MIXER_FD");
    fd=mixer ? number(mixer,CIUKI_OPEN_MAX-1) : -1;
    d->mixer_fd=fd>=3 && fcntl(fd,F_GETFD)>=0 ? fd : -1;
    d->mixer_set=mixer_apply;
    struct utsname u;
    strcpy(d->build_identity,"unavailable");
    d->version[0]=0;
    if (!uname(&u)) {
        snprintf(d->build_identity,sizeof(d->build_identity),"%.64s",u.release);
        snprintf(d->version,sizeof(d->version),"%.64s",u.version);
#ifdef __i386__
        d->realtime_source=(int)u.realtime_source;
#endif
    }
    scan_programs(d);
    /* Runtime gate scenarios keep the built-in defaults and never touch desktop.cfg. */
    d->sh.gate=gate;
    if (!gate) settings_load(d);
    if (!gate) startup_stage("settings_read");
    struct stat pictures;
    d->pictures_exists = !stat("/Documents/Pictures", &pictures) && S_ISDIR(pictures.st_mode);
    desk_shell_enable(d);
    if (!gate) desk_sound_init(d);
    if (!gate) {
        int result=desk_folder_init(d,fm_native_vfs());
        if (result) desk_shell_notify_error(d,"Desktop folder unavailable",result);
        settings_writer.run_job=mixer_job_run;
        settings_writer_started=!cfg_writer_start(&settings_writer,CFG_FILE,CFG_DIR "/desktop.new");
        audio_shutdown_wait=d->sound_shutdown; d->sound_shutdown=shutdown_wait;
    }
    if (!gate) { startup_stage("fonts_icons_ready"); mixer_read(d); system_read(d); wallpaper_load(d); wallpaper_worker_prepare(d); }
    else desk_shell_wallpaper_update(d);
    if (!gate) desktop_watchdog_start(d);
}
int main(int argc, char **argv)
{
    startup_begin=monotonic(); startup_stage("entry");
#ifdef __i386__
    static struct ciuki_devices_info devices;
    devices.version=CIUKI_DEVICES_VERSION; devices.size=sizeof(devices);
    if (!ciuki_devices(&devices)) verbose_boot=!!(devices.boot_flags&(1u<<11)); /* CBI_F_VERBOSE */
#endif
    int display = 3, input = 4, test = 0, gate = 0, cycles = 1, demo_count = 0, fullscreen_test = 0;
    int channels[DESK_CLIENTS], channel_count = 0;
    const char *demos[DESK_CLIENTS], *victim = NULL, *launch = NULL;
    for (int a = 1; a < argc; a++) if (!strcmp(argv[a],"--test=crash-isolation")) test = 1;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a],"--test=crash-isolation")) continue;
        if (!strcmp(argv[a],"--test=vm-fullscreen")) { fullscreen_test=1; continue; }
        if (test && !strcmp(argv[a],"--gate")) gate=1;
        else if (!strncmp(argv[a],"--display-fd=",13)) display = number(argv[a]+13,63);
        else if (!strncmp(argv[a],"--input-fd=",11)) input = number(argv[a]+11,63);
        else if (!strncmp(argv[a],"--channel=",10) && channel_count < DESK_CLIENTS) {
            int fd = number(argv[a]+10,63);
            if (fd < 3) return 2;
            for (int i=0; i<channel_count; i++) if (channels[i] == fd) return 2;
            channels[channel_count++] = fd;
        } else if (test && !strncmp(argv[a],"--demo=",7) && demo_count < DESK_CLIENTS && fault_kind(argv[a]+7))
            demos[demo_count++] = argv[a]+7;
        else if (test && !strncmp(argv[a],"--victim=",9) && fault_kind(argv[a]+9) && strcmp(argv[a]+9,"none"))
            victim = argv[a]+9;
        else if (test && !strncmp(argv[a],"--cycles=",9)) cycles = number(argv[a]+9,100);
        else if (!strncmp(argv[a],"--launch=",9) && argv[a][9] && !launch) launch = argv[a]+9;
        else return 2;
    }
    if (display < 3 || input < 3 || display == input || cycles < 1 ||
        channel_count+demo_count+(victim != NULL) > DESK_CLIENTS) return 2;
    for (int i=0; i<channel_count; i++) if (channels[i]==display || channels[i]==input) return 2;
    /* Kernel channel_send posts SIGPIPE as well as returning EPIPE. */
    if (desk_ignore_sigpipe()) return gate_setup_error(gate,GATE_SETUP_SIGPIPE,errno);
    struct ciuki_framebuffer_info info = {.size=sizeof(info)};
    if (ciuki_display_info(display,&info)) {
        fprintf(stderr,"desktop: display unavailable (errno=%d); console ready\n",errno);
        return gate_setup_error(gate,GATE_SETUP_DISPLAY,errno); /* no-LFB profiles use the stand-in */
    }
    if (!info.width || !info.height || info.width>2048 || info.height>2048) return gate_setup_error(gate,GATE_SETUP_GEOMETRY,EINVAL);
    struct desktop *d = calloc(1,sizeof(*d));
    uint32_t *portrait = malloc(DESK_PORTRAIT_SIZE*DESK_PORTRAIT_SIZE*4);
    if (!d || !portrait) { free(d); free(portrait); return gate_setup_error(gate,GATE_SETUP_MEMORY,ENOMEM); }
    const struct desk_ops ops = {ciuki_channel_send,ciuki_surface_info,ciuki_surface_map,munmap,close,ciuki_present};
    desk_init(d,&ops,info.width,info.height); d->display=display; d->input=input;
    d->now_ns=monotonic(); /* startup notifications use the same clock as service turns */
    d->time_ns=monotonic; d->lfb_bytes_per_pixel=(info.bpp+7)/8;
    d->accel=ciuki_display_accel;
    d->accel_info=ciuki_display_accel_info;
    d->gfx_benchmark=gfx_benchmark;
    if (desk_load_portrait(portrait)) { fprintf(stderr,"desktop: approved Ciuki asset unavailable\n"); free(portrait); free(d); return gate_setup_error(gate,GATE_SETUP_PORTRAIT,EIO); }
    d->portrait=portrait;
    d->output=ciuki_surface_create(info.width,info.height,CIUKI_SURFACE_XRGB8888);
    d->pixels=d->output < 0 ? MAP_FAILED : ciuki_surface_map(d->output,PROT_READ|PROT_WRITE);
    if (d->pixels==MAP_FAILED) { int error=errno; if (d->output>=0) close(d->output); free(portrait); free(d); return gate_setup_error(gate,GATE_SETUP_OUTPUT,error); }
    desk_accel_reset(d,info.generation);
    int flags = fcntl(input,F_GETFL);
    if (flags < 0 || fcntl(input,F_SETFL,flags|O_NONBLOCK)) return gate_setup_error(gate,GATE_SETUP_INPUT,errno);
    shell_setup(d,gate);
    if (fullscreen_test) d->realtime_second=12*3600;
    display_start(d);
    /* f3-02: --launch=<name> starts one /Programs menu program through the same
     * path as a menu click (the f3:vm-window probe opens /Programs/dosbox so). */
    if (launch && !gate) {
        d->action=-2;
        for (unsigned i=0; i<d->program_count; i++) if (!strcmp(d->programs[i].name,launch)) d->action=(int)i;
        if (d->action<0) { d->action=-1; snprintf(d->notice,sizeof(d->notice),"%.40s is not in /Programs",launch); }
    }
    /* The ABI has no timed input read or poll/select. A bounded nonblocking
     * drain followed by an interruptible 10 ms sleep keeps IPC/time moving. */
    for (int i=0; i<channel_count; i++) desk_add_client(d,channels[i]);
    for (int i=0; i<demo_count; i++) if (spawn_demo(d,demos[i],0) < 0) fprintf(stderr,"desktop: demo spawn failed\n");
    struct gate_control *control = NULL;
    int survivor_pid=0, gate_victim=0, gate_cycle=0, interaction=0;
    unsigned generation=0;
    if (gate) {
        control=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (control==MAP_FAILED) return gate_setup_error(gate,GATE_SETUP_CONTROL,errno);
        if ((survivor_pid=spawn_demo(d,"none",1))<0) return gate_setup_error(gate,GATE_SETUP_SURVIVOR,errno);
        gate_report(d,CU_PTR(control),0,survivor_pid,0,0);
    }
    int victim_pid = -1, completed = 0;
    uint64_t next_victim = 0;
    int input_reported = 0, display_reported = 0;
    int report=0, input_dirty=0, shell_stage_shown=0, scenario_key=0, scenario_released=0;
    unsigned report_send=0;
    uint64_t report_deadline=0, next_input_report=0;
    unsigned reported_progress=0;
    int holding=0, shifted=0, startup_sound=0;
    uint64_t shift_base=0, shift_release=0, mixer_poll_next=0;
    for (;;) {
        uint64_t now = monotonic();
        desktop_stage(STALL_SHELL);
        desktop_wait("mixer-poll");
        mixer_poll(d,now,&mixer_poll_next);
        if (!gate) { desktop_wait("storage-poll"); storage_poll(d,now); }
        /* Test pacing suppresses unsolicited heartbeat PINGs only. */
        if (gate) for (int i=0; i<DESK_CLIENTS; i++) d->clients[i].next_ping_ns=UINT64_MAX;
        desktop_wait("heartbeat"); desk_heartbeat(d,now);
        if (!gate) { desktop_wait("sound-results"); desk_sound_results(d); }
        if (d->shell) {
            struct timespec rtc;
            if (!fullscreen_test && !clock_gettime(CLOCK_REALTIME,&rtc) && rtc.tv_sec>=0 && d->realtime_second!=(uint64_t)rtc.tv_sec) {
                uint64_t previous=d->realtime_second;
                d->realtime_second=(uint64_t)rtc.tv_sec;
                desk_shell_clock_damage(d,previous);
            }
            if (!d->power.state) { desktop_wait("shell-tick"); desk_shell_tick(d); }
        }
        if (gate && control->command) {
            unsigned command=control->command; generation=control->generation;
            control->command=0;
            report_send=0;
            report_deadline=now+GATE_BARRIER_NS;
            switch (command) {
            case GATE_SPAWN:
                if (gate_victim || gate_cycle>=100) return gate_error("spawn_state",EINVAL,generation);
                gate_victim=spawn_demo(d,gate_faults[gate_cycle%5],1);
                if (gate_victim<0) return gate_error("spawn_victim",errno,generation);
                /* Register the PID before any CONFIGURE send can schedule
                 * the child and let its armed report reach the controller. */
                gate_report(d,CU_PTR(control),generation,survivor_pid,gate_victim,(unsigned)gate_cycle);
                report=1; break;
            case GATE_RELEASE: report_send=GATE_RELEASE; report=1; break;
            case GATE_REAP: {
                int status;
                int reaped=waitpid(gate_victim,&status,WNOHANG);
                if (reaped!=gate_victim) return gate_error("reap_victim",reaped<0 ? errno : EAGAIN,generation);
                for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].pid==(uint32_t)gate_victim) desk_drop_client(d,i);
                gate_victim=0; gate_cycle++; report=1; break;
            }
            case GATE_RUN: report_send=GATE_RUN; report=1; break;
            case GATE_SNAPSHOT: report_send=GATE_SNAPSHOT; report=1; break;
            case GATE_INTERACT: interaction=1; report=1; break;
            default: return gate_error("command",EINVAL,generation);
            }
        }
        uint64_t input_begin=monotonic();
        desktop_stage(STALL_INPUT);
        if (!gate) {
            int n=desk_drain_input(d,ciuki_input_read);
            if (n>0) input_reported=0;
            else if (n<0 && !input_reported) {
                fprintf(stderr,"desktop: input unavailable (errno=%d)\n",errno); input_reported=1;
                struct ciuki_input_event resync={.type=CIUKI_INPUT_RESYNC}; desk_input(d,&resync);
            }
        }
        while (gate && fifo_count+CIUKI_INPUT_READ_MAX<=GATE_INPUT_FIFO) {
            struct ciuki_input_event events[CIUKI_INPUT_READ_MAX];
            int n=ciuki_input_read(input,events,CIUKI_INPUT_READ_MAX);
            for (int i=0; i<n; i++) input_fifo[(fifo_head+fifo_count++)%GATE_INPUT_FIFO]=events[i];
            if (n>0) input_reported=0;
            else if (n<0 && errno!=EAGAIN && errno!=EINTR && !input_reported) {
                fprintf(stderr,"desktop: input unavailable (errno=%d)\n",errno); input_reported=1;
                struct ciuki_input_event resync={.type=CIUKI_INPUT_RESYNC}; desk_input(d,&resync);
            }
            if (n<=0) break;
        }
        int scenario=gate && (d->shell_test || d->tasks_test || d->app_test);
        int hold=scenario && scenario_hold(d,now);
        if (hold) holding=1;
        else if (holding) {
            holding=0;
            if (fifo_count) { shift_base=input_fifo[fifo_head].monotonic_ns; shift_release=now; shifted=1; }
        }
        while (fifo_count && !holding) {
            /* A launch starts its hold before the input that follows the launching click. */
            if (gate && (d->shell_test || d->tasks_test || d->app_test) && d->action>=0) break;
            struct ciuki_input_event ev=input_fifo[fifo_head];
            if (shifted && (int64_t)(now-shift_release)<(int64_t)(ev.monotonic_ns-shift_base)) break;
            fifo_head=(fifo_head+1)%GATE_INPUT_FIFO; fifo_count--;
            /* Runtime scenario selection. Old gates never send these keys: one of them, pressed
             * once inside the controller's post-fault interaction stage before a scenario is selected,
             * selects the case. F12: shell, F9: Task Manager (stage machines of their own);
             * F11: Files, F10: CiukNote (app scenarios: the launched program gets
             * --test=<its name>, checkpoint PINGs, a fixed clock and the storage write gate). */
            if (gate && interaction && !scenario_key && ev.type==CIUKI_INPUT_KEY && ev.value==1) {
                int code=ev.code;
                int app=code==0x57 ? DESK_APP_FILES : code==0x44 ? DESK_APP_NOTE : 0;
                if (code==0x58 || code==0x43 || app) {
                    if (code==0x58) d->shell_test=1;
                    else if (code==0x43) d->tasks_test=1;
                    else d->app_test=app;
                    scenario_key=code; scenario_released=0; d->shell_keys=1;
                    /* The controller learns of the write gate request before the app can write. */
                    if (app) gate_report(d,CU_PTR(control),generation,survivor_pid,gate_victim,(unsigned)gate_cycle);
                    continue;
                }
            }
            /* Its release is the scenario's second key edge; later F-key use belongs to the programs. */
            if (gate && scenario_key && !scenario_released && ev.type==CIUKI_INPUT_KEY &&
                ev.code==scenario_key && ev.value==0) {
                scenario_released=1; d->shell_keys++; input_dirty=1; continue;
            }
            if (gate && interaction) {
                gate_input(d,&ev);
                input_dirty=1;
            } else desk_input(d,&ev);
        }
        uint64_t input_time=monotonic()-input_begin;
        if (gate) { d->stats.input_ns+=input_time; d->pending_input_ns+=input_time; }
        /* Input has priority over IPC, theme/file reads and rendering. The
         * gate's FIFO may deliberately retain paced events; ordinary input
         * drains again before rendering if a full FIFO left anything queued. */
        desktop_stage(STALL_IPC);
        for (int i=0; i<DESK_CLIENTS; i++) {
            struct desk_client *c=&d->clients[i];
            for (unsigned n=0; n<16 && c->channel>=0; n++) {
                struct ciuki_message m;
                int r=ciuki_channel_recv(c->channel,&m,DONTWAIT);
                if (r==1) desk_receive(d,i,&m);
                else { if (!r || (errno!=EAGAIN && errno!=EINTR)) desk_drop_client(d,i); break; }
            }
            desk_flush(d,i);
        }
        if (!gate && fifo_count==0) {
            (void)desk_drain_input(d,ciuki_input_read);
        }
        desktop_stage(STALL_DISPLAY); display_poll(d);
        if (!gate && d->inputs) startup_stage("first_input_handled");
        desktop_stage(STALL_THEME);
        if (d->shell && !gate && !d->power.state && !(d->sh.want_reload&1) && theme_poll_async(now)) {
            snprintf(d->sh.cfg.theme,sizeof(d->sh.cfg.theme),"%s",desk_theme->name);
            desk_theme_changed(d); d->sh.wall_valid=0; wallpaper_update(d);
        }
        if (d->sh.want_power && !d->sh.gate) d->action=-1;
        desktop_stage(STALL_LAUNCH);
        if (d->action>=0) {
            unsigned selected=(unsigned)d->action; d->action=-1;
            int pid=selected<d->program_count && d->launch ? d->launch(d,&d->programs[selected]) : -1;
            if (pid<0) {
                desk_shell_notify_error(d,"Program did not start",errno);
                desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},d->width,d->height);
            } else if (d->shell_test && d->shell_stage==1 && !strcmp(d->programs[selected].name,"demo")) {
                d->shell_pid=(uint32_t)pid; d->shell_stage=2;
            } else if (d->tasks_test && d->tasks_stage==0 && !strcmp(d->programs[selected].name,"tasks")) {
                d->tasks_pid=(uint32_t)pid; d->tasks_stage=1;
            } else if (d->app_test && !d->app_pid && !strcmp(d->programs[selected].name,desk_app_program(d->app_test))) {
                d->app_pid=(uint32_t)pid;
            }
        }
        desktop_stage(STALL_SHELL);
        if (d->sh.want_reload&1) desktop_input_pending();
        shell_service(d);
        if (d->tasks_test && d->tasks_stage==6 && d->tasks_checkpoint && tasks_verify(d)) {
            d->tasks_stage=DESK_TASKS_DONE; input_dirty=1;
        }
        if ((d->shell_test && d->shell_stage!=shell_stage_shown) || (d->tasks_test && d->tasks_stage!=shell_stage_shown)) {
            /* Lead diagnostics: the shell test's stage is visible in the runner's screenshot. */
            shell_stage_shown=d->tasks_test ? d->tasks_stage : d->shell_stage;
            snprintf(d->notice,sizeof(d->notice),"%s stage %u programs %u",d->tasks_test ? "tasks" : "shell",
                     (unsigned)shell_stage_shown,d->program_count);
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},d->width,d->height);
        }
        if (d->app_test && d->app_checkpoint!=shell_stage_shown) {
            shell_stage_shown=d->app_checkpoint;
            snprintf(d->notice,sizeof(d->notice),"%s test checkpoint %d",d->app_test==DESK_APP_NOTE ? "CiukNote" : "Files",d->app_checkpoint);
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},d->width,d->height);
        }
        /* Interaction frames stay complete frames (the probe and the lead's pins hash them), but only when
         * something changed. Repainting the idle screen on every 1 ms loop made the desktop CPU-bound in
         * the interaction stage; at interactive priority (f0-21) it then starved its normal-priority
         * clients: on image eb774bb9 the Start-menu demo could not answer its close request within
         * DESK_CLOSE_NS, was terminated, and the F12 shell case stayed at stage 5 (keys=0, f2-65). */
        if (interaction && (d->damage.count || d->move_damage.count || d->present_damage.count || d->cursor_dirty))
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
        int render_ready=gate || desk_frame_due(d,monotonic());
        uint64_t presents_before=d->presents; desktop_stage(STALL_RENDER);
        int redraw = render_ready ? desk_redraw(d) : -1;
        if (!gate) desktop_watchdog_frame(d,presents_before,redraw);
        if (render_ready && redraw && errno!=EINTR && errno!=EBUSY && !display_reported) {
            fprintf(stderr,"desktop: present failed (errno=%d)\n",errno); display_reported=1;
        }
        if (!gate && render_ready && !redraw && d->presents) startup_stage("first_frame_presented");
        desktop_stage(STALL_REPORT); startup_flush();
        if (!gate && d->shell && d->presents && !startup_sound) { startup_sound=1; desk_sound(d,SOUND_STARTUP); desk_sound(d,SOUND_LOGON); }
        if (desk_display_frame_commit(d, redraw)) {
            desk_display_reply(d,d->display_reply_mode && d->display_reply_mode!=d->display_mode ? -EIO : 0);
            if (d->shell && !d->sh.gate && settings_save(d))
                snprintf(d->notice, sizeof(d->notice), "Display applied; settings not saved");
        }
        if (!gate) performance_sample(d,monotonic());
        /* The controller hashes the framebuffer during this report: send it
         * only after the checkpoint frame has been presented completely. */
        if (gate && d->app_test && d->app_checkpoint>d->app_reported && !d->damage.count) {
            d->app_reported++;   /* one checkpoint per report, in order */
            gate_report(d,CU_PTR(control),generation,survivor_pid,gate_victim,(unsigned)gate_cycle);
        }
        if (report) {
            int pending=0;
            if (control->command==0) {
                for (int i=0;i<DESK_CLIENTS;i++) {
                    desk_flush(d,i);
                    if (d->clients[i].tx_count) pending=1;
                }
            }
            /* Command snapshots retain their flush barrier. A full
             * nonblocking channel needs another frame, not a server exit. */
            if (pending && now>=report_deadline) return gate_error("flush_timeout",EAGAIN,generation);
            if (!pending && report_send) {
                if (gate_send(d,report_send==GATE_RELEASE ? gate_victim : survivor_pid,report_send,generation)) {
                    int error=errno;
                    if (error!=EAGAIN && error!=EINTR) return gate_error("command_send",error,generation);
                    if (now>=report_deadline) return gate_error("send_timeout",error,generation);
                    pending=1;
                } else report_send=0;
            }
            if (!pending) { report=0; input_dirty=1; next_input_report=0; }
        }
        /* Coalesce interaction evidence across input batches. Unlike command
         * barriers it measures desktop consumption, not client queue drain. */
        if (gate && input_dirty && !report && now>=next_input_report) {
            gate_report(d,CU_PTR(control),generation,survivor_pid,gate_victim,(unsigned)gate_cycle);
            input_dirty=0;
            next_input_report=monotonic()+GATE_REPORT_NS;
            /* Reports use the console too; restore the complete frame. */
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
            if (desk_redraw(d) && errno!=EINTR) return gate_error("report_redraw",errno,generation);
        }
        if (victim_pid>0) {
            int status;
            int r=waitpid(victim_pid,&status,WNOHANG);
            if (r==victim_pid) { victim_pid=-1; completed++; next_victim=now+120000000ull;
                printf("desktop pid=%ld cycles=%d status=%d present=%llu input=%llu received=%llu replies=%llu\n",
                    (long)getpid(),completed,status,(unsigned long long)d->presents,(unsigned long long)d->inputs,
                    (unsigned long long)d->received,(unsigned long long)d->replied);
            }
        }
        /* Reap test demo children without keeping zombies. The kernel controller
         * owns evidence/ticks/ledgers; these lines are application output only. */
        if (!gate && victim_pid<0) {
            int status, pid;
            while ((pid=waitpid(-1,&status,WNOHANG))>0) desk_child_exited(d,(uint32_t)pid,status);
        }
        if (gate && d->shell) for (int i=0; i<DESK_CLIENTS; i++) if (d->children[i].pid) {
            int status, pid=(int)d->children[i].pid;
            if (waitpid(pid,&status,WNOHANG)==pid) desk_child_exited(d,(uint32_t)pid,status);
        }
        /* Scenario progress that completes without input (the shell demo's reap: stage 6; the app's
         * exit: app_done) changes the reported keys (gate_report): report it even when no input
         * follows, or the probe keeps the last input's keys=0 (QEMU image d964369f). */
        unsigned progress=(unsigned)d->shell_stage|(unsigned)d->tasks_stage<<8|(unsigned)d->app_done<<16;
        if (gate && progress!=reported_progress) { reported_progress=progress; input_dirty=1; }
        if (test && victim && victim_pid<0 && completed<cycles && now>=next_victim)
            victim_pid=spawn_demo(d,victim,0);
        /* Sleep even under continuous damage. The independent frame deadline
         * caps rendering; 1 ms service keeps input ahead of the next frame. */
        desktop_stage(STALL_IDLE);
        __atomic_store_n(&desktop_watchdog.loop_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
        struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
}
