/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "sound_runtime.h"
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
static void gate_input(struct desktop *d, const struct ciuki_input_event *event)
{
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
static int launch_program(struct desktop *d, const struct desk_program *program)
{
    if (d->power.state) { errno=EBUSY; return -1; }
    /* Recheck each launch: the operator can change console verbosity at run time. */
    d->launch_mark = ciuki_launch_mark(CIUKI_LAUNCH_STATUS,0,0) > 0 ? ciuki_launch_mark : NULL;
    uint64_t click_ns = d->launch_click_ns ? d->launch_click_ns : monotonic();
    d->launch_click_ns = 0;
    int child=-1;
    for (int j=0; j<DESK_CLIENTS; j++) if (!d->children[j].pid) { child=j; break; }
    if (child<0) { errno=EAGAIN; return -1; }
    int pair[2];
    if (ciuki_channel_pair(pair)) return -1;
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
    if (!fcntl(pair[1],F_SETFD,0)) {
        index=desk_add_client(d,pair[0]);
        if (index>=0) {
            if (d->launch_mark) d->launch_mark(CIUKI_LAUNCH_BEGIN,0,click_ns/1000);
            pid=ciuki_spawn(program->path,d->launch_argv[0] ? full : argv,env,mapping,mappings,0);
        }
        else errno=ENOSPC;
    }
    int error=errno;
    close(pair[1]);
    if (pid<0) {
        if (index>=0) desk_drop_client(d,index); else close(pair[0]);
        errno=error; return -1;
    }
    /* Bind launch ownership before accepting the first kernel-stamped message.
     * This also permits cleanup if a child exits without ever sending HELLO. */
    d->clients[index].pid=(uint32_t)pid;
    d->clients[index].program=kind;
    d->clients[index].taskman=tasks;
    d->clients[index].mixer=!strcmp(program->name,"mixer");
    d->clients[index].awaiting_frame=graphical;
    d->clients[index].launched_ns=monotonic();
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
/* Volume Control requests (DESK_MIXER_SET): the desktop holds the delegated
 * /dev/mixer control handle (F4 contract, Volume and persistence); the kernel
 * validates the record and saves C:\\SYSTEM\\AUDIO.CFG. */
#ifdef __i386__   /* the SDK target; host replays (x86-64) have no mixer device */
#include <ciuki/audio.h>
#endif
static int mixer_apply(struct desktop *d, uint32_t left, uint32_t right, uint32_t mute)
{
    if (d->power.state) return -EBUSY;
#ifndef __i386__
    (void)d; (void)left; (void)right; (void)mute;
    return -ENODEV;   /* host replays: no mixer device */
#else
    struct ciuki_mixer_master m;
    if (d->mixer_fd<0) return -ENODEV;
    memset(&m,0,sizeof(m)); m.version=1; m.size=sizeof(m);
    if (ciuki_devctl(d->mixer_fd,MIXER_GET_MASTER,&m,sizeof(m))) return -errno;
    m.left=left; m.right=right; m.mute=mute; m.save_state=0; m.save_error=0; m.reserved=0;
    return ciuki_devctl(d->mixer_fd,MIXER_SET_MASTER,&m,sizeof(m)) ? -errno : 0;
#endif
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
static int settings_save(struct desktop *d)
{
    static char text[CFG_TEXT_MAX];
    size_t n=cfg_format(&d->sh.cfg,text,sizeof(text));
    if (!n) { errno=ENOSPC; return -1; }
    if (mkdir(CFG_DIR,0755) && errno!=EEXIST) return -1;
    static const char temp[]=CFG_DIR "/desktop.new";
    FILE *f=fopen(temp,"wb");
    if (!f) return -1;
    int ok=fwrite(text,1,n,f)==n;
    if (fflush(f)) ok=0;
    if (ok && fsync(fileno(f))) ok=0;
    if (fclose(f)) ok=0;
    if (ok && rename(temp,CFG_FILE)) {
        /* A file system without replacing rename: remove the old file first. */
        ok=!unlink(CFG_FILE) && !rename(temp,CFG_FILE);
    }
    if (!ok) { int error=errno; unlink(temp); errno=error; return -1; }
    return 0;
}
static int display_apply(struct desktop *d, uint16_t id)
{
    int e = ciuki_display_set(id);
    if (!e && id != d->display_mode) d->display_pending = id;
    return e;
}
/* Polls the readable counter once per frame. Allocation failure keeps the old
 * surface private and lets the kernel watchdog restore its previous mode. */
static void display_poll(struct desktop *d)
{
    struct ciuki_display_info info = {.version = CIUKI_DISPLAY_VERSION, .size = sizeof(info)};
    if (ciuki_display_query(&info)) return;
    uint32_t counter;
    if (read(d->display, &counter, sizeof(counter)) != sizeof(counter)) return;
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
    if (d->shell) desk_shell_wallpaper_update(d);
    d->display_counter = counter;
    d->display_mode = mode;
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
        if (e) snprintf(d->notice, sizeof(d->notice), "Display mode unavailable (error %d)", -e);
        break;
    }
}

static void wallpaper_load(struct desktop *d)
{
    desk_image_free(&d->sh.image);
    d->sh.wall_valid=0;
    const char *spec=d->sh.cfg.wallpaper;
    if (spec[0]=='/') {
        uint8_t *data; size_t n;
        if (read_file(spec,DESK_IMAGE_FILE_MAX,&data,&n) || desk_image_decode(data,n,&d->sh.image)) {
            snprintf(d->notice,sizeof(d->notice),"Wallpaper %.40s cannot be shown",strrchr(spec,'/')+1);
            desk_image_free(&d->sh.image);
        }
        if (data) free(data);
    }
    desk_shell_wallpaper_update(d);
}
static void mixer_read(struct desktop *d)
{
    int fd=d->mixer_fd, own=0;
    if (fd<0) { fd=open("/dev/mixer",O_RDONLY|O_CLOEXEC); own=1; }   /* public, read-only */
    struct ciuki_mixer_master m={.version=CIUKI_AUDIO_VERSION,.size=sizeof(m)};
    if (fd<0 || ciuki_devctl(fd,MIXER_GET_MASTER,&m,sizeof(m))<0) d->sh.mixer_state=-1;
    else {
        d->sh.mixer_state=d->mixer_fd>=0 ? 2 : 1;
        d->sh.volume=(int)((m.left+m.right)/2); d->sh.muted=m.mute!=0;
    }
    if (own && fd>=0) close(fd);
    desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
}
static void mixer_write(struct desktop *d)
{
    if (d->mixer_fd<0) { mixer_read(d); return; }
    struct ciuki_mixer_master m={.version=CIUKI_AUDIO_VERSION,.size=sizeof(m),
        .left=(uint32_t)d->sh.volume,.right=(uint32_t)d->sh.volume,.mute=(uint32_t)(d->sh.muted!=0)};
    if (ciuki_devctl(d->mixer_fd,MIXER_SET_MASTER,&m,sizeof(m))<0)
        snprintf(d->notice,sizeof(d->notice),"Volume change failed (errno %d)",errno);
    else if (m.save_state==CIUKI_MIXER_UNSAVED)
        snprintf(d->notice,sizeof(d->notice),"Volume applied but not saved (error %d)",(int)m.save_error);
    mixer_read(d);
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
            char text[160]; snprintf(text,sizeof(text),"'%s' could not be started (error %d).",name,error);
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
    if (sh->gate) { sh->want_save=sh->want_mixer=sh->want_power=0; sh->want_reload&=~1; sh->want_run[0]=0; }
    if (d->power.state) { desk_power_tick(d); return; }
    desk_folder_service(d);
    if (sh->want_power) {
        int action=sh->want_power; sh->want_power=0;
        if (settings_save(d)) { /* still try: settings are a convenience */ }
        desk_power_begin(d,action);
        return;
    }
    if (sh->want_reload & 1) {
        uint16_t confirmed = sh->cfg.display_mode;
        settings_load(d);
        sh->cfg.display_mode = confirmed; scan_programs(d);
        sh->icon_count=0; desk_shell_apply(d); wallpaper_load(d);
    }
    if (sh->want_reload & 2) { system_read(d); if (d->about>=0) desk_damage_add(&d->damage,d->clients[d->about].frame,d->width,d->height); }
    int volume_feedback = sh->want_reload & 8;
    if (sh->want_reload & 4) mixer_read(d);
    sh->want_reload=0;
    if (!sh->wall_valid) desk_shell_wallpaper_update(d);
    if (sh->want_save) {
        sh->want_save=0;
        if (settings_save(d)) snprintf(d->notice,sizeof(d->notice),"Settings not saved (errno %d)",errno);
    }
    if (sh->want_mixer) { sh->want_mixer=0; mixer_write(d); }
    if (volume_feedback) desk_sound(d,SOUND_VOLUME);
    if (sh->want_run[0]) {
        char line[sizeof(sh->want_run)];
        memcpy(line,sh->want_run,sizeof(line)); sh->want_run[0]=0;
        run_command(d,line);
    }
}
static void shell_setup(struct desktop *d, int gate)
{
    d->launch=launch_program; d->terminate=terminate_child; d->icon=store_icon; d->mixer_set=mixer_apply;
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
    desk_shell_enable(d);
    if (!gate) {
        int result=desk_folder_init(d,fm_native_vfs());
        if (result) snprintf(d->notice,sizeof(d->notice),"Desktop folder unavailable (error %d)",-result);
        desk_sound_init(d);
    }
    /* The first frame shows the desktop colour, bars and icons at once; the
     * image wallpaper (the default Ciuk2 PNG takes seconds to decode under
     * TCG) is loaded two loop iterations later (lead fix, 2026-10-12). */
    if (!gate) { d->sh.wallpaper_deferred=2; mixer_read(d); system_read(d); }
    desk_shell_wallpaper_update(d);
}
int main(int argc, char **argv)
{
    int display = 3, input = 4, test = 0, gate = 0, cycles = 1, demo_count = 0;
    int channels[DESK_CLIENTS], channel_count = 0;
    const char *demos[DESK_CLIENTS], *victim = NULL, *launch = NULL;
    for (int a = 1; a < argc; a++) if (!strcmp(argv[a],"--test=crash-isolation")) test = 1;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a],"--test=crash-isolation")) continue;
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
    d->accel=ciuki_display_accel;
    if (desk_load_portrait(portrait)) { fprintf(stderr,"desktop: approved Ciuki asset unavailable\n"); free(portrait); free(d); return gate_setup_error(gate,GATE_SETUP_PORTRAIT,EIO); }
    d->portrait=portrait;
    d->output=ciuki_surface_create(info.width,info.height,CIUKI_SURFACE_XRGB8888);
    d->pixels=d->output < 0 ? MAP_FAILED : ciuki_surface_map(d->output,PROT_READ|PROT_WRITE);
    if (d->pixels==MAP_FAILED) { int error=errno; if (d->output>=0) close(d->output); free(portrait); free(d); return gate_setup_error(gate,GATE_SETUP_OUTPUT,error); }
    desk_accel_reset(d,info.generation);
    int flags = fcntl(input,F_GETFL);
    if (flags < 0 || fcntl(input,F_SETFL,flags|O_NONBLOCK)) return gate_setup_error(gate,GATE_SETUP_INPUT,errno);
    if (!gate) shell_setup(d,0);
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
    uint64_t shift_base=0, shift_release=0;
    for (;;) {
        uint64_t now = monotonic();
        display_poll(d);
        /* Test pacing suppresses unsolicited heartbeat PINGs only. */
        if (gate) for (int i=0; i<DESK_CLIENTS; i++) d->clients[i].next_ping_ns=UINT64_MAX;
        desk_heartbeat(d,now);
        if (!gate) desk_sound_results(d);
        if (d->shell && !d->power.state && d->sh.wallpaper_deferred && !--d->sh.wallpaper_deferred) wallpaper_load(d);
        /* The shell follows the theme key of DESK_SETTINGS_PATH (gates keep the default). */
        if (d->shell && !gate && !d->power.state && desk_theme_poll(DESK_SETTINGS_PATH,now)) {
            desk_theme_changed(d); d->sh.wall_valid=0; desk_shell_wallpaper_update(d);   /* theme colours in the wallpaper */
        }
        if (d->shell) {
            struct timespec rtc;
            if (!clock_gettime(CLOCK_REALTIME,&rtc) && rtc.tv_sec>=0 && d->realtime_second!=(uint64_t)rtc.tv_sec) {
                if (d->realtime_second/60!=(uint64_t)rtc.tv_sec/60)   /* the top bar shows the date */
                    desk_damage_add(&d->damage,(struct desk_box){(int32_t)d->width/2,0,(int32_t)d->width/2,DESK_BAR},d->width,d->height);
                d->realtime_second=(uint64_t)rtc.tv_sec;
                desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
            }
            if (!d->power.state) desk_shell_tick(d);
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
        if (fifo_count+CIUKI_INPUT_READ_MAX<=GATE_INPUT_FIFO) {
            struct ciuki_input_event events[CIUKI_INPUT_READ_MAX];
            int n=ciuki_input_read(input,events,CIUKI_INPUT_READ_MAX);
            for (int i=0; i<n; i++) input_fifo[(fifo_head+fifo_count++)%GATE_INPUT_FIFO]=events[i];
            if (n>0) input_reported=0;
            else if (n<0 && errno!=EAGAIN && errno!=EINTR && !input_reported) {
                fprintf(stderr,"desktop: input unavailable (errno=%d)\n",errno); input_reported=1;
                struct ciuki_input_event resync={.type=CIUKI_INPUT_RESYNC}; desk_input(d,&resync);
            }
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
             * once inside the controller's post-fault interaction stage before the shell exists,
             * selects the case. F12: shell, F9: Task Manager (stage machines of their own);
             * F11: Files, F10: CiukNote (app scenarios: the launched program gets
             * --test=<its name>, checkpoint PINGs, a fixed clock and the storage write gate). */
            if (gate && interaction && !d->shell && ev.type==CIUKI_INPUT_KEY && ev.value==1) {
                int code=ev.code;
                int app=code==0x57 ? DESK_APP_FILES : code==0x44 ? DESK_APP_NOTE : 0;
                if (code==0x58 || code==0x43 || app) {
                    if (code==0x58) d->shell_test=1;
                    else if (code==0x43) d->tasks_test=1;
                    else d->app_test=app;
                    scenario_key=code; scenario_released=0; d->shell_keys=1; shell_setup(d,1);
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
        if (d->sh.want_power && !d->sh.gate) d->action=-1;
        if (d->action>=0) {
            unsigned selected=(unsigned)d->action; d->action=-1;
            int pid=selected<d->program_count && d->launch ? d->launch(d,&d->programs[selected]) : -1;
            if (pid<0) {
                snprintf(d->notice,sizeof(d->notice),"Program could not start (error %d)",errno);
                desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},d->width,d->height);
            } else if (d->shell_test && d->shell_stage==1 && !strcmp(d->programs[selected].name,"demo")) {
                d->shell_pid=(uint32_t)pid; d->shell_stage=2;
            } else if (d->tasks_test && d->tasks_stage==0 && !strcmp(d->programs[selected].name,"tasks")) {
                d->tasks_pid=(uint32_t)pid; d->tasks_stage=1;
            } else if (d->app_test && !d->app_pid && !strcmp(d->programs[selected].name,desk_app_program(d->app_test))) {
                d->app_pid=(uint32_t)pid;
            }
        }
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
        if (interaction) desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
        int redraw = desk_redraw(d);
        if (redraw && errno!=EINTR && !display_reported) {
            fprintf(stderr,"desktop: present failed (errno=%d)\n",errno); display_reported=1;
        }
        if (!gate && d->shell && d->presents && !startup_sound) { startup_sound=1; desk_sound(d,SOUND_STARTUP); }
        if (desk_display_frame_commit(d, redraw)) {
            if (d->shell && !d->sh.gate && settings_save(d))
                snprintf(d->notice, sizeof(d->notice), "Display applied; settings not saved");
        }
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
        struct timespec pause={.tv_sec=0,.tv_nsec=(long)(gate ? 1000000 : desk_launch_wait(d,monotonic()))}; nanosleep(&pause,NULL);
    }
}
