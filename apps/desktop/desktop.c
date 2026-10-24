/* SPDX-License-Identifier: MIT */
#include "../files/operations.h"
#include "desktop.h"
#include <ciuki/present_abi.h>
#include "../../sdk/sysroot-overlay/include/ciuki/dosvm.h"
#include "../gfxdemo/benchmark.h"
#include "sound_runtime.h"
#include "firstboot.h"
#include "startup.h"
#include "sound_storage.h"
#include "media_notice.h"
#include "settings_io.h"
#include "users_worker.h"
#include "shortcut_io.h"
#include "schedsvc.h"
#include "resources.h"
#include "wallpaper_io.h"
#ifdef __i386__
#include <ciuki/devices.h>
#endif
#include "catalog.h"
#include "gate.h"
#include "../libdeskui/deskui.h"
#include "../libdeskui/completion.h"
#include <poll.h>
#include <ciuki/raw.h>
#include <ciuki/runtime.h>
#include <ciuki/channel.h>
#include "clipboard.h"
#include "../libdeskui/clipboard.h"
#include <ciuki/spawn.h>
#include <ciuki/surface.h>
#include <ciuki/display.h>
#include <ciuki/proc.h>
#include <ciuki/audio.h>
#include <ciuki/net.h>
#include <ciuki/media.h>
#include <ciuki/power.h>
#ifdef __i386__
#include <ciuki/timezone.h>
#endif
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
static int screen_control(struct ciuki_screen_control *q)
{ return (int32_t)CU_CALL(DISPLAY,CIUKI_DISPLAY_SCREEN,CU_PTR(q),0,0,0,0); }
static int monitor_power(int state)
{ return (int32_t)CU_CALL(DISPLAY,CIUKI_DISPLAY_DPMS,(uint32_t)state,0,0,0,0); }
static uint64_t monotonic(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000000000ull+(uint32_t)t.tv_nsec;
}
static struct du_completion desktop_completion;
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
/* Input and completion descriptors use the f2-88c native wait. */
/* f2-95: which deadline shortened the wait (the [desktop] loop record). */
enum { PAUSE_IDLE, PAUSE_INPUT, PAUSE_FRAME, PAUSE_ANIMATION, PAUSE_TIMER, PAUSE_PING, PAUSE_CLOSE,
       PAUSE_NOTIFY, PAUSE_HOVER, PAUSE_POWER, PAUSE_CACHE, PAUSE_REASONS };
static const char *const pause_names[PAUSE_REASONS]={"idle","input","frame","animation","timer","ping",
    "close","notify","hover","power","cache"};
static unsigned pause_reason;
static void loop_deadline_for(uint64_t now,uint64_t deadline,uint64_t *wait,unsigned reason)
{
    if (deadline==UINT64_MAX) return;
    uint64_t left=deadline>now ? deadline-now : 1000000ull;
    if (left<*wait) { *wait=left; pause_reason=reason; }
}
#define loop_deadline(now,deadline,wait) loop_deadline_for(now,deadline,wait,reason)
static uint64_t desktop_pause_ns(const struct desktop *d,uint64_t now,int input_pending,uint64_t timer)
{
    uint64_t wait=input_pending ? 0 : UINT64_MAX;
    unsigned reason=PAUSE_FRAME;
    pause_reason=input_pending ? PAUSE_INPUT : PAUSE_IDLE;
    if (d->damage.count || d->move_damage.count || d->present_damage.count || d->cursor_dirty)
        loop_deadline(now,desk_frame_time(d,now),&wait);
    reason=PAUSE_ANIMATION;
    if (d->animation.active)
        loop_deadline(now,d->animation.started+(d->animation.step+1)*25000000ull,&wait);
    for(unsigned k=0;k<d->pointer_trail_count;k++)loop_deadline(now,d->pointer_trail[k].until,&wait);
    reason=PAUSE_TIMER;
    if(d->shell && d->shell_caret_next)loop_deadline(now,d->shell_caret_next,&wait);
    loop_deadline(now,timer,&wait);
    if (d->shell && !d->sh.gate && !d->power.state) {
        loop_deadline(now,d->saver_poll ? d->saver_poll : now+1000000000ull,&wait);
        if (d->saver_active && !d->monitor_off && d->saver_frame!=UINT64_MAX) loop_deadline(now,d->saver_frame,&wait);
    }
    reason=PAUSE_PING;
    for (int i=0;i<DESK_CLIENTS;i++) {
        const struct desk_client *c=&d->clients[i];
        if (c->channel<0 || !c->hello || (c->version==DESK_V3_VERSION && !c->window)) continue;
        if (!c->ping_pending && c->tx_count<DESK_TX_MAX) loop_deadline(now,c->next_ping_ns,&wait);
        else if (c->ping_pending && !c->unresponsive) loop_deadline(now,c->ping_sent_ns+5000000000ull,&wait);
    }
    if (d->shell && !d->power.state) {
        reason=PAUSE_CLOSE;
        uint64_t session=desk_session_deadline(d);   /* G3: queries, Win32 broker polls, hung closes */
        if (session) loop_deadline(now,session,&wait);
        for (int i=0;i<DESK_CLIENTS;i++) if (d->children[i].pid && d->children[i].close_ns && !d->children[i].term_sent)
            loop_deadline(now,d->children[i].close_ns,&wait);
        reason=PAUSE_NOTIFY;
        const struct sh_notification *n=&d->sh.notification;
        if (n->active) {
            loop_deadline(now,n->expires,&wait);
            if (now>=n->started && now-n->started<160000000ull)
                loop_deadline(now,n->started+((now-n->started)/8000000ull+1)*8000000ull,&wait);
        }
        reason=PAUSE_HOVER;
        if (!d->sh.tooltip && d->sh.hover_since && d->sh.hover_target && !n->active &&
            !d->sh.levels && !d->sh.volume_open && !d->sh.network_open && !d->sh.media_open)
            loop_deadline(now,d->sh.hover_since+600000000ull,&wait);
    } else if (d->shell && !d->sh.gate) {
        /* Shutdown suspends ordinary shell ticks and child close timeouts.
         * Stale shell deadlines must not turn its static Busy panel into a
         * 1 ms poll; only the active shutdown phase supplies a deadline. */
        reason=PAUSE_POWER;
        if (d->power.state==DESK_POWER_CLOSING)
            loop_deadline(now,now<d->power.close_deadline ? d->power.close_deadline : now+DESK_POWER_RETRY_NS,&wait);
        else if (d->power.state==DESK_POWER_RETRY || d->power.state==DESK_POWER_FORCE)
            loop_deadline(now,d->power.next_try,&wait);
    }
    return wait;
}
#undef loop_deadline
/* f2-95: service turns per 2 s and what shortened them. An idle desktop
 * turns 100 times a second; a deadline left in the past turns it every
 * 1 ms. Reported when above 150 turns/s, otherwise once a minute. */
static struct { unsigned turns, short_turns, reasons[PAUSE_REASONS]; uint64_t quiet_ns; } loop_turns;
static void loop_turn(uint64_t wait)
{
    loop_turns.turns++;
    if (wait<10000000ull) { loop_turns.short_turns++; loop_turns.reasons[pause_reason<PAUSE_REASONS ? pause_reason : 0]++; }
}
static int loop_record_line(char *line,size_t size,uint64_t now,uint64_t window_ns)
{
    unsigned top=0;
    for (unsigned i=1;i<PAUSE_REASONS;i++) if (loop_turns.reasons[i]>loop_turns.reasons[top]) top=i;
    uint64_t per_second=window_ns ? (uint64_t)loop_turns.turns*1000000000ull/window_ns : 0;
    int n=0;
    if (per_second>150 || now-loop_turns.quiet_ns>=60000000000ull) {
        if (per_second<=150) loop_turns.quiet_ns=now;
        n=snprintf(line,size,"[desktop] loop turns=%u short=%u top=%s:%u window_ms=%llu\n",loop_turns.turns,
                   loop_turns.short_turns,pause_names[top],loop_turns.reasons[top],(unsigned long long)(window_ns/1000000ull));
    }
    uint64_t quiet=loop_turns.quiet_ns; memset(&loop_turns,0,sizeof(loop_turns)); loop_turns.quiet_ns=quiet;
    return n>0 && (size_t)n<size ? n : 0;
}
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
/* G1: once a second, the system zone: the tray clock's offset, the TZ of
 * launched programs and of the desktop itself, and the one-time notice
 * after the kernel moved the RTC for a daylight-saving change (Windows
 * tells the user the same). Host harnesses keep UTC. */
static void desktop_time_refresh(struct desktop *d)
{
#ifdef __i386__
    struct ciuki_time_zone z;
    memset(&z,0,sizeof(z));
    if (ciuki_time_zone(&z,CIUKI_TIME_GET)) return;
    d->local_offset=z.offset_minutes*60;
    d->time_flags=z.flags;
    if (z.generation!=d->time_generation) {
        d->time_generation=z.generation;
        ciuki_tz_env(d->tz_env,sizeof(d->tz_env));
        setenv("TZ",d->tz_env+3,1); tzset();
    }
    if ((z.flags&CIUKI_TIME_ADJUSTED) && (z.flags&CIUKI_TIME_AUTHORITY)) {
        desk_shell_notify(d,"Date/Time","CiukiOS has adjusted your clock for daylight saving changes.",DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS);
        ciuki_time_zone(&z,CIUKI_TIME_ACKNOWLEDGE);
    }
#else
    (void)d;
#endif
}
static int spawn_demo(struct desktop *d, const char *kind, int gate)
{
    int pair[2];
    if (ciuki_channel_pair(pair)) return -1;
    char fault[48];
    snprintf(fault,sizeof(fault),"--fault=%s",kind);
    char *argv[] = {"demo","--channel-fd=3",fault,gate ? "--test=crash-isolation" : NULL,NULL};
    char language[20]; snprintf(language,sizeof(language),"LANG=%s",reg_locale(&d->sh.cfg.regional));
    char *env[] = {language,d->tz_env[0] ? d->tz_env : "TZ=UTC0",NULL};
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
/* fx-8: the desktop never waits for a process to be created.
 *
 * ciuki_spawn() returns only after the kernel has read the executable and
 * built the child (proc_spawn -> image_open -> elf_load). On the T23 trial of
 * image 423b803a that took 3.9 s for NetSurf (13.5 MB file) and 0.5-1.1 s for
 * the 1.5-3.8 MB built-in programs, all of it inside the desktop's frame and
 * input loop (`[desktop] stall stage=launch ... wait=launch no_frame_ms=3891`).
 * Windows 9x's shell returns from ShellExecute at once, shows the AppStarting
 * cursor and lets the window appear when the program creates it; a failed
 * start is reported afterwards. The desktop now does the same: the input
 * thread prepares the channel and the client slot (cheap, no file I/O), a
 * launcher worker performs the spawn, and the next service turn binds the PID
 * or reports the failure to the launch's origin (desk_launch_completed).
 * Gate scenarios keep the synchronous call: their controller sequences
 * expect the PID at the click. */
enum { LAUNCH_FREE, LAUNCH_QUEUED, LAUNCH_RUNNING, LAUNCH_DONE };
#define LAUNCH_ARGV 13
#define LAUNCH_STRINGS 2048
#define LAUNCH_EXITS 8
struct launch_job {
    unsigned state, sequence;
    int client, child, channel, child_fd;
    uint64_t click_ns, begin_ns, end_ns;
    struct desk_program program;
    char *argv[LAUNCH_ARGV], *env[12];
    char strings[LAUNCH_STRINGS], display[40], keymap[24], tz[8 + CIUKI_TIME_RULE_MAX], language[20];
    struct ciuki_spawn_fd mapping[2];
    uint32_t mappings;
    int (*mark)(uint32_t, ciuki_pid_t, uint64_t);
    struct desk_launch_request request;
    ciuki_pid_t pid;
    int error;
};
static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    int started;
    unsigned sequence;
    void (*wake)(void *);
    void *wake_ctx;
    struct launch_job jobs[DESK_CLIENTS];
    /* Input-thread only: exits reaped while a spawn was still unbound. */
    uint32_t exited[LAUNCH_EXITS];
    unsigned exited_next;
} launcher={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};
static struct launch_job launch_sync_job;
static void *launcher_run(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&launcher.lock);
    for (;;) {
        struct launch_job *job=NULL;
        for (unsigned i=0;i<DESK_CLIENTS;i++) {
            struct launch_job *j=&launcher.jobs[i];
            if (j->state==LAUNCH_QUEUED && (!job || (int)(j->sequence-job->sequence)<0)) job=j;
        }
        if (!job) { pthread_cond_wait(&launcher.changed,&launcher.lock); continue; }
        job->state=LAUNCH_RUNNING;
        pthread_mutex_unlock(&launcher.lock);
        /* The job's strings and descriptors stay owned by the input thread,
         * which does not touch a RUNNING job; only the spawn runs here. */
        uint64_t begin=monotonic();
        if (job->mark) job->mark(CIUKI_LAUNCH_BEGIN,0,job->click_ns/1000);
        errno=0;
        ciuki_pid_t pid=ciuki_spawn(job->program.path,job->argv,job->env,job->mapping,job->mappings,0);
        int error=errno;
        uint64_t end=monotonic();
        pthread_mutex_lock(&launcher.lock);
        job->begin_ns=begin; job->end_ns=end; job->pid=pid; job->error=pid<0 ? (error ? error : EIO) : 0; job->state=LAUNCH_DONE;
        if (launcher.wake) launcher.wake(launcher.wake_ctx);
    }
    return NULL;
}
/* Started once by the ordinary (non-gate) desktop; a failure keeps the
 * synchronous launch, which is correct, only slower to the eye. */
static int launcher_start(struct desktop *d)
{
    if (launcher.started) { d->launch_async=1; return 0; }
    launcher.wake=d->wake; launcher.wake_ctx=d->wake_ctx;
    int error=pthread_create(&launcher.thread,NULL,launcher_run,NULL);
    if (error) return error;
    launcher.started=1; d->launch_async=1;
    return 0;
}
static int launcher_inflight(void)
{
    int busy=0;
    pthread_mutex_lock(&launcher.lock);
    for (unsigned i=0;i<DESK_CLIENTS;i++) busy|=launcher.jobs[i].state!=LAUNCH_FREE;
    pthread_mutex_unlock(&launcher.lock);
    return busy;
}
static int launch_strings(struct launch_job *job, unsigned *used, const char *text, char **out)
{
    size_t n=strlen(text)+1;
    if (n>LAUNCH_STRINGS-*used) return -1;
    memcpy(job->strings+*used,text,n); *out=job->strings+*used; *used+=(unsigned)n;
    return 0;
}
/* The complete spawn request, copied: the caller's argument pointers
 * (a LAUNCH message, the Run dialog's words) do not outlive this call. */
static int launch_prepare(struct desktop *d, const struct desk_program *program, struct launch_job *job)
{
    memset(job,0,sizeof(*job));
    job->program=*program;
    int graphical=desk_program_graphical(program->name), kind=desk_program_kind(program->name);
    int tasks=!strcmp(program->name,"tasks");
    int takes_file=kind==DESK_APP_NOTE || desk_catalog_takes_file(program->name);
    const char *argv[LAUNCH_ARGV]={program->name};
    unsigned at=1;
    /* The program record grants the window channel (catalog.h; f6-19: also NetSurf). */
    if (graphical) argv[at++]="--channel-fd=3";
    /* The scenario's program gets --test=<its name> (apps/desktop/gate.h scenarios). */
    if (graphical && d->app_test && kind==d->app_test) argv[at++]=kind==DESK_APP_NOTE ? "--test=ciuknote" : "--test=files";
    /* f2-33: only the Task Manager receives the delegated termination
     * authority, explicitly mapped to fd 4; it also may send task requests. */
    if (tasks && d->authority_fd>=0) argv[at++]="--authority-fd=4";
    if (tasks && d->tasks_test) argv[at++]="--test=tasks";
    /* LAUNCH from Files: the path of the text file CiukNote opens (the client validated it).
     * The shell also opens Files on a folder and the Control Panel on a page.
     * Accessories that open a file (Image Viewer, Help) take it as CiukNote does. */
    if (graphical && (kind || takes_file) && d->launch_arg) argv[at++]=d->launch_arg;
    /* Run dialog: the typed arguments after the desktop's own. */
    if (d->launch_argv[0]) for (unsigned k=0; k<6 && d->launch_argv[k] && at<LAUNCH_ARGV-1; k++) argv[at++]=d->launch_argv[k];
    unsigned used=0;
    for (unsigned k=0;k<at;k++) if (launch_strings(job,&used,argv[k],&job->argv[k])) return E2BIG;
    job->argv[at]=NULL;
    /* Window size hint for graphical clients; the protocol has no query. */
    snprintf(job->display,sizeof(job->display),"CIUKI_DISPLAY=%ux%u",(unsigned)d->width,(unsigned)d->height);
    /* The Control Panel's keyboard layout, for libdeskui programs. */
    snprintf(job->keymap,sizeof(job->keymap),"CIUKI_KEYMAP=%s",cfg_keyboard_names[d->sh.cfg.keyboard==CFG_KEYS_IT ? CFG_KEYS_IT : CFG_KEYS_US]);
    snprintf(job->tz,sizeof(job->tz),"%s",d->tz_env[0] ? d->tz_env : "TZ=UTC0");
    snprintf(job->language,sizeof(job->language),"LANG=%s",reg_locale(&d->sh.cfg.regional));
    char home[160], user[64], settings[160], profile[160];
    snprintf(home,sizeof(home),"HOME=%s",d->sh.profile.documents[0] ? d->sh.profile.documents : "/Documents");
    snprintf(user,sizeof(user),"USER=%s",d->sh.profile.name);
    snprintf(settings,sizeof(settings),"CIUKI_SETTINGS=%s",d->sh.profile.settings[0] ? d->sh.profile.settings : CFG_FILE);
    snprintf(profile,sizeof(profile),"CIUKI_PROFILE=%s",d->sh.profile.root);
    const char *env[]={job->language,job->tz,home,"TMPDIR=/tmp","PATH=/Programs",job->display,job->keymap,user,settings,profile,NULL};
    for (unsigned k=0;env[k];k++) if (launch_strings(job,&used,env[k],&job->env[k])) return E2BIG;
    job->mappings=tasks && d->authority_fd>=0 ? 2 : 1;
    job->mapping[1]=(struct ciuki_spawn_fd){d->authority_fd,4};
    job->request=d->launch_request;
    return 0;
}
static void launch_bind(struct desktop *d, int index, const struct desk_program *program, uint64_t click_ns)
{
    struct desk_client *c=&d->clients[index];
    int kind=desk_program_kind(program->name);
    snprintf(c->application,sizeof(c->application),"%s",program->name);
    c->program=kind;
    c->taskman=!strcmp(program->name,"tasks");
    c->sysapp=!strcmp(program->name,"sysinfo") || !strcmp(program->name,"sysmon") || !strcmp(program->name,"schedtask") || !strcmp(program->name,"resmeter");
    c->gfxdemo=!strcmp(program->name,"gfxdemo");
    c->dosbox=!strcmp(program->name,"dosbox");
    c->mixer=!strcmp(program->name,"mixer") || !strcmp(program->name,"music") || !strcmp(program->name,"terminal");
    c->awaiting_frame=desk_program_graphical(program->name);
    c->initial_show=d->launch_show;
    c->launched_ns=monotonic();
    c->launch_input_ns=click_ns;
}
static int launch_program(struct desktop *d, const struct desk_program *program)
{
    d->launch_deferred=0;
    if (d->power.state) return launch_failed(program,EBUSY,"power");
    /* Recheck each launch: the operator can change console verbosity at run time. */
    d->launch_mark = ciuki_launch_mark(CIUKI_LAUNCH_STATUS,0,0) > 0 ? ciuki_launch_mark : NULL;
    uint64_t click_ns = d->launch_click_ns ? d->launch_click_ns : monotonic();
    d->launch_click_ns = 0;
    int child=-1;
    for (int j=0; j<DESK_CLIENTS; j++) if (!d->children[j].pid && !d->children[j].pending) { child=j; break; }
    if (child<0) return launch_failed(program,EAGAIN,"child-slot");
    struct launch_job *job=&launch_sync_job;
    if (d->launch_async) {
        job=NULL;
        pthread_mutex_lock(&launcher.lock);
        for (unsigned i=0;i<DESK_CLIENTS && !job;i++) if (launcher.jobs[i].state==LAUNCH_FREE) job=&launcher.jobs[i];
        pthread_mutex_unlock(&launcher.lock);
        if (!job) return launch_failed(program,EAGAIN,"launch-queue");
    }
    /* A FREE job belongs to the input thread until it is queued. */
    int error=launch_prepare(d,program,job);
    if (error) return launch_failed(program,error,"arguments");
    int pair[2];
    if (ciuki_channel_pair(pair)) return launch_failed(program,errno,"channel-pair");
    job->mapping[0]=(struct ciuki_spawn_fd){pair[1],3};
    job->click_ns=click_ns; job->mark=d->launch_mark;
    int index=-1;
    ciuki_pid_t pid=-1;
    const char *operation="channel-inherit";
    if (!fcntl(pair[1],F_SETFD,0)) {
        index=desk_add_client(d,pair[0]);
        if (index>=0) operation="spawn";
        else { errno=ENOSPC; operation="client-slot"; }
    }
    if (index>=0 && d->launch_async) {
        /* The slot shows the AppStarting cursor at once; its channel is
         * neither read nor polled until the PID is bound (launch_service). */
        launch_bind(d,index,program,click_ns);
        d->clients[index].spawn_pending=1;
        d->children[child].pending=1;
        job->client=index; job->child=child; job->channel=pair[0]; job->child_fd=pair[1];
        pthread_mutex_lock(&launcher.lock);
        job->sequence=++launcher.sequence; job->state=LAUNCH_QUEUED;
        pthread_cond_signal(&launcher.changed);
        pthread_mutex_unlock(&launcher.lock);
        d->launch_deferred=1;
        desk_cursor_update(d);
        return 0;
    }
    if (index>=0) {
        if (d->launch_mark) d->launch_mark(CIUKI_LAUNCH_BEGIN,0,click_ns/1000);
        pid=ciuki_spawn(job->program.path,job->argv,job->env,job->mapping,job->mappings,0);
    }
    error=errno;
    close(pair[1]);
    if (pid<0) {
        if (index>=0) desk_drop_client(d,index); else close(pair[0]);
        return launch_failed(program,error,operation);
    }
    /* Bind launch ownership before accepting the first kernel-stamped message.
     * This also permits cleanup if a child exits without ever sending HELLO. */
    d->clients[index].pid=(uint32_t)pid;
    launch_bind(d,index,program,click_ns);
    d->children[child].pid=(uint32_t)pid;
    return pid;
}
/* One line per worker launch, also in quiet boots (the trial had none):
 * the queue wait from the click and the kernel's spawn time. */
static void launch_timing_record(const struct launch_job *job)
{
    char line[200];
    uint64_t queued=job->begin_ns>job->click_ns ? job->begin_ns-job->click_ns : 0;
    uint64_t spawn=job->end_ns>job->begin_ns ? job->end_ns-job->begin_ns : 0;
    int n=snprintf(line,sizeof(line),"[desktop] launch name=%.40s pid=%d error=%d queue_ms=%llu spawn_ms=%llu",
                   job->program.name,job->pid<0 ? 0 : (int)job->pid,job->pid<0 ? job->error : 0,
                   (unsigned long long)(queued/1000000u),(unsigned long long)(spawn/1000000u));
    if (n<=0 || (size_t)n>=sizeof(line)) return;
#ifdef __i386__
    (void)ciuki_raw_debug_write(CU_PTR(line),(uint32_t)n,0,0,0,0);
#else
    (void)write(STDERR_FILENO,line,(size_t)n);
    (void)write(STDERR_FILENO,"\n",1);
#endif
}
/* fx-15: click -> first frame on screen, one line per launched program, also
 * in quiet boots: process_ms (queue and spawn, from the click), window_ms
 * (process start to CREATE_WINDOW), paint_ms (CREATE_WINDOW to the first
 * frame: program initialisation and first render), present_ms (first frame
 * to the completed present), total_ms (click to the frame on screen). */
static unsigned long long span_ms(uint64_t from, uint64_t to)
{ return from && to && to>=from ? (unsigned long long)((to-from)/1000000u) : 0ull; }
static void first_frame_record(struct desktop *d, const struct desk_client *c, uint64_t presented)
{
    (void)d;
    char line[240];
    uint64_t click=c->launch_input_ns ? c->launch_input_ns : c->launched_ns;
    int n=snprintf(line,sizeof(line),"[desktop] first-frame name=%.40s pid=%u process_ms=%llu window_ms=%llu paint_ms=%llu present_ms=%llu total_ms=%llu",
                   c->application[0] ? c->application : c->title,(unsigned)c->pid,span_ms(click,c->launched_ns),
                   span_ms(c->launched_ns,c->window_ns),span_ms(c->window_ns,c->damage_ns),span_ms(c->damage_ns,presented),
                   span_ms(click,presented));
    if (n<=0 || (size_t)n>=sizeof(line)) return;
#ifdef __i386__
    (void)ciuki_raw_debug_write(CU_PTR(line),(uint32_t)n,0,0,0,0);
#else
    (void)write(STDERR_FILENO,line,(size_t)n);
    (void)write(STDERR_FILENO,"\n",1);
#endif
}
/* Input thread: an exit reaped while a spawn may still be unbound. */
static void launch_note_exit(struct desktop *d, uint32_t pid)
{
    for (int i=0;i<DESK_CLIENTS;i++) if (d->children[i].pid==pid || d->clients[i].pid==pid) return;
    if (!launcher.started || !launcher_inflight()) return;
    launcher.exited[launcher.exited_next++%LAUNCH_EXITS]=pid;
}
static int launch_exited(uint32_t pid)
{
    for (unsigned i=0;i<LAUNCH_EXITS;i++) if (launcher.exited[i]==pid) { launcher.exited[i]=0; return 1; }
    return 0;
}
/* Input thread, every turn: bind the PIDs of completed spawns or report
 * their failure. Returns the number of completions. */
static unsigned launch_service(struct desktop *d)
{
    if (!launcher.started) return 0;
    /* One completed job at a time, oldest sequence first. The input thread
     * owns this scratch copy; command text must not consume its 4 KiB stack. */
    static struct launch_job one; /* input-thread scratch; includes the bounded Run command */
    struct launch_job *job=&one;
    unsigned count=0;
    for (;;) {
        pthread_mutex_lock(&launcher.lock);
        struct launch_job *next=NULL;
        for (unsigned i=0;i<DESK_CLIENTS;i++) {
            struct launch_job *j=&launcher.jobs[i];
            if (j->state==LAUNCH_DONE && (!next || (int)(j->sequence-next->sequence)<0)) next=j;
        }
        if (next) { one=*next; next->state=LAUNCH_FREE; }
        pthread_mutex_unlock(&launcher.lock);
        if (!next) break;
        count++;
        launch_timing_record(job);
        close(job->child_fd);
        d->children[job->child].pending=0;
        struct desk_client *c=job->client>=0 ? &d->clients[job->client] : NULL;
        int live=c && c->channel==job->channel && c->spawn_pending;
        if (job->pid<0) {
            if (live) desk_drop_client(d,job->client); /* a dropped slot already closed its channel */
            (void)launch_failed(&job->program,job->error,"spawn");
            desk_launch_completed(d,&job->request,job->program.name,-job->error);
            continue;
        }
        uint32_t pid=(uint32_t)job->pid;
        int gone=launch_exited(pid);
        if (live) {
            c->spawn_pending=0; c->pid=pid;
            c->launched_ns=monotonic(); /* AppStarting runs from the process's creation */
        }
        if (!gone) d->children[job->child].pid=pid;
        desk_launch_completed(d,&job->request,job->program.name,(int)pid);
        if (gone && live) desk_drop_client(d,job->client);
    }
    if (count) desk_cursor_update(d);
    return count;
}
void desk_v3_fixture_report(const char *line);
/* f6-18 GATE_BROWSER: start /Programs/netsurf --channel-fd=3 --test=browser <url>
 * through the ordinary launch path (same environment, channel and client slot
 * as a menu launch) and name the child to the controller; the kernel validates
 * the PID and reads the browser's page outcome from it. One shot per gate command.
 * f6-19: the window channel comes from NetSurf's program record, not from the gate. */
static int desk_browser_fixture_begin(struct desktop *d,const char *url)
{
    static char gate_browser_url[48];
    struct desk_program program;
    memset(&program,0,sizeof(program));
    snprintf(program.name,sizeof(program.name),"netsurf");
    snprintf(program.path,sizeof(program.path),"/Programs/netsurf");
    snprintf(gate_browser_url,sizeof(gate_browser_url),"%s",url);
    d->launch_arg=NULL;
    memset(d->launch_argv,0,sizeof(d->launch_argv));
    d->launch_argv[0]="--test=browser"; d->launch_argv[1]=gate_browser_url;
    int pid=launch_program(d,&program), error=errno;
    memset(d->launch_argv,0,sizeof(d->launch_argv));
    char line[80];
    snprintf(line,sizeof(line),"case=browser child=%d",pid<0 ? 0 : pid);
    desk_v3_fixture_report(line);
    return pid<0 ? -error : pid;
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
static struct users_worker user_worker;
static int user_initial, user_switch_saving, user_default_loading, user_default_ready;
static int links_profile_update(struct desktop *);
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
    const struct desktop *desktop;
    unsigned live, stop, stage, pending, work, frame_ms, loop_ms;
    const char *wait;
} desktop_watchdog;
static pthread_mutex_t watchdog_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t watchdog_changed=PTHREAD_COND_INITIALIZER;
static const char *const desktop_stages[]={"idle","input","ipc","display",
    "theme","launch","shell","render","report"};
static void desktop_wait(const char *name)
{ __atomic_store_n(&desktop_watchdog.wait,name,__ATOMIC_RELEASE); }
static void desktop_stage(unsigned stage)
{
    desktop_wait(stage<sizeof(desktop_stages)/sizeof(*desktop_stages) ? desktop_stages[stage] : "unknown");
    unsigned previous=__atomic_exchange_n(&desktop_watchdog.stage,stage,__ATOMIC_ACQ_REL);
    if (previous==STALL_IDLE && stage!=STALL_IDLE && desktop_watchdog.live) {
        pthread_mutex_lock(&watchdog_lock);pthread_cond_signal(&watchdog_changed);pthread_mutex_unlock(&watchdog_lock);
    }
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
    int n=snprintf(line,size,"[desktop] stall stage=%s worker=%s/%s no_frame_ms=%u input_pending=%u work_pending=%u wait=%s wallpaper_wait=%s autoplay_wait=%s folder_wait=%s\n",
        stage<sizeof(desktop_stages)/sizeof(*desktop_stages) ? desktop_stages[stage] : "unknown",
        wall,settings,now-frame,pending,work,wait ? wait : "unknown",wall_wait ? wall_wait : "starting",desk_autoplay_activity(desktop_watchdog.desktop),desk_folder_activity(desktop_watchdog.desktop));
    return n>0 && (size_t)n<size ? n : 0;
}
static void *desktop_watchdog_run(void *unused)
{
    (void)unused; uint32_t reported=0;
    while (!__atomic_load_n(&desktop_watchdog.stop,__ATOMIC_ACQUIRE)) {
        uint32_t now=(uint32_t)(monotonic()/1000000ull);
        char line[320]; int n;
        if((n=desk_v3_restore_watchdog((struct desktop *)desktop_watchdog.desktop,now,line,sizeof(line)))>0) {
#ifdef __i386__
            if(line[n-1]=='\n')--n;
            (void)ciuki_raw_debug_write(CU_PTR(line),(uint32_t)n,0,0,0,0);
#else
            (void)write(STDERR_FILENO,line,(size_t)n);
#endif
        }
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
        pthread_mutex_lock(&watchdog_lock);
        /* Idle needs no watchdog sampling. Active work retains the existing
         * diagnostic deadline; an input/service turn wakes this condition. */
        const struct desktop *d=desktop_watchdog.desktop;
        if (__atomic_load_n(&desktop_watchdog.stage,__ATOMIC_ACQUIRE)==STALL_IDLE &&
            !__atomic_load_n(&desktop_watchdog.pending,__ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&desktop_watchdog.work,__ATOMIC_ACQUIRE) &&
            (!d || !__atomic_load_n(&d->v3_restore_watchdog_pending,__ATOMIC_ACQUIRE))) {
            pthread_cond_wait(&watchdog_changed,&watchdog_lock);
        } else {
            struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=100000000;
            if(until.tv_nsec>=1000000000){until.tv_sec++;until.tv_nsec-=1000000000;}
            pthread_cond_timedwait(&watchdog_changed,&watchdog_lock,&until);
        }
        pthread_mutex_unlock(&watchdog_lock);
    }
    return NULL;
}
static void desktop_watchdog_start(struct desktop *d)
{
    desktop_watchdog.desktop=d;
    __atomic_store_n(&desktop_watchdog.frame_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
    __atomic_store_n(&desktop_watchdog.loop_ms,(uint32_t)(monotonic()/1000000ull),__ATOMIC_RELEASE);
    int error=pthread_create(&desktop_watchdog.thread,NULL,desktop_watchdog_run,NULL);
    if (error) { desk_shell_notify_error(d,"Desktop watchdog unavailable",error); return; }
    desktop_watchdog.live=1; d->input_pending=desktop_input_pending;
}
/* result: desk_redraw's value, or -1 with errno 0 when no frame was due.
 * f2-95: a frame is the desktop catching up with its input, whether it
 * wrote scanout rows, only moved the hardware cursor or found nothing left
 * to draw (an EBUSY display belongs to its owner). The T23 trial a4d9454e
 * logged `stall stage=idle ... input_pending=1 wait=idle` 19 times while the
 * pointer moved only the hardware cursor: those frames write no row, so the
 * stamp kept the last row write and the observer reported a stall that was
 * none. An up-to-date desktop also renews the stamp, so a later blocked
 * loop reports the time since it was last current. */
static void desktop_watchdog_frame(struct desktop *d,uint64_t before,int result)
{
    (void)before;
    unsigned waiting=d->sh.wallpaper_pending || settings_reload_pending;
    __atomic_store_n(&desktop_watchdog.work,waiting,__ATOMIC_RELEASE);
    int outstanding=d->damage.count || d->move_damage.count || d->present_damage.count || d->cursor_dirty;
    if (!result || errno==EBUSY || (!outstanding && !settings_reload_pending)) {
        /* The turn's input clock (no extra kernel entry on an idle turn). */
        uint64_t now=d->now_ns ? d->now_ns : monotonic();
        __atomic_store_n(&desktop_watchdog.frame_ms,(uint32_t)(now/1000000ull),__ATOMIC_RELEASE);
        __atomic_store_n(&desktop_watchdog.pending,0,__ATOMIC_RELEASE);
    }
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
static int desktop_joystick_control(struct desktop *d, struct ciuki_joystick_control *c)
{
#ifdef __i386__
    if (d->input<0) return -ENODEV;
    return ciuki_devctl(d->input,INPUT_JOY_CONTROL,c,sizeof(*c)) ? -errno : 0;
#else
    (void)d; (void)c; return -ENODEV;
#endif
}
static uint32_t mixer_serial;
static int32_t last_ec_preference=-1;
/* fx-9b: a volume-key (EC) master the kernel applied but did not save is
 * persisted by this normal-priority writer once it has been unchanged for
 * MASTER_QUIET_NS (a burst of presses spans several 2 s EC polls), and
 * before a shutdown; one registry write per burst. */
#define MASTER_QUIET_NS 3000000000ull
static int32_t ec_candidate=-1;
/* fx-26: mixer change readiness, see mixer_poll (-1 unknown, 1 on, 0 off). */
static int mixer_events=-1;
static uint64_t ec_candidate_ns;
static struct ciuki_mixer_master ec_candidate_master;
static int master_candidate_save(int fd);
#include "mixer_job.h"
static int mixer_line_save(int fd,struct ciuki_mixer_line *line)
{
    int error=ciuki_devctl(fd,MIXER_SET_LINE,line,sizeof(*line)) ? errno : 0;
    if (!error && line->recording && !line->selected) {
        int found=0;
        for (unsigned i=1;i<=CIUKI_MIXER_MIC;i++) {
            if (i==CIUKI_MIXER_MIDI) continue;
            struct ciuki_mixer_line selected={.version=1,.size=sizeof(selected),.recording=1,.line=i};
            if (ciuki_devctl(fd,MIXER_GET_LINE,&selected,sizeof(selected))) { error=errno; break; }
            if (selected.selected) { *line=selected; found=1; break; }
        }
        if (!error && !found) error=EIO;
    }
    if (!error && line_preference_save(REG_DIRECTORY,line)) error=errno;
    return error;
}
static int mixer_job_run(void *record, size_t n)
{
    if (n!=sizeof(struct mixer_job)) return EINVAL;
    struct mixer_job job; memcpy(&job,record,n);
    if (job.mask & ~0x7ffu) return EINVAL;
    int error=0;
    if (job.mask & 1) {
        if (!job.save_only) error=ciuki_devctl(job.fd,MIXER_SET_MASTER,&job.master,sizeof(job.master)) ? errno : 0;
        if (!error) {
            int saved=master_preference_save(REG_DIRECTORY,job.master.left,job.master.right,job.master.mute);
            job.master.save_state=saved ? CIUKI_MIXER_UNSAVED : CIUKI_MIXER_SAVED;
            job.master.save_error=saved ? -errno : 0;
        }
    }
    for (unsigned i=1;i<=5;i++) if (job.mask & (1u<<i)) {
        uint16_t v=job.values[i-1];
        struct ciuki_mixer_line line={.version=1,.size=sizeof(line),.line=i>5 ? i-5 : i,.recording=i>5,
            .left=v&127,.right=(v>>7)&127,.mute=(v>>14)&1,.selected=v>>15};
        int e=mixer_line_save(job.fd,&line); if (!error) error=e;
    }
    struct ciuki_mixer_line recording;
    if (mixer_record_line(&job,&recording)) { int e=mixer_line_save(job.fd,&recording); if (!error) error=e; }
    memcpy(record,&job,n); return error;
}
#ifdef __i386__
#include <ciuki/audio.h>
#endif
static int mixer_apply(struct desktop *d, uint32_t left, uint32_t right, uint32_t mute)
{
    if (d->power.state) return -EBUSY;
    if (d->mixer_fd<0) return -ENODEV;
    if (!settings_writer_started) return -EIO;
    struct mixer_job job=mixer_pending;
    job.fd=d->mixer_fd; job.mask|=1; job.save_only=0;
    job.master=(struct ciuki_mixer_master){.version=1,.size=sizeof(job.master),.left=left,.right=right,.mute=mute};
    return mixer_submit(&job);
}
static int mixer_line_apply(struct desktop *d,const struct ciuki_mixer_line *line)
{
    if (d->power.state) return -EBUSY;
    if (d->mixer_fd<0 || !settings_writer_started) return -ENODEV;
    struct mixer_job job=mixer_pending; job.fd=d->mixer_fd;
    int error=mixer_pack_line(&job,line); if (error) return error;
    return mixer_submit(&job);
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
    if (read_file(d->sh.profile.settings[0] ? d->sh.profile.settings : CFG_FILE,CFG_TEXT_MAX,&text,&n)) return;   /* first boot: defaults */
    int rejected=cfg_parse(&d->sh.cfg,(const char *)text,n);
    free(text);
    if (rejected) snprintf(d->notice,sizeof(d->notice),"desktop.cfg: %d line(s) ignored",rejected);
    (void)pp_load(&d->sh.power_cfg,d->sh.cfg.extra);   /* G11: the power_* lines */
    d->sh.power_meter_hidden=!d->sh.power_cfg.show_meter;
    d->sh.power_disk_sent=CIUKI_POWER_UNKNOWN;   /* the first poll programs the drive timer, Never included */

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
    static struct ciuki_settings durable;
    desk_preferences_persistent(d,&durable);
    size_t n=cfg_format(&durable,text,sizeof(text));
    if (!n) { errno=ENOSPC; return -1; }
    if (!settings_writer_started) { errno=EIO; return -1; }
    int result=cfg_writer_try_submit(&settings_writer,text,n);
    if (result && errno==EAGAIN) { d->sh.want_save=1; return 0; }
    return result;
}
static int media_busy(void);
static void media_stop(void);
static void master_candidate_latest(struct desktop *d);
static int shutdown_wait(struct desktop *d)
{
    master_candidate_latest(d); /* fx-9b: a press after the last mixer poll too */
    if (master_candidate_save(d->mixer_fd)) return 0; /* fx-9b: the last key change first */
    if (media_busy()) return 0;
    if (desk_autoplay_busy(d)) return 0;
    if (desk_folder_busy(d)) return 0;
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
/* f2-92: what the 2D engine did in the last 2 s window, on every boot:
 * engine fills, copies (screen and offscreen-cache), writes (cache uploads),
 * requests that fell back to software, rows served from the caches, frames
 * and CPU-uploaded bytes. Every window with engine activity; otherwise at
 * most once a minute while frames are drawn (a seconds clock alone would
 * add a record every 2 s), silent when idle. */
static int accel_record_line(const struct desktop *d, char *line, size_t size, uint64_t now, uint64_t *quiet_ns)
{
    const struct desk_frame_stats *s=&d->sample;
    int engine=s->accel_fills || s->accel_copies || s->accel_writes || s->accel_fallback;
    if (!engine && (!s->frames || now-*quiet_ns<60000000000ull)) return 0;
    if (!engine) *quiet_ns=now;
    /* f2-96: who draws the pointer (hw, sw, or hiding: a hardware cursor
     * still to be hidden), busy retries and software fallbacks since boot. */
    const char *pointer=d->hw_cursor ? "hw" : d->hw_cursor_shown ? "hiding" : "sw";
    int n=snprintf(line,size,"[desktop] accel fills=%llu copies=%llu writes=%llu fallback=%llu cache_rows=%llu frames=%llu lfb_bytes=%llu caps=%u vram_rows=%u pointer=%s pointer_retries=%u pointer_fallbacks=%u\n",
        (unsigned long long)s->accel_fills,(unsigned long long)s->accel_copies,(unsigned long long)s->accel_writes,
        (unsigned long long)s->accel_fallback,(unsigned long long)s->cache_rows,(unsigned long long)s->frames,
        (unsigned long long)s->lfb_bytes,d->accel_caps,d->vram_end>d->vram_first ? d->vram_end-d->vram_first : 0u,
        pointer,d->hw_cursor_retries,d->hw_cursor_fallbacks);
    return n>0 && (size_t)n<size ? n : 0;
}
static void accel_record(struct desktop *d, uint64_t now)
{
    static uint64_t quiet_ns;
    char line[320]; int n=accel_record_line(d,line,sizeof(line),now,&quiet_ns);
    if (n) (void)write(STDERR_FILENO,line,(size_t)n);
}
static void performance_sample(struct desktop *d, uint64_t now)
{
    if (!d->sample_begin_ns) { d->sample_begin_ns=now; return; }
    if (now-d->sample_begin_ns<2000000000ull) return;
    desk_stats_sample(d); d->sample_ns=now-d->sample_begin_ns;
    d->sample_begin_ns=now;
    accel_record(d,now);
    { char line[160]; int n=loop_record_line(line,sizeof(line),now,d->sample_ns); if (n) (void)write(STDERR_FILENO,line,(size_t)n); }
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
    struct ciuki_framebuffer_info fb;
    if (ciuki_display_info(d->display,&fb) || fb.width!=info.width || fb.height!=info.height) return;
    /* Full-screen return changes the lease, usually with the same layout.
     * Learn its generation and damage it without allocating another surface. */
    if (info.width==d->width && info.height==d->height) {
        desk_accel_reset(d,fb.generation);
        d->display_counter=counter; d->display_mode=mode;
        d->lfb_bytes_per_pixel=(info.bpp+7)/8; d->display_pending=mode;
        return;
    }
    int output = ciuki_surface_create(info.width, info.height, CIUKI_SURFACE_XRGB8888);
    uint32_t *pixels = output < 0 ? MAP_FAILED : ciuki_surface_map(output, PROT_READ | PROT_WRITE);
    if (pixels == MAP_FAILED) { if (output >= 0) close(output); return; }
    munmap(d->pixels, ((size_t)d->width * d->height * 4 + CIUKI_PAGE_SIZE - 1) & ~(size_t)(CIUKI_PAGE_SIZE - 1));
    close(d->output);
    d->pixels = pixels; d->output = output;
    desk_display_relayout(d, info.width, info.height);
    desk_accel_reset(d,fb.generation);
    if (d->shell) wallpaper_update(d);
    d->display_counter = counter;
    d->display_mode = mode;
    d->lfb_bytes_per_pixel=(info.bpp+7)/8;
    /* Includes rollback. The frame classifier preserves preferences for a
     * Control Panel preview; ordinary completed mode changes remain durable. */
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
/* f2-90: the wallpaper is in memory before the first frame. The build-seeded
 * native frame is read synchronously here; on any miss the solid colour stays
 * (desk_shell_wallpaper_update ran in wallpaper_load) and the worker decodes
 * after the first frame as before. */
static void wallpaper_preload(struct desktop *d)
{
    if (!d->shell || d->sh.gate || !d->sh.wallpaper_pending || wallpaper_worker_failed) return;
    uint32_t *pixels=NULL;
    if (desk_wall_preload(&wallpaper_request,DESK_WALL_CACHE,&pixels,startup_stage) || !pixels) return;
    if (d->scanout_valid) { free(pixels); return; }
    free(d->sh.wall); free(d->sh.background); d->sh.background=NULL;
    d->sh.wall=pixels; d->sh.wall_w=d->width; d->sh.wall_h=d->height;
    d->sh.wall_valid=1; d->sh.background_valid=0; d->sh.image_failed=0; d->sh.wallpaper_pending=0;
    startup_stage("wallpaper_applied");
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
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
    wallpaper_worker.notify=d->wake; wallpaper_worker.notify_ctx=d->wake_ctx;
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
        wallpaper_worker.notify=d->wake; wallpaper_worker.notify_ctx=d->wake_ctx;
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
/* Prepare observers before the first gate snapshot. Polls retain descriptors:
 * an open/query/close sequence changes the process ledger during the query. */
static int observer_open(int *fd, const char *path)
{
    if (*fd<0) *fd=open(path,O_RDONLY|O_CLOEXEC);
    return *fd;
}
static void observer_close(int *fd)
{
    if (*fd>=0) { close(*fd); *fd=-1; }
}
static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    struct ciuki_media_state state;
    struct ciuki_media_volume volumes[26];
    struct ciuki_media_cd_state cd_states[4];
    unsigned requested, completed;
    int started, failed, pending, busy, ready, stopping, observed, ok;
    int blocking; /* completion wake installed: the UI takes the short mailbox lock, else trylock */
    int fd;       /* fx-26: the worker's /dev/media description, polled by the service loop (-1 none) */
    unsigned commit;
    int confirm_pending, rescan, choice, always;
    struct ciuki_media_volume confirmation;
} media_worker = {.lock=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER, .fd=-1};
/* With a completion wake the mailbox lock is short (device queries run outside it) and a
 * failed trylock would strand a result nothing wakes the loop to collect; without one
 * (host harnesses) a contended mailbox is deferred, never waited on. */
static int media_lock(void)
{
    if (media_worker.blocking) { pthread_mutex_lock(&media_worker.lock); return 0; }
    return pthread_mutex_trylock(&media_worker.lock);
}
static void observers_prepare(struct desktop *d)
{
    if (!media_worker.started) observer_open(&d->media_fd,"/dev/media");
    observer_open(&d->network_fd,"/dev/net");
    if (d->mixer_fd<0) observer_open(&d->mixer_observer_fd,"/dev/mixer");
}
static void observers_close(struct desktop *d)
{
    media_stop();
    observer_close(&d->media_fd);
    observer_close(&d->network_fd);
    observer_close(&d->mixer_observer_fd);
}
static void network_read(struct desktop *d)
{
    if (d->power.state && d->power.prepared) return;
    int fd = observer_open(&d->network_fd,"/dev/net");
    struct ciuki_net_state state = { .version = CIUKI_NET_VERSION, .size = sizeof(state) };
    if (fd >= 0) {
        if (ciuki_devctl(fd, NET_GET_STATE, &state, sizeof(state)) < 0) {
            state = (struct ciuki_net_state){ .version = CIUKI_NET_VERSION, .size = sizeof(state) };
            observer_close(&d->network_fd);
        }
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
/* One bounded snapshot mailbox. The worker owns its descriptor and never
 * touches desktop/shell state. Device queries are outside the mailbox lock. */
static void *media_run(void *arg)
{
    struct desktop *d=arg; int fd=-1;
    for (;;) {
        pthread_mutex_lock(&media_worker.lock);
        while (!media_worker.pending && !media_worker.stopping)
            pthread_cond_wait(&media_worker.changed,&media_worker.lock);
        if (media_worker.stopping) { pthread_mutex_unlock(&media_worker.lock); break; }
        unsigned serial=media_worker.requested;
        media_worker.pending=0; media_worker.busy=1;
        pthread_mutex_unlock(&media_worker.lock);
        struct ciuki_media_state state={.version=CIUKI_MEDIA_VERSION,.size=sizeof(state)};
        struct ciuki_media_volume volumes[26]={0};
        struct ciuki_media_cd_state cd_states[4]={0};
        observer_open(&fd,"/dev/media");
        __atomic_store_n(&media_worker.fd,fd,__ATOMIC_RELEASE);
        int observed=fd>=0 && !ciuki_devctl(fd,MEDIA_GET_STATE,&state,sizeof(state));
        int ok=observed;
        for (unsigned i=3;ok && i<26;i++) {
            volumes[i]=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION,.size=sizeof(volumes[i]),.drive=i};
            if (ciuki_devctl(fd,MEDIA_GET_VOLUME,&volumes[i],sizeof(volumes[i]))) ok=0;
        }
        for (unsigned i=0;ok && i<4;i++) {
            cd_states[i]=(struct ciuki_media_cd_state){.version=1,.size=sizeof(cd_states[i]),.slot=i};
            if (ciuki_devctl(fd,MEDIA_CD_STATE,&cd_states[i],sizeof(cd_states[i]))) ok=0;
        }
        if (!ok) { __atomic_store_n(&media_worker.fd,-1,__ATOMIC_RELEASE); observer_close(&fd); }
        pthread_mutex_lock(&media_worker.lock);
        media_worker.state=state; memcpy(media_worker.volumes,volumes,sizeof(volumes));
        memcpy(media_worker.cd_states,cd_states,sizeof(cd_states));
        media_worker.observed=observed; media_worker.ok=ok;
        media_worker.completed=serial; media_worker.ready=1; media_worker.busy=0;
        desk_wake(d);
        pthread_mutex_unlock(&media_worker.lock);
    }
    __atomic_store_n(&media_worker.fd,-1,__ATOMIC_RELEASE);
    observer_close(&fd);
    return NULL;
}
static int media_read(struct desktop *d)
{
    if (d->power.state) return 0;
    if (!media_worker.started && !media_worker.failed) {
        media_worker.stopping=media_worker.pending=media_worker.busy=media_worker.ready=0;
        media_worker.commit=0; media_worker.confirm_pending=0; media_worker.blocking=d->wake!=NULL;
        int error=pthread_create(&media_worker.thread,NULL,media_run,d);
        if (error) { media_worker.failed=1; desk_shell_notify_error(d,"Media worker unavailable",error); }
        else { media_worker.started=1; observer_close(&d->media_fd); }
    }
    if (!media_worker.started) return 0;
    /* Polling and submissions never wait for a busy mailbox. */
    if (media_lock()) return 0;
    if (media_worker.rescan || (!media_worker.pending && !media_worker.busy && !media_worker.ready)) {
        media_worker.rescan=0; media_worker.ready=0;
        media_worker.requested++; media_worker.pending=1;
        pthread_cond_signal(&media_worker.changed);
    }
    pthread_mutex_unlock(&media_worker.lock);
    return 1;
}
static void media_poll(struct desktop *d)
{
    if (d->power.state || !media_worker.started || media_lock()) return;
    int ready=media_worker.ready, observed=media_worker.observed, ok=media_worker.ok;
    unsigned serial=media_worker.completed;
    if (serial < media_worker.requested) ready=0;
    struct ciuki_media_state state=media_worker.state;
    struct ciuki_media_volume volumes[26];
    struct ciuki_media_cd_state cd_states[4];
    if (ready) { memcpy(volumes,media_worker.volumes,sizeof(volumes)); memcpy(cd_states,media_worker.cd_states,sizeof(cd_states)); media_worker.ready=0; }
    pthread_mutex_unlock(&media_worker.lock);
    if (!ready) return;
    /* An unavailable observation never fabricates an eject. */
    if (observed) desk_shell_media_update(d,&state);
    if (ok) { desk_shell_media_labels(d,volumes); desk_autoplay_update(d,volumes); desk_autoplay_cd_update(d,cd_states); }
    if (media_worker.commit && serial>=media_worker.commit) {
        media_worker.commit=0; media_worker.confirm_pending=0;
        struct desk_autoplay *a=&d->sh.autoplay;
        const struct ciuki_media_volume *v=&media_worker.confirmation;
        int same=a->open && a->active.drive==v->drive && a->active.generation==v->generation && !strcmp(a->active.id,v->id);
        if (same && ok) {
            a->selected=media_worker.choice; a->always=media_worker.always; a->want_commit=1;
        } else if (same) desk_shell_message(d,"AutoPlay","The media could not be checked. Please try again.",1);
    }
}
static int media_busy(void)
{
    if (!media_worker.started) return 0;
    if (media_lock()) return 1;
    int busy=media_worker.busy || media_worker.pending;
    pthread_mutex_unlock(&media_worker.lock);
    return busy;
}
static void media_stop(void)
{
    if (!media_worker.started) return;
    pthread_mutex_lock(&media_worker.lock);
    media_worker.stopping=1; pthread_cond_signal(&media_worker.changed);
    pthread_mutex_unlock(&media_worker.lock);
    pthread_join(media_worker.thread,NULL);
    media_worker.started=0;
}
static void media_remove(struct desktop *d)
{
    struct ciuki_media_remove r={CIUKI_MEDIA_VERSION,sizeof(r),(uint32_t)d->sh.want_remove,d->sh.remove_generation};
    int optical=r.drive<26 && d->sh.media.type[r.drive]==CIUKI_MEDIA_OPTICAL;
    d->sh.want_remove=0;
    if (d->power.state && d->power.prepared) return;
    int fd=observer_open(&d->media_fd,"/dev/media"), e=fd<0 ? errno : 0;
    struct ciuki_media_blockers blockers={.version=CIUKI_MEDIA_VERSION,.size=sizeof(blockers),.drive=r.drive,.generation=r.generation};
    char reason[320]; desk_media_blocker_text(reason,sizeof(reason),NULL,desk_media_notice_body(d->width,d->height));
    if (fd>=0) {
        if (ciuki_devctl(fd,MEDIA_SAFE_REMOVE,&r,sizeof(r))) e=errno;
        if (e==EBUSY && !ciuki_devctl(fd,MEDIA_GET_BLOCKERS,&blockers,sizeof(blockers)))
            desk_media_blocker_text(reason,sizeof(reason),&blockers,desk_media_notice_body(d->width,d->height));
        if (e && e!=EBUSY) observer_close(&d->media_fd);
    }
    /* The successful command already proves this generation was detached.
     * Apply that fact before the success notice; its later worker observation
     * must not replace the notice with a duplicate detach notification. */
    if (!e && r.drive<26 && d->sh.media.generation[r.drive]==r.generation) {
        struct ciuki_media_state state=d->sh.media;
        state.removable_mask&=~(1u<<r.drive);
        state.type[r.drive]=optical ? 0 : CIUKI_MEDIA_SAFE_REMOVED;
        desk_shell_media_update(d,&state);
    }
    observer_close(&d->media_fd);
    d->now_ns=monotonic();
    media_worker.rescan=1; media_read(d);
    d->now_ns=monotonic();
    if (e==EBUSY) desk_shell_notify(d,"Drive is in use",reason,DU_DLG_ICON_WARNING,SH_NOTIFY_DETAILS);
    else if (e) desk_shell_notify_error(d,"Media removal failed",e);
    else desk_shell_notify(d,"Removable media",optical ? "The disc has been ejected." : "It is safe to unplug the USB device.",DU_DLG_ICON_INFO,SH_NOTIFY_MEDIA);
}
/* Queue the pending key-applied master (fx-9b); 1 while it is not saved. */
static int master_candidate_save(int fd)
{
    if (ec_candidate<0 || ec_candidate==last_ec_preference) return 0;
    if (!settings_writer_started) return 0;
    if (cfg_writer_busy(&settings_writer)) return 1;
    struct mixer_job job=mixer_pending; job.fd=fd; job.master=ec_candidate_master; job.save_only=1; job.mask|=1;
    if (!mixer_submit(&job)) last_ec_preference=ec_candidate;
    return 1;
}
/* fx-9b: the kernel's current key-applied master becomes the candidate at
 * once (shutdown: no quiet period is left to wait for). */
static void master_candidate_latest(struct desktop *d)
{
    if (d->mixer_fd<0) return;
    struct ciuki_mixer_master m={.version=CIUKI_AUDIO_VERSION,.size=sizeof(m)};
    if (ciuki_devctl(d->mixer_fd,MIXER_GET_MASTER,&m,sizeof(m)) || m.save_state!=CIUKI_MIXER_UNSAVED || m.save_error) return;
    int32_t packed=ciuki_master_pack(m.left,m.right,m.mute);
    if (packed!=ec_candidate && packed!=last_ec_preference) { ec_candidate=packed; ec_candidate_ns=d->now_ns; ec_candidate_master=m; }
}
static void mixer_read(struct desktop *d)
{
    if (d->power.state && d->power.prepared) return;
    int old_volume=d->sh.volume, old_muted=d->sh.muted, old_state=d->sh.mixer_state;
    int fd=d->mixer_fd;
    if (fd<0) fd=observer_open(&d->mixer_observer_fd,"/dev/mixer"); /* public, read-only */
    struct ciuki_mixer_master m={.version=CIUKI_AUDIO_VERSION,.size=sizeof(m)};
    /* f1-100: no periodic hardware (EC) reads; the popup's open (want_reload
     * bit 8) asks the kernel for one on the control handle, the plain read
     * follows otherwise. */
    int refreshed=(d->sh.want_reload&8) && d->mixer_fd>=0 && fd==d->mixer_fd &&
        !ciuki_devctl(fd,MIXER_REFRESH_MASTER,&m,sizeof(m));
    if (fd<0 || (!refreshed && ciuki_devctl(fd,MIXER_GET_MASTER,&m,sizeof(m))<0)) {
        d->sh.mixer_state=-1;
        observer_close(&d->mixer_observer_fd);
        /* fx-26: a description that cannot be read would stay readable:
         * back to the 250 ms observation rather than a busy loop. */
        if (fd>=0 && mixer_events>0) mixer_events=0;
    }
    else {
        d->sh.mixer_state=d->mixer_fd>=0 ? 2 : 1;
        if (m.save_state!=CIUKI_MIXER_UNSAVED || m.save_error) last_ec_preference=ec_candidate=-1;
        if (!d->sh.volume_drag && !d->sh.want_mixer) {
            d->sh.volume=(int)((m.left+m.right)/2); d->sh.muted=m.mute!=0;
            int32_t packed=ciuki_master_pack(m.left,m.right,m.mute);
            if (m.save_state==CIUKI_MIXER_UNSAVED && !m.save_error && packed!=last_ec_preference) {
                if (packed!=ec_candidate) { ec_candidate=packed; ec_candidate_ns=d->now_ns; ec_candidate_master=m; }
                else if (d->now_ns-ec_candidate_ns>=MASTER_QUIET_NS && !d->power.state) master_candidate_save(fd);
            }
        }
    }
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
    }
}
/* fx-26: the kernel reports master changes through poll() on the mixer
 * description (POLLIN until it is read again; Windows 98: the mixer driver's
 * MM_MIXM_CONTROL_CHANGE to the tray's mixerOpen callback window), so a key,
 * a refresh or another program's change is read on the loop turn it wakes
 * and drawn on the next frame. -1 not yet known, 1 reported, 0 the kernel
 * refuses (POLLNVAL): the 250 ms observation remains the fallback. With
 * events the only deadline left is the quiet-period save of a key-applied
 * master (MASTER_QUIET_NS after the last change, retried every 250 ms while
 * the settings writer is busy). */
static int mixer_watch_fd(const struct desktop *d)
{
    if (!d->shell || d->sh.gate || d->power.state || !mixer_events) return -1;
    return d->mixer_fd>=0 ? d->mixer_fd : d->mixer_observer_fd;
}
static void mixer_poll(struct desktop *d,uint64_t now,uint64_t *next)
{
    if (!d->shell || d->sh.gate || d->power.state) return;
    if (mixer_events>0) {
        if (ec_candidate<0 || ec_candidate==last_ec_preference) { *next=UINT64_MAX; return; }
        uint64_t due=ec_candidate_ns+MASTER_QUIET_NS;
        if (*next==UINT64_MAX || *next<due) *next=due;
        if (now>=*next) { *next=now+250000000ull; d->sh.want_reload|=4; }
        return;
    }
    if (now>=*next) {
        *next=now+250000000ull;
        d->sh.want_reload|=4;
    }
}
/* fx-26: the media worker's description is readable after a drive was
 * published or withdrawn (WM_DEVICECHANGE); polled only while the worker is
 * idle, so a pending query never spins the loop. */
static int media_events=-1; /* as mixer_events: 0 after a POLLNVAL (no kernel readiness) */
static int media_watch_fd(const struct desktop *d)
{
    if (!d->shell || d->sh.gate || d->power.state || !media_worker.started || !media_events) return -1;
    if (pthread_mutex_trylock(&media_worker.lock)) return -1;
    int idle=!media_worker.pending && !media_worker.busy && !media_worker.ready;
    pthread_mutex_unlock(&media_worker.lock);
    return idle ? __atomic_load_n(&media_worker.fd,__ATOMIC_ACQUIRE) : -1;
}
/* After poll(): what the two watch entries reported. */
static void watch_result(struct desktop *d,int mixer_revents,int media_revents)
{
    if (mixer_revents>=0) {
        if (mixer_revents&POLLNVAL) mixer_events=0;
        else { if (mixer_events<0) mixer_events=1; if (mixer_revents&POLLIN) d->sh.want_reload|=4; }
    }
    if (media_revents>=0) {
        if (media_revents&POLLNVAL) media_events=0;
        else if (media_revents&POLLIN) { media_events=1; media_worker.rescan=1; d->sh.want_reload|=32; }
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
/* f1-97: one-line 2D acceleration answer at the first desktop frame, from the
 * kernel's `2D accelerator` device record (stable accelerator id; reason = the
 * quarantine token). A shell notice card, never a dialog: it expires after
 * 10 s and any click dismisses it. f2-92: "YES" only once the engine has
 * drawn for a presenter (record reason "accelerated"); before that the
 * engine is "OK (drawing: software)". */
static __attribute__((unused)) void accel_notice_text(char *out, size_t size, int accelerated, int used, const char *reason, const char *name)
{
    if (accelerated && used) snprintf(out,size,"2D acceleration: YES \xE2\x80\x94 %s",name && *name ? name : "Native driver");
    else if (accelerated) snprintf(out,size,"2D engine: OK (drawing: software)");
    else snprintf(out,size,"2D engine: NO (%s)",reason && *reason ? reason : "unknown");
}
/* The activation is asynchronous and usually decides after the first frame:
 * poll the inventory twice a second for up to 60 s; an engine that is ready
 * gets 3 s to draw before it is reported as unused. */
static struct { unsigned failures; uint64_t first_ns, next_ns, ready_ns; } accel_notice_state;
/* Returns 1 when finished (shown, nothing recorded, or given up), 0 to retry. */
static int accel_notice(struct desktop *d)
{
#ifdef __i386__
    static struct ciuki_devices_info info;
    if (!accel_notice_state.first_ns) accel_notice_state.first_ns=d->now_ns ? d->now_ns : 1;
    if (d->now_ns<accel_notice_state.next_ns) return 0;
    accel_notice_state.next_ns=d->now_ns+500000000ull;
    info.version=CIUKI_DEVICES_VERSION; info.size=sizeof(info);
    if (ciuki_devices(&info)) return ++accel_notice_state.failures>=3;
    for (unsigned i=0;i<info.count && i<CIUKI_DEVICES_MAX;i++) {
        const struct ciuki_device_record *v=&info.devices[i];
        if (v->id!=CIUKI_DEVICE_ACCELERATOR_ID) continue;
        if (!strcmp(v->reason,"pending")) return d->now_ns-accel_notice_state.first_ns>=60000000000ull;
        int ready=v->accel_ready, used=ready && v->native_used;
        if (ready && !used) {
            if (!accel_notice_state.ready_ns) accel_notice_state.ready_ns=d->now_ns ? d->now_ns : 1;
            if (d->now_ns-accel_notice_state.ready_ns<3000000000ull) return 0;
        }
        char text[160];
        accel_notice_text(text,sizeof(text),ready,used,v->reason,v->driver_display_name);
        desk_shell_notify(d,"Graphics",text,DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS);
        d->sh.notification.expires=d->now_ns+10000000000ull;
        return 1;
    }
    return d->now_ns-accel_notice_state.first_ns>=60000000000ull;
#else
    (void)d; return 1;
#endif
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
static int run_submit(struct desktop *,const char *);
static void links_service(struct desktop *);
static int run_history_idle(void);
static const char *run_environment(void *ctx,const char *name)
{
    (void)ctx;
    extern char **environ;
    size_t n=strlen(name);
    for (char **p=environ;p && *p;p++) if (!strncasecmp(*p,name,n) && (*p)[n]=='=') return *p+n+1;
    if (!strcasecmp(name,"windir") || !strcasecmp(name,"SystemRoot")) return "C:\\WINDOWS";
    if (!strcasecmp(name,"COMSPEC")) return "C:\\DOS\\COMMAND.COM";
    return NULL;
}
static __attribute__((noinline)) int run_command(struct desktop *d,char *line)
{
    desk_run_trim(line);
    char expanded[DESK_RUN_MAX],target[260],arguments[DESK_RUN_MAX];
    int error=desk_run_expand(line,expanded,sizeof(expanded),run_environment,NULL);
    if (!error) error=desk_run_split(expanded,target,arguments);
    if (error) { desk_run_result(d,line,error); return error; }
    char native[FM_PATH];
    if (target[0] && target[1]==':' && !fm_posix_path(native,target) && strlen(native)<sizeof(target)) strcpy(target,native);
    const char *name=!strncasecmp(target,"/Programs/",10) ? target+10 : target;
    for (unsigned k=0;k<d->program_count;k++) if (!strcasecmp(d->programs[k].name,name)) {
        name=d->programs[k].name;
        char *args[6]; int count=fm_launch_arguments(arguments,args,6);
        if (count<0) { desk_run_result(d,line,count); return count; }
        desk_launch_origin(d,DESK_LAUNCH_RUN,name);
        snprintf(d->launch_request.run_command,sizeof(d->launch_request.run_command),"%s",line);
        int pid=count ? desk_task_launch_args(d,name,(const char *const *)args,(unsigned)count) : desk_task_launch(d,name,NULL);
        if (pid>0) desk_run_remember(d,line);
        if (pid) desk_run_result(d,line,pid<0 ? pid : 0);
        if (!pid) snprintf(d->notice,sizeof(d->notice),"Starting %.32s",name);
        return pid;
    }
    int result=run_submit(d,line);
    if (result<0) desk_run_result(d,line,result);
    return result;
}
/* Shared by --launch and the Programs menu: a bare launch carries no payload. */
static void startup_launch(struct desktop *d,const char *name)
{
    d->action=-1;
    for (unsigned i=0;i<d->program_count;i++) if (!strcmp(d->programs[i].name,name)) d->action=(int)i;
    if (d->action<0) snprintf(d->notice,sizeof(d->notice),"%.40s is not in /Programs",name);
}
/* f3-61 f3:vm-game: one Programs > Games entry launched at startup through
 * the menu's own path, optionally asserting --dos-kernel=unified. */
static int startup_game_index=-1;
static const char *startup_game_kernel;
static int launch_pending(struct desktop *d)
{
    if (d->action<0 && startup_game_index>=0) {
        int index=startup_game_index; startup_game_index=-1;
        return desk_launch_game(d,index,startup_game_kernel);
    }
    if (d->action<0) return desk_autoplay_launch(d);
    unsigned selected=(unsigned)d->action; d->action=-1;
    int pid=selected<d->program_count ? desk_task_launch(d,d->programs[selected].name,NULL) : -ENOENT;
    if (pid<0) {
        desk_shell_notify_error(d,"Program did not start",-pid);
    } else if (d->shell_test && d->shell_stage==1 && !strcmp(d->programs[selected].name,"demo")) {
        d->shell_pid=(uint32_t)pid; d->shell_stage=2;
    } else if (d->tasks_test && d->tasks_stage==0 && !strcmp(d->programs[selected].name,"tasks")) {
        d->tasks_pid=(uint32_t)pid; d->tasks_stage=1;
    } else if (d->app_test && !d->app_pid && !strcmp(d->programs[selected].name,desk_app_program(d->app_test))) {
        d->app_pid=(uint32_t)pid;
    }
    return pid;
}
/* Strong native bindings are linked by build_desktop.py; host shell replays
 * keep their existing isolated platform. */
/* G30: the scheduled-tasks service is linked by build_desktop.py (schedsvc.c); host replays run without it. */
__attribute__((weak)) int desk_schedsvc_start(struct desktop *d) { (void)d; return 0; }
__attribute__((weak)) void desk_schedsvc_tick(struct desktop *d) { (void)d; }
__attribute__((weak)) void desk_resources_poll(struct desktop *d) { (void)d; }
__attribute__((weak)) int desk_v3_fixture_active(void) { return 0; }
__attribute__((weak)) int desk_v3_fixture_begin(struct desktop *d) { (void)d; return -ENOSYS; }
__attribute__((weak)) void desk_v3_fixture_tick(struct desktop *d) { (void)d; }
__attribute__((weak)) void desk_dialogs_fixture_tick(struct desktop *d) { (void)d; }
__attribute__((weak)) int desk_dialogs_fixture_active(void) { return 0; }
__attribute__((weak)) int desk_dialogs_fixture_begin(struct desktop *d) { (void)d; return -ENOSYS; }
__attribute__((weak)) void desk_upper_fixture_tick(struct desktop *d) { (void)d; }
__attribute__((weak)) int desk_upper_fixture_active(void) { return 0; }
__attribute__((weak)) int desk_upper_fixture_begin(struct desktop *d) { (void)d; return -ENOSYS; }
__attribute__((weak)) int desk_media_fixture_begin(struct desktop *d) { (void)d; return -ENOSYS; }
__attribute__((weak)) int desk_w32_fixture_active(void) { return 0; }
__attribute__((weak)) int desk_w32_fixture_begin(struct desktop *d,const char *name) { (void)d; (void)name; return -ENOSYS; }
__attribute__((weak)) void desk_w32_fixture_tick(struct desktop *d) { (void)d; }
void desk_v3_fixture_report(const char *line)
{ (void)ciuki_raw_probe_report(CU_PTR(line),(uint32_t)strlen(line),0,0,0,0); }
__attribute__((weak)) void desk_w32_start(struct desktop *d) { (void)d; }
__attribute__((weak)) void desk_w32_poll(struct desktop *d) { (void)d; }
__attribute__((weak)) void desk_w32_child_exited(struct desktop *d,uint32_t pid,int status)
{ (void)d; (void)pid; (void)status; }
__attribute__((weak)) int desk_w32_shutdown(void) { return 1; }
static int power_request(int action)
{
    if (!run_history_idle()) { errno=EBUSY; return -1; }
    int ready=desk_w32_shutdown();
    if(ready<=0){errno=ready<0 ? -ready : EBUSY;return -1;}
    return ciuki_power(action==1 ? CIUKI_POWER_REBOOT : CIUKI_POWER_OFF);
}
static int power_state(void) { return ciuki_power(CIUKI_POWER_QUERY); }
static int power_cancel_standby(void) { return ciuki_power(CIUKI_POWER_CANCEL_STANDBY); }
static int power_standby(void) { return ciuki_power(CIUKI_POWER_STANDBY); }   /* G11 */
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
    observers_close(d);
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
static void user_logoff(struct desktop *d)
{
    user_initial=0; user_switch_saving=user_default_loading=user_default_ready=0;
    desk_logon_open(d,1);
    d->sh.user_request.operation=USERS_READ; d->sh.user_want=1;
    d->sh.want_run[0]=0;
}
static void user_service(struct desktop *d)
{
    struct desk_shell *s=&d->sh;
    if (s->user_want && !user_worker.job.started) {
        int e=users_begin(&user_worker,&s->user_request,d->wake,d->wake_ctx);
        if (!e) { s->user_want=0; s->user_pending=1; users_erase(s->user_request.password,USERS_PASSWORD); }
        else { s->user_want=0; desk_logon_result(d,&s->users,e); }
    }
    struct users_view v; int e;
    if (users_poll(&user_worker,&v,&e)) {
        if (user_default_loading) {
            user_default_loading=0; user_initial=0; s->user_pending=0;
            if (e) { s->user_choose=0; desk_logon_result(d,&s->users,e); }
            else user_default_ready=1;
        } else if (user_initial) {
            user_initial=0; s->user_pending=0;
            if (!e) s->users=v;
            if (e || (v.flags&USERS_LOGON)) {
                desk_logon_open(d,1);
                if (e) desk_logon_result(d,&s->users,e);
            } else desk_logon_done(d);
        } else desk_logon_result(d,&v,e);
    }
    if (!s->user_choose) return;
    if (s->user_choose==2 && !user_default_ready) {
        if (!user_worker.job.started) {
            struct users_request q={.operation=USERS_DEFAULT};
            int e=users_begin(&user_worker,&q,d->wake,d->wake_ctx);
            if (e) { s->user_choose=0; desk_logon_result(d,&s->users,e); }
            else { user_default_loading=1; s->user_pending=1; }
        }
        return;
    }
    if (s->user_choose==1 && !s->users.authenticated) return;
    if (!user_switch_saving) {
        if (settings_save(d)) {
            s->user_choose=0;
            snprintf(s->user_status,sizeof(s->user_status),"Current settings could not be saved. Try again.");
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
            return;
        }
        user_switch_saving=1;
    }
    if (launcher_inflight() || (settings_writer_started && cfg_writer_busy(&settings_writer)) || desk_folder_busy(d)) return;
    /* The shortcut worker retains a complete snapshot until its current scan
     * is over; update its profile only at the next worker boundary. */
    struct user_profile old=s->profile;
    users_profile(&s->profile,s->user_choose==1 ? s->user_request.name : "",s->user_choose==1 ? s->users.authenticated : 0,s->users.flags);
    if (!links_profile_update(d)) { s->profile=old; return; }
    if (settings_writer_started) { cfg_writer_stop(&settings_writer); settings_writer_started=0; }
    int (*run_job)(void *,size_t)=settings_writer.run_job;
    memset(&settings_writer,0,sizeof(settings_writer));
    settings_writer.notify=d->wake; settings_writer.notify_ctx=d->wake_ctx; settings_writer.run_job=run_job;
    desk_folder_destroy(d);
    settings_reload_pending=settings_reload_request=settings_reload_wallpaper=theme_read_pending=0;
    s->cfg=user_worker.settings;
    d->pictures_exists=!!s->profile.root[0] || d->pictures_exists;
    s->icon_count=0; desk_shell_apply(d);
    int error=d->autoplay_fs ? desk_folder_init_async(d,d->autoplay_fs) : -ENOSYS;
    settings_writer_started=!cfg_writer_start(&settings_writer,s->profile.settings,s->profile.temporary);
    wallpaper_load(d); wallpaper_update(d);
    d->v3_links_force|=DESK_LINKS_SCAN;
    desk_logon_done(d); user_switch_saving=user_default_ready=user_initial=0;
    if (error) desk_shell_notify_error(d,"Desktop folder unavailable",error);
    if (!settings_writer_started) desk_shell_notify_error(d,"Profile settings worker unavailable",EIO);
    desk_sound(d,SOUND_LOGON);
}
/* Serves the requests that shell.c raised while handling input. */
static void input_settings_apply(struct desktop *d)
{
    static int delay=-1,speed=-1;
    if(delay==d->sh.cfg.repeat_delay && speed==d->sh.cfg.repeat_speed)return;
    struct ciuki_keyboard_settings s={1,sizeof(s),(unsigned)d->sh.cfg.repeat_delay,(unsigned)d->sh.cfg.repeat_speed};
    if(ciuki_devctl(d->input,INPUT_KEYBOARD_SET,&s,sizeof(s))) {
        snprintf(d->notice,sizeof(d->notice),"Keyboard settings: error %d",errno);
    }
    delay=d->sh.cfg.repeat_delay;speed=d->sh.cfg.repeat_speed;
}
static void shell_service(struct desktop *d)
{
    struct desk_shell *sh=&d->sh;
    if (!d->shell) return;
    if (d->now_ns >= sh->power_poll_ns) {
        sh->power_poll_ns = d->now_ns + 1000000000ull;
        struct ciuki_power_status status;
        if (!ciuki_power_status(&status)) (void)desk_shell_power_update(d,&status);
        /* G11: cached EVENTS and QUERY (no firmware call), then the drive
         * timer of the current power source when it changed. */
        struct ciuki_power_events events;
        int events_ok=!ciuki_power_events(&events), query=ciuki_power(CIUKI_POWER_QUERY);
        desk_shell_power_events(d,events_ok ? &events : NULL,query);
        uint32_t disks=pp_timer(&sh->power_cfg,&sh->power_summary,PP_DISKS);
        if (disks!=sh->power_disk_sent && !sh->gate) {
            struct ciuki_power_policy policy={.disk_seconds=disks};
            int r=ciuki_power_policy(&policy);
            sh->power_disk_sent=disks;   /* sent once per value; refusals are kernel records */
            fprintf(stderr,"[desktop] power policy disk_seconds=%u result=%d\n",disks,r ? -errno : 0);
        }
    }
    desktop_wait("mixer-result");
    struct mixer_job job; size_t job_size; int job_error;
    if (settings_writer_started && cfg_writer_job_result(&settings_writer,&job,&job_size,&job_error)) {
        if (job_size!=sizeof(job)) job_error=EIO;
        if ((job.mask & 1) && !job_error && job.master.save_state==CIUKI_MIXER_SAVED)
            last_ec_preference=ciuki_master_pack(job.master.left,job.master.right,job.master.mute);
        if (job_error) desk_shell_notify_error(d,"Volume change failed",job_error);
        else if ((job.mask & 1) && job.master.save_state==CIUKI_MIXER_UNSAVED)
            desk_shell_notify_error(d,"Volume applied but not saved",(int)job.master.save_error);
        if (!job_error && (!job.save_only || (job.mask & ~1u))) desk_sound(d,SOUND_VOLUME);
        if (job.serial==mixer_serial) mixer_pending.mask=0;
        if (job.serial == mixer_serial && !sh->volume_drag && !sh->want_mixer) sh->want_reload|=4;
    }
    int save_error;
    desktop_wait("settings-save-result");
    if (settings_writer_started && cfg_writer_result(&settings_writer,&save_error) && save_error) {
        desk_shell_notify_error(d,"Settings not saved",save_error);
        if (sh->user_gate && user_switch_saving) {
            sh->user_choose=0; user_switch_saving=0;
            snprintf(sh->user_status,sizeof(sh->user_status),"Current settings could not be saved. Try again.");
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
        }
    }
    if (sh->gate) { sh->want_save=sh->want_mixer=sh->want_power=sh->want_power_force=sh->want_standby=0; sh->want_reload&=~1; sh->want_run[0]=0; }
    if (d->power.state) {
        links_service(d);
        if (run_history_idle()) desk_power_tick(d);
        return;
    }
    user_service(d);
    desktop_wait("folder-service"); desk_folder_service(d);
    if (sh->want_power) {
        int action=sh->want_power, force=sh->want_power_force; sh->want_power=sh->want_power_force=0;
        if (d->session.state) return;   /* G3: the programs are being asked already */
        if (settings_save(d)) { /* still try: settings are a convenience */ }
        desk_session_begin(d,action);   /* G3: ask every program; power only when all agreed */
        if (force && d->session.state) d->session.force=1;   /* G11 alarm "Force" */
        return;
    }
    if (sh->want_programs) {
        sh->want_programs=0;desktop_wait("program-scan");scan_programs(d);
    }
    if (sh->want_reload & 1) {
        settings_reload_pending=settings_reload_request=1;
        theme_read_pending=0;
        settings_reload_wallpaper|=sh->want_wallpaper;
        sh->want_wallpaper=0;
    }
    if (settings_reload_pending && !settings_writer_started) {
        settings_reload_pending=settings_reload_request=settings_reload_wallpaper=0;
        desk_shell_notify_error(d,"Settings worker unavailable",ENOSYS);
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
            desk_saver_config(d);
            desk_shell_power_config(d);   /* G11: Control Panel's power_* lines */

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
    if (sh->want_reload & 32) { desktop_wait("media-submit"); media_read(d); }
    if (sh->want_remove) { desktop_wait("media-remove"); media_remove(d); }
    desktop_wait("autoplay-check");
    if (sh->autoplay.want_commit) {
        sh->autoplay.want_commit=0; media_worker.confirm_pending=1; media_worker.commit=0;
        media_worker.confirmation=sh->autoplay.active;
        media_worker.choice=sh->autoplay.selected; media_worker.always=sh->autoplay.always;
    }
    if (media_worker.confirm_pending && !media_worker.commit) {
        if (media_read(d)) {
            /* Require a scan requested after the user's confirmation. */
            if (!pthread_mutex_trylock(&media_worker.lock)) {
                media_worker.commit=++media_worker.requested;
                media_worker.pending=1; pthread_cond_signal(&media_worker.changed);
                pthread_mutex_unlock(&media_worker.lock);
            }
        } else if (!media_worker.started) {
            media_worker.confirm_pending=0;
            desk_shell_message(d,"AutoPlay","The media could not be checked. Please try again.",1);
        }
    }
    desktop_wait("media-result");
    if (media_worker.rescan) media_read(d);
    media_poll(d);
    desktop_wait("autoplay-service"); desk_autoplay_service(d);
    desk_schedsvc_tick(d);
    desk_resources_poll(d);
    desktop_wait("autoplay-done");
    sh->want_reload=0;
    wallpaper_poll(d);
    if (!sh->wall_valid) wallpaper_update(d);
    if (sh->want_save) {
        desktop_wait("settings-save-submit");
        sh->want_save=0;
        if (settings_save(d)) {
            desk_shell_notify_error(d,"Settings not saved",errno);
        }
    }
    if (sh->want_mixer) { desktop_wait("mixer-submit"); sh->want_mixer=0; mixer_write(d); }
    if (sh->want_run[0]) {
        char line[sizeof(sh->want_run)];
        memcpy(line,sh->want_run,sizeof(line)); sh->want_run[0]=0;
        desktop_wait("run-command"); run_command(d,line);
    }
    if (!sh->gate) links_service(d); /* submit new Run/Browse work before sleeping */
    desktop_wait("shell-done");
}
__attribute__((weak)) int desk_startup_begin(int skip) { (void)skip; return -ENOSYS; }
/* Optional f5-07A/f5-08 adapters. Until integrated, operations fail explicitly. */
__attribute__((weak)) int ciuki_win32_shell_launch(const struct desk_launch_record *record)
{ (void)record; return -ENOSYS; }
__attribute__((weak)) int ciuki_win32_shell_restart(uint32_t pid, uint32_t flags)
{ (void)pid; (void)flags; return -ENOSYS; }
/* f5-08 supplies this transaction: switch/delegate, or synchronously revoke
 * and restore the desktop. The default cannot change the physical display. */
__attribute__((weak)) int ciuki_win32_presenter_lease(struct desktop *d, uint32_t pid, int acquire, int mode,
    uint32_t w, uint32_t h, uint32_t bpp, uint32_t generation)
{ (void)d; (void)pid; (void)acquire; (void)mode; (void)w; (void)h; (void)bpp; (void)generation; return -ENOSYS; }
__attribute__((weak)) void desk_w32_presenter_poll(struct desktop *d) { (void)d; }
static int win32_lease(struct desktop *d, uint32_t pid, int acquire,
                       uint32_t w, uint32_t h, uint32_t bpp, uint32_t generation)
{
    int mode=0;
    if (acquire) {
        struct ciuki_display_info info={.version=CIUKI_DISPLAY_VERSION,.size=sizeof(info)};
        int error=ciuki_display_query(&info);
        if (error) {
            fprintf(stderr,"[win32] display lease=query pid=%u serial=%u result=%d\n",pid,generation,error);
            return error; /* display calls return negative errno directly */
        }
        struct ciuki_present_surface source={.width=w,.height=h,
            .format=bpp==8 ? CIUKI_PRESENT_INDEX8 : bpp==16 ? CIUKI_PRESENT_RGB565 : CIUKI_PRESENT_XRGB8888};
        int index=ciuki_present_choose_mode(&info,&source);
        if(d->v3_lease_preferred_mode) {
            index=-ENODEV;
            for(unsigned k=0;k<info.mode_count && k<32;k++)
                if(info.modes[k].id==d->v3_lease_preferred_mode){index=(int)k;break;}
        }
        if (index<0) {
            fprintf(stderr,"[win32] display lease=mode pid=%u serial=%u result=%d\n",pid,generation,index);
            return index;
        }
        mode=info.modes[index].id;
        d->v3_lease_tuple.native_width=info.modes[index].width;
        d->v3_lease_tuple.native_height=info.modes[index].height;
        d->v3_lease_tuple.native_bpp=info.modes[index].bpp;
    }
    return ciuki_win32_presenter_lease(d,pid,acquire,mode,w,h,bpp,generation);
}
static int win32_restart(struct desktop *d, uint32_t pid, uint32_t flags)
{ (void)d; return ciuki_win32_shell_restart(pid,flags); }
static int link_read(struct desktop *d, const char *path, struct desk_launch_record *record)
{
    (void)d;
    FILE *f=fopen(path,"rb"); if (!f) return -errno;
    uint8_t *bytes=malloc(DESK_LINK_BYTES+1); if (!bytes) { fclose(f); return -ENOMEM; }
    size_t n=fread(bytes,1,DESK_LINK_BYTES+1,f); int bad=ferror(f);
    if (fclose(f)) bad=1;
    int result=bad || n>DESK_LINK_BYTES || fm_link_decode(bytes,(unsigned)n,path,record) ? -EINVAL : 0;
    free(bytes); return result;
}
/* f2-95: the shortcut scan walks the Start Menu, the Windows desktop, /Games
 * and /Programs. On the T23 trial of image a4d9454e the worker (desktop#30)
 * repeated that walk every 2 s with a stat() per entry, re-read every
 * shortcut and asked CIUKI_DOSVM_GAME_QUERY for each bundled game (each query
 * mounts the games image and walks a program tree in the kernel): 12-50 % of
 * the CPU in kernel mode, holding the block cache that netsurf and the
 * compositor then waited for ([sched] inherit ... to=desktop#30
 * reason=cache count=424). Now the entry type, size and time come from the
 * directory enumeration itself (CIUKI_DIRENT_METADATA, no allocation-chain
 * walk); an unchanged shortcut (same path, size and time) keeps its decoded
 * record; a scan runs on request (start, Programs menu, program change,
 * window close, Windows restart) or after DESK_LINKS_RESCAN_NS; the bundled
 * games are queried only at start and after a program change. */
#define DESK_LINKS_RESCAN_NS 10000000000ull
struct link_stamp { uint64_t size; int64_t mtime; };
static const char *links_roots[]={"/Windows/Start Menu/Programs","/Windows/Desktop","/Games","/Programs"};
static const unsigned links_depth[]={3,1,3,3}, links_budget[]={128,128,256,256}, links_on_desktop[]={0,1,0,0};
struct links_scan {
    struct desk_link_entry *out; struct link_stamp *stamps; unsigned cap, count, budget;
    const struct desk_link_entry *previous; const struct link_stamp *previous_stamps; unsigned previous_count;
    unsigned stats, reads;
    char programs[USERS_PATH];
};
/* 1 directory, 0 regular file, -1 anything else or gone. */
static int link_entry_kind(const char *file, const struct dirent *entry, struct link_stamp *stamp, struct links_scan *s)
{
    *stamp=(struct link_stamp){0,0};
#if defined(CIUKI_DIRENT_METADATA)
    if (entry->d_type==DT_DIR) return 1;
    if (entry->d_type==DT_REG) { stamp->size=entry->d_size; stamp->mtime=entry->d_mtime; return 0; }
    if (entry->d_type!=DT_UNKNOWN) return -1;
#else
    (void)entry; /* host builds: no enumeration metadata, stat as before */
#endif
    struct stat st; s->stats++;
    if (stat(file,&st)) return -1;
    if (S_ISDIR(st.st_mode)) return 1;
    if (!S_ISREG(st.st_mode)) return -1;
    stamp->size=(uint64_t)st.st_size; stamp->mtime=(int64_t)st.st_mtime;
    return 0;
}
static int link_reuse(const struct links_scan *s, const char *file, const struct link_stamp *stamp,
                      struct desk_launch_record *out)
{
    if (!stamp->mtime) return 0; /* no time: always read again */
    for (unsigned k=0;k<s->previous_count;k++)
        if (s->previous_stamps[k].size==stamp->size && s->previous_stamps[k].mtime==stamp->mtime &&
            !strcmp(s->previous[k].source,file)) { *out=s->previous[k].launch; return 1; }
    return 0;
}
static void links_directory(struct links_scan *s, const char *path, unsigned desktop, unsigned cap, unsigned depth)
{
    if (!depth || !s->budget || s->count>=cap) return;
    DIR *dir=opendir(path); if (!dir) return;
    struct dirent *entry;
    while (s->budget && s->count<cap && (entry=readdir(dir))) {
        s->budget--;
        if ((entry->d_name[0]=='.' && strcmp(entry->d_name,".ciuki")) || strchr(entry->d_name,'/') || strchr(entry->d_name,'\\')) continue;
        char file[260]; int n=snprintf(file,sizeof(file),"%s/%s",path,entry->d_name);
        if (n<0 || n>=(int)sizeof(file)) continue;
        struct link_stamp stamp; int kind=link_entry_kind(file,entry,&stamp,s);
        if (kind==1) { links_directory(s,file,desktop,cap,depth-1); continue; }
        if (kind || n<4 || (strcasecmp(file+n-4,".lnk") && (n<6 || strcasecmp(file+n-6,".ciuki")))) continue;
        /* Whole entries are cleared: snapshots compare by bytes. */
        struct desk_link_entry *e=&s->out[s->count]; memset(e,0,sizeof(*e));
        if (!link_reuse(s,file,&stamp,&e->launch)) { s->reads++; if (link_read(NULL,file,&e->launch)) continue; }
        snprintf(e->source,sizeof(e->source),"%s",file); e->desktop=desktop; s->stamps[s->count++]=stamp;
    }
    closedir(dir);
}
static int links_list(struct links_scan *s)
{
    s->count=0;
    if (s->programs[0] && strcmp(s->programs,links_roots[0])) {
        s->budget=128; links_directory(s,s->programs,0,s->cap/2,3);
    }
    for (unsigned r=0;r<sizeof(links_roots)/sizeof(*links_roots);r++) {
        s->budget=links_budget[r];
        links_directory(s,links_roots[r],links_on_desktop[r],r ? s->cap : s->cap/2,links_depth[r]);
    }
    /* Stable labels independent of FAT enumeration order. */
    struct desk_link_entry *temp=s->count>1 ? malloc(sizeof(*temp)) : NULL;
    if (s->count>1 && !temp) return (int)s->count;
    for (unsigned i=1;i<s->count;i++) for (unsigned j=i;j && strcmp(s->out[j-1].launch.label,s->out[j].launch.label)>0;j--) {
        *temp=s->out[j-1]; s->out[j-1]=s->out[j]; s->out[j]=*temp;
        struct link_stamp t=s->stamps[j-1]; s->stamps[j-1]=s->stamps[j]; s->stamps[j]=t;
    }
    free(temp);
    return (int)s->count;
}
/* Discovery and stale-link reads share one sleeping worker. The shell never
 * waits for directory/disk/RPC I/O; it consumes only complete snapshots. */
struct links_worker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int scan_requested, games_requested, scanning, launch_requested, launch_busy, completed, result;
    unsigned save_requested, save_game, save_busy, save_done, save_operation;
    int save_result;
    struct ciuki_dosvm_command save_command;
    unsigned games_removed;
    unsigned count;
    struct desktop *desktop;
    uint64_t scanned_ns;                       /* f2-95: last completed scan, 0 = none yet */
    unsigned scans, game_scans, stats, reads;  /* f2-95: totals for tests and diagnostics */
    char source[260];
    char profile_programs[USERS_PATH];
    int edit_requested, edit_busy, edit_done, edit_result;
    char edit_source[260];
    struct desk_launch_record edit_record;
    unsigned char icon_valid[DESK_LINKS], scratch_icon_valid[DESK_LINKS];
    uint32_t icons[DESK_LINKS][DESK_SHORTCUT_PIXELS], scratch_icons[DESK_LINKS][DESK_SHORTCUT_PIXELS];
    int run_requested, run_busy, run_done, run_result;
    char run_command[DESK_RUN_MAX];
    int history_requested, history_busy, history_done, history_error;
    char history_save[DESK_RUN_MAX];
    struct desk_run_history history, history_work;
    int browse_requested, browse_busy, browse_done, browse_error;
    char browse_path[260];
    struct du_file_entry browse_entries[DU_FILE_ENTRIES];
    unsigned browse_count;
    struct desk_link_entry published[DESK_LINKS], scratch[DESK_LINKS];
    struct link_stamp published_stamps[DESK_LINKS], scratch_stamps[DESK_LINKS];
};
static struct links_worker *links_worker;
static int links_profile_update(struct desktop *d)
{
    struct links_worker *w=links_worker;
    if (!w) return 1;
    pthread_mutex_lock(&w->mutex);
    if (w->scanning || w->launch_requested || w->launch_busy || w->save_requested || w->save_busy || w->edit_requested || w->edit_busy || w->edit_done) { pthread_mutex_unlock(&w->mutex); return 0; }
    snprintf(w->profile_programs,sizeof(w->profile_programs),"%s",d->sh.profile.programs);
    w->count=0; w->scanned_ns=0; w->scan_requested=1;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->mutex);
    d->v3_link_count=0;
    return 1;
}
/* Render-owned snapshot: worker writes never race a paint using these pixels. */
static unsigned char links_icon_valid[DESK_LINKS];
static uint32_t links_icons[DESK_LINKS][DESK_SHORTCUT_PIXELS];
static int run_history_idle(void)
{
    struct links_worker *w=links_worker; if (!w) return 1;
    pthread_mutex_lock(&w->mutex);
    int idle=!w->run_requested && !w->run_busy && !w->run_done &&
        !w->history_requested && !w->history_busy && !w->history_done && !w->desktop->sh.run_save_count;
    pthread_mutex_unlock(&w->mutex); return idle;
}
static void links_scan_once(struct links_worker *w, int games)
{
    struct links_scan s={.out=w->scratch,.stamps=w->scratch_stamps,.cap=DESK_LINKS,
        .previous=w->published,.previous_stamps=w->published_stamps,.previous_count=w->count};
    pthread_mutex_lock(&w->mutex);
    snprintf(s.programs,sizeof(s.programs),"%s",w->profile_programs);
    pthread_mutex_unlock(&w->mutex);
    int count=links_list(&s);
    memset(w->scratch_icon_valid,0,sizeof(w->scratch_icon_valid));
    for(unsigned k=0;k<s.count;k++)
        w->scratch_icon_valid[k]=(unsigned char)desk_shortcut_icon_load(&w->scratch[k].launch,w->scratch_icons[k]);
    unsigned removed=w->games_removed;
    for(unsigned g=1;games && g<=CIUKI_DOSVM_GAME_COUNT;g++){
        struct ciuki_dosvm_command c={.version=CIUKI_DOSVM_VERSION,.size=sizeof(c)};c.launch.game=g;
        if(!ciuki_dosvm(CIUKI_DOSVM_GAME_QUERY,&c)){
            if(c.state==1)removed&=~(1u<<(g-1));else removed|=1u<<(g-1);
        }
    }
    pthread_mutex_lock(&w->mutex);
    w->games_removed=removed;
    w->count=count>0 && count<=(int)DESK_LINKS ? (unsigned)count : 0;
    memcpy(w->published,w->scratch,w->count*sizeof(*w->published));
    memcpy(w->published_stamps,w->scratch_stamps,w->count*sizeof(*w->published_stamps));
    memcpy(w->icon_valid,w->scratch_icon_valid,sizeof(w->icon_valid));
    memcpy(w->icons,w->scratch_icons,w->count*sizeof(*w->icons));
    w->scanned_ns=monotonic(); w->scanning=0;
    w->scans++; w->game_scans+=!!games; w->stats+=s.stats; w->reads+=s.reads;
    desk_wake(w->desktop);
    pthread_mutex_unlock(&w->mutex);
}
static int open_with_record(const char *path,struct desk_launch_record *record)
{
    if (!path || path[0]!='/' || strchr(path,'"')) return -EINVAL;
    memset(record,0,sizeof(*record)); strcpy(record->label,"Open With");
    strcpy(record->target,"/Programs/files"); record->show=1;
    int n=snprintf(record->arguments,sizeof(record->arguments),"\"--file-action=%u:%s\"",FILES_OPEN_WITH,path);
    return n<0 || (unsigned)n>=sizeof(record->arguments) ? -E2BIG : 0;
}
static void *links_thread(void *arg)
{
    struct links_worker *w=arg;
    for (;;) {
        pthread_mutex_lock(&w->mutex);
        while (!w->scan_requested && !w->launch_requested && !w->save_requested && !w->edit_requested && !w->run_requested && !w->history_requested && !w->browse_requested) pthread_cond_wait(&w->changed,&w->mutex);
        int launch=w->launch_requested, games=0; char path[260];
        int history=w->history_requested;
        int run=!history && w->run_requested,browse=!history && !run && w->browse_requested;
        unsigned save=w->save_requested,game=w->save_game;
        int edit=w->edit_requested;
        if(edit){w->edit_requested=0;w->edit_busy=1;}
        else if (save) { w->save_requested=0; w->save_busy=1; }
        else if (run) { w->run_requested=0; w->run_busy=1; }
        else if (history) { w->history_requested=0; w->history_busy=1; }
        else if (browse) { w->browse_requested=0; w->browse_busy=1; }
        else if (launch) { memcpy(path,w->source,sizeof(path)); w->launch_requested=0; w->launch_busy=1; }
        else { w->scan_requested=0; games=w->games_requested; w->games_requested=0; w->scanning=1; }
        pthread_mutex_unlock(&w->mutex);
        if(edit){
            /* Re-read the source before replacement: stale or deleted links
             * fail explicitly and are never silently recreated. */
            struct desk_launch_record *old=malloc(sizeof(*old));
            int result=old ? link_read(NULL,w->edit_source,old) : -ENOMEM;
            free(old);
            if(!result)result=desk_shortcut_write(w->edit_source,&w->edit_record);
            pthread_mutex_lock(&w->mutex);
            w->edit_result=result;w->edit_done=1;w->edit_busy=0;w->scan_requested=1;
            desk_wake(w->desktop);pthread_mutex_unlock(&w->mutex);
        }else if (save) {
            struct ciuki_dosvm_command command={.version=CIUKI_DOSVM_VERSION,.size=sizeof(command)};
            command.launch.game=game;
            int result=ciuki_dosvm(save,&command) ? -errno : 0;
            pthread_mutex_lock(&w->mutex);
            w->save_result=result; w->save_command=command; w->save_operation=save;
            w->save_done=1; w->save_busy=0; pthread_mutex_unlock(&w->mutex);
        } else if (run) {
            struct desk_launch_record *r=malloc(sizeof(*r)); int result=-ENOMEM;
            if (r) result=desk_run_resolve(fm_native_vfs(),w->run_command,run_environment,NULL,r);
            if (!result) result=ciuki_win32_shell_launch(r);
            free(r);
            pthread_mutex_lock(&w->mutex);
            w->run_result=result; w->run_busy=0; w->run_done=1;
            desk_wake(w->desktop); pthread_mutex_unlock(&w->mutex);
        } else if (history) {
            int error=desk_run_history_load(&w->history_work);
            if (!error && w->history_save[0]) error=desk_run_history_add(&w->history_work,w->history_save);
            pthread_mutex_lock(&w->mutex);
            w->history=w->history_work; w->history_save[0]=0; w->history_busy=0; w->history_done=1; w->history_error=error;
            desk_wake(w->desktop); pthread_mutex_unlock(&w->mutex);
        } else if (browse) {
            unsigned count=0; int error=du_posix_lister(NULL,w->browse_path,w->browse_entries,DU_FILE_ENTRIES,&count);
            if (error==-ENOTDIR) { struct stat st; if (!stat(w->browse_path,&st) && S_ISREG(st.st_mode)) error=1; }
            pthread_mutex_lock(&w->mutex);
            w->browse_count=count; w->browse_error=error; w->browse_busy=0; w->browse_done=1;
            desk_wake(w->desktop); pthread_mutex_unlock(&w->mutex);
        } else if (launch) {
            struct desk_launch_record *r=malloc(sizeof(*r));
            int result=-ENOMEM;
            if(r && launch==2){
                memset(r,0,sizeof(*r));snprintf(r->label,sizeof(r->label),"Install");
                snprintf(r->target,sizeof(r->target),"%s",path);r->show=1;
                snprintf(r->directory,sizeof(r->directory),"%s",path);
                char *slash=strrchr(r->directory,'/');if(slash)*slash=0;
                result=0;
            }else if(r && launch==3) {
                result=fm_launch_resolve(fm_native_vfs(),path,NULL,r);
                if (result==1) result=0; /* folder is a Files launch record */
                if (!strncmp(path,"/system/settings/open-",22)) (void)unlink(path);
            }else if(r) {
                result=fm_launch_resolve(fm_native_vfs(),path,NULL,r);
                if (result==1) result=0;
            }
            if (r && result==-ENOEXEC && launch!=2) {
                char unresolved[260]; snprintf(unresolved,sizeof(unresolved),"%s",r->target[0]?r->target:path);
                result=open_with_record(unresolved,r);
            }
            if (!result) result=ciuki_win32_shell_launch(r);
            free(r);
            pthread_mutex_lock(&w->mutex); w->result=result; w->completed=1; w->launch_busy=0; w->scan_requested=1; desk_wake(w->desktop); pthread_mutex_unlock(&w->mutex);
        } else links_scan_once(w,games);
    }
    return NULL;
}
/* f2-95: request policy, called with the worker's mutex held. A forced
 * request waits for a scan in progress (it may predate the change). */
static void links_request(struct links_worker *w, struct desktop *d, uint64_t now)
{
    unsigned force=d->v3_links_force;
    if (force && !w->scanning) {
        w->scan_requested=1; if (force&DESK_LINKS_GAMES) w->games_requested=1;
        d->v3_links_force=0; pthread_cond_signal(&w->changed);
    } else if (!force && !w->scan_requested && !w->scanning && w->scanned_ns &&
               now-w->scanned_ns>=DESK_LINKS_RESCAN_NS) {
        w->scan_requested=1; pthread_cond_signal(&w->changed);
    }
}
static int links_snapshot(struct desktop *d, struct desk_link_entry *out, unsigned cap)
{
    struct links_worker *w=links_worker;
    if (!w) return -EAGAIN;
    pthread_mutex_lock(&w->mutex);
    unsigned n=w->count<cap ? w->count : cap;
    int same=n==d->v3_link_count && !memcmp(out,w->published,n*sizeof(*out));
    same=same && !memcmp(links_icon_valid,w->icon_valid,n*sizeof(*links_icon_valid)) &&
        !memcmp(links_icons,w->icons,n*sizeof(*links_icons));
    if (!same) memcpy(out,w->published,n*sizeof(*out));
    memcpy(links_icon_valid,w->icon_valid,n*sizeof(*links_icon_valid));
    memcpy(links_icons,w->icons,n*sizeof(*links_icons));
    links_request(w,d,monotonic());
    pthread_mutex_unlock(&w->mutex); return same ? -EAGAIN : (int)n;
}
static int link_cached(struct desktop *d,const char *source,struct desk_launch_record *out)
{
    for (unsigned k=0;k<d->v3_link_count;k++) if (!strcmp(source,d->v3_links[k].source)) { *out=d->v3_links[k].launch; return 0; }
    return -ENOENT;
}
static int drop_snapshot(struct desktop *d)
{
    /* Channel attachments are read-only surface grants, never file fds.
     * Serialize the shell-owned list and release the writable mapping
     * before publication so a queued drop has independent immutable bytes. */
    const struct sh_dnd *g=&d->sh.dnd;
    if (!g->count || g->count>DESK_DRAG_ITEMS) return -1;
    unsigned bytes=2;
    for (unsigned k=0;k<g->count;k++) {
        size_t n=strlen(g->paths[k]);
        if (!n || n>=FM_PATH || g->paths[k][0]!='/' || !desk_utf8_valid((const uint8_t *)g->paths[k],(unsigned)n)) return -1;
        if (strchr(g->paths[k],'\\') || strchr(g->paths[k],':')) return -1;
        const char *part=g->paths[k]+1;
        while (*part) {
            const char *end=strchr(part,'/'); size_t len=end ? (size_t)(end-part) : strlen(part);
            if (!len || (len==1 && part[0]=='.') || (len==2 && part[0]=='.' && part[1]=='.')) return -1;
            if (!end) break;
            part=end+1;
        }
        bytes+=(unsigned)n+2;
    }
    unsigned rows=(bytes+4+4095)/4096;
    int fd=ciuki_surface_create(1024,rows,CIUKI_SURFACE_XRGB8888);
    if (fd<0) return -1;
    uint8_t *out=ciuki_surface_map(fd,PROT_READ|PROT_WRITE);
    if (!out || out==MAP_FAILED) { close(fd); return -1; }
    desk_put32(out,bytes); memcpy(out+4,"X\n",2); unsigned at=6;
    for (unsigned k=0;k<g->count;k++) {
        size_t n=strlen(g->paths[k]);
        out[at++]=g->types && g->types[k]=='D' ? 'D' : 'F';
        memcpy(out+at,g->paths[k],n); at+=(unsigned)n; out[at++]='\n';
    }
    if (munmap(out,rows*4096)) { close(fd); return -1; }
    return fd;
}
static const uint32_t *link_icon_cached(struct desktop *d,const char *source,unsigned size)
{
    if(size!=16 && size!=32)return NULL;
    for(unsigned k=0;k<d->v3_link_count;k++)if(!strcmp(source,d->v3_links[k].source))
        return links_icon_valid[k] ? links_icons[k]+(size==32 ? 16*16 : 0) : NULL;
    return NULL;
}
static int link_save_async(struct desktop *d,const char *source,const struct desk_launch_record *record)
{
    if(!source || !record || !desk_launch_valid(record))return -EINVAL;
    unsigned k=0;for(;k<d->v3_link_count;k++)if(!strcmp(source,d->v3_links[k].source))break;
    if(k==d->v3_link_count)return -ENOENT;
    struct links_worker *w=links_worker;
    if(!w || pthread_mutex_trylock(&w->mutex))return -EBUSY;
    int result=0;
    if(w->edit_requested || w->edit_busy || w->edit_done)result=-EBUSY;
    else {
        snprintf(w->edit_source,sizeof(w->edit_source),"%s",source);w->edit_record=*record;w->edit_requested=1;
        pthread_cond_signal(&w->changed);
    }
    pthread_mutex_unlock(&w->mutex);return result;
}
static int link_launch_async(struct desktop *d,const struct desk_launch_record *record)
{
    struct links_worker *w=links_worker;
    if (!w || pthread_mutex_trylock(&w->mutex)) return -EBUSY;
    if (w->launch_requested || w->launch_busy || w->completed) { pthread_mutex_unlock(&w->mutex); return -EBUSY; }
    const char *source=NULL;
    for (unsigned k=0;k<d->v3_link_count;k++) if (!memcmp(record,&d->v3_links[k].launch,sizeof(*record))) source=d->v3_links[k].source;
    if (!source) { pthread_mutex_unlock(&w->mutex); return -ENOENT; }
    snprintf(w->source,sizeof(w->source),"%s",source); w->launch_requested=1;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->mutex); return 0;
}
static int run_submit(struct desktop *d,const char *command)
{
    (void)d;
    if (!command || strlen(command)>=DESK_RUN_MAX) return -E2BIG;
    struct links_worker *w=links_worker; if (!w) return -ENOSYS;
    pthread_mutex_lock(&w->mutex); int result=0;
    if (w->run_requested || w->run_busy || w->run_done) result=-EBUSY;
    else { strcpy(w->run_command,command); w->run_requested=1; pthread_cond_signal(&w->changed); }
    pthread_mutex_unlock(&w->mutex); return result;
}
static int shell_open_path(struct desktop *d,const char *path)
{
    (void)d;
    if (!path || strlen(path)>=260 || (path[0]!='/' && !(path[0] && path[1]==':') &&
        strncmp(path,"http://",7) && strncmp(path,"https://",8))) return -EINVAL;
    struct links_worker *w=links_worker;
    if (!w || pthread_mutex_trylock(&w->mutex)) return -EBUSY;
    int e=0;
    if (w->launch_requested || w->launch_busy || w->completed) e=-EBUSY;
    else { strcpy(w->source,path); w->launch_requested=3; pthread_cond_signal(&w->changed); }
    pthread_mutex_unlock(&w->mutex); return e;
}
extern uint32_t desk_w32_exchange(const void *,uint32_t,void *,uint32_t,uint32_t *) __attribute__((weak));
static int install_program(struct desktop *d,const char *path)
{
    (void)d;
    /* Removable volume/CD paths are resolved by the ordinary launch helper.
     * Keep the same bounded name policy as the Control Panel picker. */
    if(strncmp(path,"/mnt/",5) || path[5]<'d' || path[5]>'z' || path[6]!='/')return -EINVAL;
    const char *base=strrchr(path,'/');if(!base)return -EINVAL;base++;
    size_t n=strlen(base);
    if(strcasecmp(base,"setup.exe") && strcasecmp(base,"install.exe") && !(n>4 && !strcasecmp(base+n-4,".bat")))return -EINVAL;
    if(strstr(path,"/../") || strstr(path,"/./") || strchr(path,'\\'))return -EINVAL;
    struct links_worker *w=links_worker;
    if(!w || pthread_mutex_trylock(&w->mutex))return -EBUSY;
    int e=0;
    if(w->launch_requested || w->launch_busy || w->completed)e=-EBUSY;
    else if(strlen(path)>=sizeof(w->source))e=-ENAMETOOLONG;
    else {strcpy(w->source,path);w->launch_requested=2;pthread_cond_signal(&w->changed);}
    pthread_mutex_unlock(&w->mutex);return e;
}
static void links_start(struct desktop *d)
{
    if (links_worker) return;
    struct links_worker *w=calloc(1,sizeof(*w)); if (!w) return;
    if (pthread_mutex_init(&w->mutex,NULL)) { free(w); return; }
    if (pthread_cond_init(&w->changed,NULL)) { pthread_mutex_destroy(&w->mutex); free(w); return; }
    w->desktop=d;
    w->scan_requested=w->games_requested=1; /* the first scan also queries the bundled games */
    fm_registry_exchange=desk_w32_exchange;
    if (pthread_create(&w->thread,NULL,links_thread,w)) { pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->mutex); free(w); return; }
    links_worker=w;
    d->install_program=install_program; d->open_path=shell_open_path;
    fm_registry_exchange=desk_w32_exchange;
    d->v3_links_list=links_snapshot; d->v3_link_read=link_cached; d->v3_launch=link_launch_async;
    d->v3_link_save=link_save_async;d->v3_link_icon=link_icon_cached;
}
static void links_service(struct desktop *d)
{
    struct links_worker *w=links_worker;
    if (!w) {
        if (d->sh.save_op) {
            unsigned op=d->sh.save_op; d->sh.save_op=0;
            struct ciuki_dosvm_command command={0};
            desk_game_data_result(d,d->sh.save_game,op,-ENOMEM,&command);
        }
        return;
    }
    /* f2-98c: completions arrive as events with no retry tick, so take the
     * worker mutex (held briefly by the worker) instead of trylock. */
    pthread_mutex_lock(&w->mutex);
    d->games_removed=w->games_removed;
    unsigned save_done=w->save_done,save_game=w->save_game,save_op=w->save_operation;
    int save_result=w->save_result; struct ciuki_dosvm_command save_command=w->save_command;
    w->save_done=0;
    if (d->sh.save_op && !w->save_requested && !w->save_busy && !save_done) {
        w->save_game=d->sh.save_game; w->save_requested=d->sh.save_op; d->sh.save_op=0;
        pthread_cond_signal(&w->changed);
    }
    int edit_done=w->edit_done,edit_result=w->edit_result;w->edit_done=0;
    int completed=w->completed, result=w->result; w->completed=0;

    int run_done=w->run_done,run_result=w->run_result;
    char command[DESK_RUN_MAX]; command[0]=0;
    if (run_done) {
        strcpy(command,w->run_command); w->run_done=0;
        if (run_result>=0) desk_run_remember(d,command);
    }
    int history_error=0;
    if (w->history_done) { desk_run_history_ready(d,&w->history); history_error=w->history_error; w->history_done=0; }
    if (!w->run_requested && !w->run_busy && !w->history_requested && !w->history_busy &&
        (d->sh.run_history_request || d->sh.run_save_count)) {
        w->history_requested=1; d->sh.run_history_request=0;
        w->history_save[0]=0;
        if (d->sh.run_save_count) {
            strcpy(w->history_save,d->sh.run_saves[0]); d->sh.run_save_count--;
            memmove(d->sh.run_saves,d->sh.run_saves+1,d->sh.run_save_count*sizeof(d->sh.run_saves[0]));
        }
        pthread_cond_signal(&w->changed);
    }
    if (w->browse_done) {
        desk_run_browse_ready(d,w->browse_path,w->browse_entries,w->browse_count,w->browse_error); w->browse_done=0;
    }
    if (d->sh.run && d->sh.run->browse_pending && !w->browse_requested && !w->browse_busy) {
        strcpy(w->browse_path,d->sh.run->browse_path); w->browse_requested=1; d->sh.run->browse_pending=0; pthread_cond_signal(&w->changed);
    }
    pthread_mutex_unlock(&w->mutex);
    if(edit_done){
        d->v3_links_force|=DESK_LINKS_SCAN;d->v3_links_next=0;
        if(edit_result<0)desk_shell_message(d,"Shortcut Properties","The shortcut could not be saved. Check that the file is still available and writable.",1);
    }
    if (save_done) desk_game_data_result(d,save_game,save_op,save_result,&save_command);
    if (completed && result<0) desk_shell_message(d,"Programs","This shortcut is unavailable or its target could not be started.",1);
    if (run_done) desk_run_result(d,command,run_result<0 ? run_result : 0);
    if (history_error) desk_shell_notify_error(d,"Run history could not be saved",-history_error);
}
static void shell_setup(struct desktop *d, int gate)
{
    if (!gate) {
        desk_w32_start(d); links_start(d); d->v3_lease=win32_lease; d->v3_lease_poll=desk_w32_presenter_poll;
        d->v3_restart=win32_restart;
    }
    d->launch=launch_program; d->first_frame=first_frame_record; d->terminate=terminate_child; d->icon=store_icon; d->mixer_set=mixer_apply; d->mixer_line=mixer_line_apply;
    d->autoplay_fs=fm_native_vfs(); d->autoplay_preference=autoplay_preference; d->media_sound=media_sound;
    d->self_pid=(uint32_t)getpid();
    const char *authority=getenv(CIUKI_TASKS_ENV);
    int fd=authority ? number(authority,CIUKI_OPEN_MAX-1) : -1;
    d->authority_fd=fd>=3 && fcntl(fd,F_GETFD)>=0 ? fd : -1;
    terminate_authority=d->authority_fd;
    d->power_request=power_request; d->power_state=power_state; d->power_prepare=power_prepare; d->power_alive=power_alive;
    d->power_standby=power_standby;
    d->power_cancel_standby=power_cancel_standby;
    const char *mixer=getenv("CIUKI_MIXER_FD");
    fd=mixer ? number(mixer,CIUKI_OPEN_MAX-1) : -1;
    d->mixer_fd=fd>=3 && fcntl(fd,F_GETFD)>=0 ? fd : -1;
    observers_prepare(d);
    d->mixer_set=mixer_apply; d->mixer_line=mixer_line_apply;
    d->joystick_control=desktop_joystick_control;
    struct utsname u;
    strcpy(d->build_identity,"unavailable");
    strcpy(d->tz_env,"TZ=UTC0"); d->time_generation=UINT32_MAX; /* G1: until the first refresh */
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
    users_profile(&d->sh.profile,"",0,0);
    d->logoff=user_logoff;
    if (!gate) settings_load(d);
    if (!gate) startup_stage("settings_read");
    struct stat pictures;
    d->pictures_exists = !stat("/Documents/Pictures", &pictures) && S_ISDIR(pictures.st_mode);
    desk_shell_enable(d);
    if (!gate) desk_sound_init(d);
    if (!gate) {
        int result=desk_folder_init(d,fm_native_vfs());
        if (result) desk_shell_notify_error(d,"Desktop folder unavailable",result);
        settings_writer.notify=d->wake; settings_writer.notify_ctx=d->wake_ctx;
        settings_writer.run_job=mixer_job_run;
        settings_writer_started=!cfg_writer_start(&settings_writer,d->sh.profile.settings,d->sh.profile.temporary);
        int autoplay_error=desk_autoplay_prepare(d);
        if (autoplay_error) desk_shell_notify_error(d,"AutoPlay worker unavailable",autoplay_error);
        int schedule_error=desk_schedsvc_start(d);
        if (schedule_error) desk_shell_notify_error(d,"Task Scheduler unavailable",schedule_error);
        audio_shutdown_wait=d->sound_shutdown; d->sound_shutdown=shutdown_wait;
    }
    if (!gate) { startup_stage("fonts_icons_ready"); mixer_read(d); system_read(d); wallpaper_load(d); wallpaper_preload(d); wallpaper_worker_prepare(d); }
    else desk_shell_wallpaper_update(d);
    if (!gate) {
        /* Keep the ordinary first desktop frame when logon is disabled;
         * hold input only until the worker has read the optional policy. */
        user_initial=1; d->sh.user_gate=-1; d->sh.user_previous_focus=d->focus;
        d->sh.user_request.operation=USERS_READ; d->sh.user_want=1;
        desktop_watchdog_start(d);
    }
    /* fx-8: ordinary desktops spawn on the launcher worker. */
    if (!gate) { int error=launcher_start(d); if (error) desk_shell_notify_error(d,"Program launcher unavailable",error); }
}
extern void desk_recovery_init(struct desktop *, int, int) __attribute__((weak));
extern void desk_recovery_start(struct desktop *) __attribute__((weak));
int main(int argc, char **argv)
{
    int safe_mode = 0, terminal_shell = 0;
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
    for (int a = 1; a < argc; a++) if (!strcmp(argv[a],"--test=vm-fullscreen")) fullscreen_test = 1;
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
        else if (!strcmp(argv[a],"--safe")) safe_mode=1;               /* G14: Safe mode */
        else if (!strcmp(argv[a],"--shell=terminal")) terminal_shell=1; /* G14: Command prompt only */
        else if (fullscreen_test && !strncmp(argv[a],"--launch-game=",14) && startup_game_index<0) {
            for (unsigned g=0;g<DESK_GAME_COUNT;g++) if (!strcmp(argv[a]+14,desk_games[g].title)) startup_game_index=(int)g;
            if (startup_game_index<0) return 2;
        } else if (fullscreen_test && !strncmp(argv[a],"--game-kernel=",14) && !startup_game_kernel &&
                   !strcmp(argv[a]+14,"unified")) startup_game_kernel=argv[a]+14;
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
    d->drop_snapshot=drop_snapshot;
    du_clip_bind(desk_clip_local,d);
    d->now_ns=monotonic(); /* startup notifications use the same clock as service turns */
    d->time_ns=monotonic;
    d->screen_control=screen_control; d->monitor_power=monitor_power; d->lfb_bytes_per_pixel=(info.bpp+7)/8;
    d->accel=ciuki_display_accel;
    d->accel_info=ciuki_display_accel_info;
    d->engine_info=ciuki_display_engine_info;
    d->commands=ciuki_display_commands;
    d->batch=ciuki_display_batch;
    d->fence=ciuki_display_fence;
    d->display_info=ciuki_display_info;
    d->gfx_benchmark=gfx_benchmark;
    if (desk_load_portrait(portrait)) { fprintf(stderr,"desktop: approved Ciuki asset unavailable\n"); free(portrait); free(d); return gate_setup_error(gate,GATE_SETUP_PORTRAIT,EIO); }
    d->portrait=portrait;
    d->output=ciuki_surface_create(info.width,info.height,CIUKI_SURFACE_XRGB8888);
    d->pixels=d->output < 0 ? MAP_FAILED : ciuki_surface_map(d->output,PROT_READ|PROT_WRITE);
    if (d->pixels==MAP_FAILED) { int error=errno; if (d->output>=0) close(d->output); free(portrait); free(d); return gate_setup_error(gate,GATE_SETUP_OUTPUT,error); }
    desk_accel_reset(d,info.generation);
    int flags = fcntl(input,F_GETFL);
    if (flags < 0 || fcntl(input,F_SETFL,flags|O_NONBLOCK)) return gate_setup_error(gate,GATE_SETUP_INPUT,errno);
    if (du_completion_init(&desktop_completion)) return gate_setup_error(gate,GATE_SETUP_INPUT,errno);
    d->wake=du_completion_notify; d->wake_ctx=&desktop_completion;
    if (desk_recovery_init) desk_recovery_init(d,safe_mode,terminal_shell);
    int firstboot_pages=gate || terminal_shell ? 0 : desk_firstboot(d);
    shell_setup(d,gate);
    if (!gate && !test) {
        char *spool_argv[] = {"/System/printspool",NULL};
        char *spool_env[] = {"PATH=/Programs",NULL};
        if(ciuki_spawn(spool_argv[0],spool_argv,spool_env,NULL,0,0)<0)
            fprintf(stderr,"desktop: background print service unavailable (errno=%d)\n",errno);
    }
    if (fullscreen_test) d->realtime_second=12*3600;
    display_start(d);
    if(firstboot_pages&(1u<<FB_DISPLAY))desk_task_launch_args(d,"control",(const char *const[]){"--panel=display"},1);
    if(firstboot_pages&(1u<<FB_CHANGE_CLOCK))desk_task_launch_args(d,"control",(const char *const[]){"--panel=datetime"},1);
    /* f3-02: --launch=<name> starts one /Programs menu program through the same
     * path as a menu click (the f3:vm-window probe opens /Programs/dosbox so). */
    if (launch && !gate) startup_launch(d,launch);
    if (!gate && desk_recovery_start) desk_recovery_start(d);
    /* Channels and input now share a blocking native poll wait. */
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
    desk_saver_config(d);
    int victim_pid = -1, completed = 0;
    uint64_t next_victim = 0;
    int input_reported = 0, display_reported = 0;
    int report=0, input_dirty=0, shell_stage_shown=0, scenario_key=0, scenario_released=0;
    unsigned report_send=0;
    uint64_t report_deadline=0, next_input_report=0;
    unsigned reported_progress=0;
    int holding=0, shifted=0, startup_sound=0, accel_noticed=0;
    d->startup.done=gate || test || fullscreen_test;
#ifdef __i386__
    if (devices.boot_flags & ((1u<<0)|(1u<<6))) d->startup.skip=1; /* safe/test boot */
#endif
    uint64_t shift_base=0, shift_release=0, mixer_poll_next=0;
    for (;;) {
        uint64_t now = monotonic();
        desktop_stage(STALL_SHELL);
        desktop_wait("mixer-poll");
        mixer_poll(d,now,&mixer_poll_next);
        if (!gate || desk_v3_fixture_active() || desk_dialogs_fixture_active() || desk_upper_fixture_active() || desk_w32_fixture_active()) { desk_w32_poll(d); desktop_wait("storage-poll"); storage_poll(d,now); }
        /* Test pacing suppresses unsolicited heartbeat PINGs only. */
        if (gate) for (int i=0; i<DESK_CLIENTS; i++) d->clients[i].next_ping_ns=UINT64_MAX;
        desktop_wait("heartbeat"); desk_heartbeat(d,now);
        if (!gate) { desktop_wait("sound-results"); desk_sound_results(d); }
        if (d->shell) {
            struct timespec rtc;
            if (!fullscreen_test && !clock_gettime(CLOCK_REALTIME,&rtc) && rtc.tv_sec>=0 && d->realtime_second!=(uint64_t)rtc.tv_sec) {
                uint64_t previous=d->realtime_second+(uint64_t)(int64_t)d->local_offset;
                d->realtime_second=(uint64_t)rtc.tv_sec;
                desktop_time_refresh(d);
                desk_shell_clock_damage(d,previous);
            }
            if (!d->power.state) { desktop_wait("shell-tick"); desk_shell_tick(d); desk_links_refresh(d); links_service(d); }
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
            case GATE_V3:
                d->v3_lease=win32_lease;d->v3_lease_poll=desk_w32_presenter_poll;d->v3_restart=win32_restart;
                desk_w32_start(d);
                if(desk_v3_fixture_begin(d)<0)return gate_error("v3_fixture",errno,generation);
                report=1;break;
            case GATE_DIALOGS:
            case GATE_MEDIA:
                d->v3_lease=win32_lease;d->v3_lease_poll=desk_w32_presenter_poll;d->v3_restart=win32_restart;
                desk_w32_start(d);
                if ((command==GATE_MEDIA ? desk_media_fixture_begin(d) : desk_dialogs_fixture_begin(d))<0)return gate_error("media_fixture",errno,generation);
                report=1;break;
            case GATE_UPPER:
                d->v3_lease=win32_lease;d->v3_lease_poll=desk_w32_presenter_poll;d->v3_restart=win32_restart;
                desk_w32_start(d);
                if(desk_upper_fixture_begin(d)<0)return gate_error("upper_fixture",errno,generation);
                report=1;break;
            case GATE_W32_FIXTURE: {
                char name[sizeof(control->fixture)];
                memcpy(name,control->fixture,sizeof(name));
                if (!memchr(name,0,sizeof(name))) return gate_error("fixture_name",EINVAL,generation);
                int error=desk_w32_fixture_begin(d,name);
                if(error<0)return gate_error("w32_fixture",-error,generation);
                d->v3_lease=win32_lease;d->v3_lease_poll=desk_w32_presenter_poll;d->v3_restart=win32_restart;
                desk_w32_start(d);
                report=1;break;
            }
            case GATE_BROWSER: {
                char url[sizeof(control->fixture)];
                memcpy(url,control->fixture,sizeof(url));
                if (!memchr(url,0,sizeof(url)) || !url[0]) return gate_error("browser_url",EINVAL,generation);
                int pid=desk_browser_fixture_begin(d,url);
                if (pid<0) return gate_error("browser_launch",-pid,generation);
                report=1;break;
            }
            case GATE_INTERACT: interaction=1; report=1; break;
            default: return gate_error("command",EINVAL,generation);
            }
        }
        uint64_t input_begin=gate ? monotonic() : 0; /* f2-95: gate statistics only */
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
        if (gate) { uint64_t input_time=monotonic()-input_begin; d->stats.input_ns+=input_time; d->pending_input_ns+=input_time; }
        /* Input has priority over IPC, theme/file reads and rendering. The
         * gate's FIFO may deliberately retain paced events; ordinary input
         * drains again before rendering if a full FIFO left anything queued. */
        desktop_stage(STALL_IPC);
        for (int i=0; i<DESK_CLIENTS; i++) {
            struct desk_client *c=&d->clients[i];
            for (unsigned n=0; n<16 && c->channel>=0 && !c->v3_peer && !c->spawn_pending; n++) {
                struct ciuki_message m;
                desktop_wait("ipc-recv");
                int r=ciuki_channel_recv(c->channel,&m,DONTWAIT);
                if (r==1) { desktop_wait("ipc-message"); desk_receive(d,i,&m); }
                else { if (!r || (errno!=EAGAIN && errno!=EINTR)) desk_drop_client(d,i); break; }
            }
            desktop_wait("ipc-flush"); desk_flush(d,i);
        }
        d->drain_ns=monotonic();
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
        if (desk_startup_due(&d->startup,d->presents!=0,!fifo_count,now)) {
            int error=desk_startup_begin(d->startup.skip);
            if (error) desk_shell_notify_error(d,"Startup programs unavailable",-error);
        }
        launch_pending(d);
        launch_service(d);   /* fx-8: bind or report spawns the launcher worker completed */
        desktop_stage(STALL_SHELL);
        if (d->sh.want_reload&1) desktop_input_pending();
        shell_service(d);
        if(d->shell && !gate)input_settings_apply(d);
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
        /* fx-15: state changes of this turn that no path invalidated
         * (window shown or hidden, caption, task button, popup) become
         * damage before the frame decision, not only once a frame runs. */
        desk_scene_reconcile(d);
        int render_ready=gate || desk_frame_due(d,monotonic());
        uint64_t presents_before=d->presents; desktop_stage(STALL_RENDER);
        if (!render_ready) errno=0; /* the watchdog reads errno only from a frame */
        int redraw = render_ready ? desk_redraw(d) : -1;
        if (!gate) desktop_watchdog_frame(d,presents_before,redraw);
        if (render_ready && redraw && errno!=EINTR && errno!=EBUSY && !display_reported) {
            fprintf(stderr,"desktop: present failed (errno=%d)\n",errno); display_reported=1;
        }
        if (!gate && render_ready && !redraw && d->presents) startup_stage("first_frame_presented");
        desktop_stage(STALL_REPORT); startup_flush();
        if (!gate && d->shell && d->presents && !startup_sound) { startup_sound=1; desk_sound(d,SOUND_STARTUP); desk_sound(d,SOUND_LOGON); }
        if (!gate && d->shell && d->presents && !accel_noticed && (accel_noticed=accel_notice(d))) d->accel_lost=0;
        if (d->accel_lost && accel_noticed) {
            /* f2-92: the engine was quarantined after the notice: say so. */
            d->accel_lost=0; accel_noticed=0;
            memset(&accel_notice_state,0,sizeof(accel_notice_state));
        }
        desk_v3_fixture_tick(d);
        desk_dialogs_fixture_tick(d);
        desk_upper_fixture_tick(d);
        desk_w32_fixture_tick(d);
        int mode_frame=desk_display_frame_commit(d,redraw);
        if (mode_frame) {
            desk_display_reply(d,d->display_reply_mode && d->display_reply_mode!=d->display_mode ? -EIO : 0);
            if (mode_frame==DESK_DISPLAY_PERSIST_FRAME && d->shell && !d->sh.gate && settings_save(d))
                snprintf(d->notice, sizeof(d->notice), "Display applied; settings not saved");
        }
        if (!gate) performance_sample(d,d->now_ns); /* f2-95: the turn's clock, 2 s cadence */
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
            launch_service(d);
            while ((pid=waitpid(-1,&status,WNOHANG))>0) {
                launch_note_exit(d,(uint32_t)pid);
                desk_w32_child_exited(d,(uint32_t)pid,status); desk_child_exited(d,(uint32_t)pid,status);
            }
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
        /* Input and completed work share a turn; feature deadlines remain explicit. */
        desktop_stage(STALL_IDLE);
        /* f2-92: offscreen-cache uploads and checks between frames, one
         * bounded engine request per turn while nothing is due. */
        int cache_more=!gate && !d->damage.count && !d->move_damage.count && !d->present_damage.count &&
            desk_accel_cache_step(d,d->now_ns);
        /* f2-95: input that arrived during this turn's services and frame is
         * read before sleeping (and its client messages leave now), so the
         * sleep ends at its frame instead of after a further idle wait. */
        int late_input=0;
        if (!gate && fifo_count==0) {
            /* One kernel entry when nothing arrived (the common idle turn). */
            struct ciuki_input_event late[CIUKI_INPUT_READ_MAX];
            int n=ciuki_input_read(d->input,late,CIUKI_INPUT_READ_MAX);
            if (n>0) {
                d->now_ns=monotonic(); late_input=1;
                for (int i=0;i<n;i++) desk_input(d,&late[i]);
                (void)desk_drain_input(d,ciuki_input_read);
                for (int i=0;i<DESK_CLIENTS;i++) desk_flush(d,i);
            }
        }
        uint64_t sleep_now=monotonic();
        __atomic_store_n(&desktop_watchdog.loop_ms,(uint32_t)(sleep_now/1000000ull),__ATOMIC_RELEASE);
        uint64_t timer=d->shell && !gate && !d->power.state ? mixer_poll_next : UINT64_MAX;
        uint64_t wait=desktop_pause_ns(d,sleep_now,fifo_count!=0 || late_input,timer);
        /* The explicit crash-isolation probe has a shared-memory control
         * page without a descriptor. Its controller sampling is diagnostic
         * work, never the normal desktop's idle policy. */
        if (gate && wait>10000000ull) wait=10000000ull;
        if (cache_more && wait>1000000ull) { wait=1000000ull; pause_reason=PAUSE_CACHE; }
        if (!d->startup.done && d->startup.deadline && d->startup.deadline>sleep_now &&
            d->startup.deadline-sleep_now<wait) wait=d->startup.deadline-sleep_now;
        loop_turn(wait);
        if (d->shell && !gate && !d->power.state) {
            /* Shell clock/directory snapshots have a genuine one-second deadline. */
            uint64_t second=1000000000ull-sleep_now%1000000000ull;
            if (second<wait) wait=second;
        }
        struct pollfd fds[DESK_CLIENTS+4]; unsigned nfds=0;
        fds[nfds++]=(struct pollfd){d->input,POLLIN,0};
        fds[nfds++]=(struct pollfd){desktop_completion.fd[0],POLLIN,0};
        desk_clip_tick(d);
        for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].channel>=0 && !d->clients[i].spawn_pending)
            fds[nfds++]=(struct pollfd){d->clients[i].channel,POLLIN | (d->clients[i].tx_count ? POLLOUT : 0),0};
        /* fx-26: state-change watches (mixer master, drive namespace). */
        unsigned watched=nfds; int mixer_at=-1, media_at=-1;
        int watch=gate ? -1 : mixer_watch_fd(d);
        if (watch>=0) { mixer_at=(int)nfds; fds[nfds++]=(struct pollfd){watch,POLLIN,0}; }
        watch=gate ? -1 : media_watch_fd(d);
        if (watch>=0) { media_at=(int)nfds; fds[nfds++]=(struct pollfd){watch,POLLIN,0}; }
        int ready=poll(fds,nfds,du_wait_ms(sleep_now,wait==UINT64_MAX ? UINT64_MAX : sleep_now+wait));
        if (ready<0 && errno!=EINTR) return 1;
        if (ready>=0) watch_result(d,mixer_at>=0 ? fds[mixer_at].revents : -1,media_at>=0 ? fds[media_at].revents : -1);
        for (unsigned i=0;i<watched;i++) if (fds[i].revents&POLLNVAL) return 1;
        if (fds[1].revents&(POLLERR|POLLHUP)) return 1;
        if ((fds[1].revents&POLLIN) && du_completion_take(&desktop_completion)) return 1;
    }
}
