/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "gate.h"
#include <ciuki/raw.h>
#include <ciuki/runtime.h>
#include <ciuki/channel.h>
#include <ciuki/spawn.h>
#include <ciuki/surface.h>
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
    if (event->type==CIUKI_INPUT_KEY && event->code==0x1e &&
        (event->value==0 || event->value==1) &&
        (!d->shell_test || d->shell_stage>=5)) gate_keys++; /* f2-30: shell sequence first; stage 5 = close requested (the reap to 6 is asynchronous; the final report still requires 6) */
    if (event->type==CIUKI_INPUT_MOTION) gate_motion++;
    /* The input grant delivers separate button edges, not a motion mask.
     * Count consumption even when the click focuses the background/title. */
    if (event->type==CIUKI_INPUT_BUTTON && event->code>=1 && event->code<=3 &&
        (event->value==0 || event->value==1)) gate_buttons++;
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
    int n=snprintf(line,sizeof(line),"case=native-desktop control=%u generation=%u survivor=%d victim=%d cycle=%u replies=%llu keys=%u motion=%u buttons=%u",
        control,generation,survivor,victim,cycle,(unsigned long long)d->replied,keys,gate_motion,gate_buttons);
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
        if (i >= 0) pid = ciuki_spawn("/bin/demo",argv,env,&mapping,1,CIUKI_SPAWN_NEW_GROUP);
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
    DIR *dir=opendir("/bin");
    if (!dir) { strcpy(d->notice,"Programs are unavailable"); return; }
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        char path[70];
        int len=snprintf(path,sizeof(path),"/bin/%s",entry->d_name);
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
static int terminate_child(uint32_t pid, int sig)
{ return kill((pid_t)pid,sig); }
static int launch_program(struct desktop *d, const struct desk_program *program)
{
    int child=-1;
    for (int j=0; j<DESK_CLIENTS; j++) if (!d->children[j].pid) { child=j; break; }
    if (child<0) { errno=EAGAIN; return -1; }
    int pair[2];
    if (ciuki_channel_pair(pair)) return -1;
    int index=-1;
    ciuki_pid_t pid=-1;
    char *argv[]={(char *)program->name,(strcmp(program->name,"demo") && strcmp(program->name,"dosbox")) ? NULL : "--channel-fd=3",NULL};
    char *env[]={"LC_ALL=C","TZ=UTC0","HOME=/home","TMPDIR=/tmp","PATH=/bin",NULL};
    struct ciuki_spawn_fd mapping={pair[1],3};
    if (!fcntl(pair[1],F_SETFD,0)) {
        index=desk_add_client(d,pair[0]);
        if (index>=0) pid=ciuki_spawn(program->path,argv,env,&mapping,1,0);
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
    d->children[child].pid=(uint32_t)pid;
    return pid;
}
static void shell_setup(struct desktop *d)
{
    d->launch=launch_program; d->terminate=terminate_child;
    struct utsname u;
    strcpy(d->build_identity,"unavailable");
    if (!uname(&u)) {
        snprintf(d->build_identity,sizeof(d->build_identity),"%.64s",u.release);
#ifdef __i386__
        d->realtime_source=(int)u.realtime_source;
#endif
    }
    scan_programs(d);
    desk_shell_enable(d);
}
int main(int argc, char **argv)
{
    int display = 3, input = 4, test = 0, gate = 0, cycles = 1, demo_count = 0;
    int channels[DESK_CLIENTS], channel_count = 0;
    const char *demos[DESK_CLIENTS], *victim = NULL;
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
        else return 2;
    }
    if (display < 3 || input < 3 || display == input || cycles < 1 ||
        channel_count+demo_count+(victim != NULL) > DESK_CLIENTS) return 2;
    for (int i=0; i<channel_count; i++) if (channels[i]==display || channels[i]==input) return 2;
    /* Kernel channel_send posts SIGPIPE as well as returning EPIPE. */
    if (desk_ignore_sigpipe()) return gate_setup_error(gate,GATE_SETUP_SIGPIPE,errno);
    struct ciuki_display_info info = {.size=sizeof(info)};
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
    if (desk_load_portrait(portrait)) { fprintf(stderr,"desktop: approved Ciuki asset unavailable\n"); free(portrait); free(d); return gate_setup_error(gate,GATE_SETUP_PORTRAIT,EIO); }
    d->portrait=portrait;
    d->output=ciuki_surface_create(info.width,info.height,CIUKI_SURFACE_XRGB8888);
    d->pixels=d->output < 0 ? MAP_FAILED : ciuki_surface_map(d->output,PROT_READ|PROT_WRITE);
    if (d->pixels==MAP_FAILED) { int error=errno; if (d->output>=0) close(d->output); free(portrait); free(d); return gate_setup_error(gate,GATE_SETUP_OUTPUT,error); }
    int flags = fcntl(input,F_GETFL);
    if (flags < 0 || fcntl(input,F_SETFL,flags|O_NONBLOCK)) return gate_setup_error(gate,GATE_SETUP_INPUT,errno);
    if (!gate) shell_setup(d);
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
    int report=0, input_dirty=0, shell_stage_shown=0;
    unsigned report_send=0;
    uint64_t report_deadline=0, next_input_report=0;
    for (;;) {
        uint64_t now = monotonic();
        /* Test pacing suppresses unsolicited heartbeat PINGs only. */
        if (gate) for (int i=0; i<DESK_CLIENTS; i++) d->clients[i].next_ping_ns=UINT64_MAX;
        desk_heartbeat(d,now);
        if (d->shell) {
            struct timespec rtc;
            if (!clock_gettime(CLOCK_REALTIME,&rtc) && rtc.tv_sec>=0 && d->realtime_second!=(uint64_t)rtc.tv_sec) {
                d->realtime_second=(uint64_t)rtc.tv_sec;
                desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
            }
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
        struct ciuki_input_event events[CIUKI_INPUT_READ_MAX];
        int n=ciuki_input_read(input,events,CIUKI_INPUT_READ_MAX);
        if (n>0) { for (int i=0; i<n; i++) {
            /* Runtime-only shell case extension: old gates never send F12.
             * Keep it behind the controller's post-fault interaction stage. */
            if (gate && interaction && !d->shell && events[i].type==CIUKI_INPUT_KEY &&
                events[i].code==0x58 && events[i].value==1) {
                d->shell_test=1; d->shell_keys=1; shell_setup(d);
                continue;
            }
            if (gate && d->shell_test && events[i].type==CIUKI_INPUT_KEY &&
                events[i].code==0x58 && events[i].value==0) {
                d->shell_keys++; input_dirty=1; continue;
            }
            if (gate && interaction) {
                gate_input(d,&events[i]);
                input_dirty=1;
            } else desk_input(d,&events[i]);
        } input_reported=0; }
        else if (n<0 && errno!=EAGAIN && errno!=EINTR && !input_reported) {
            fprintf(stderr,"desktop: input unavailable (errno=%d)\n",errno); input_reported=1;
            struct ciuki_input_event resync={.type=CIUKI_INPUT_RESYNC}; desk_input(d,&resync);
        }
        if (d->action>=0) {
            unsigned selected=(unsigned)d->action; d->action=-1;
            int pid=selected<d->program_count && d->launch ? d->launch(d,&d->programs[selected]) : -1;
            if (pid<0) {
                snprintf(d->notice,sizeof(d->notice),"Program could not start (error %d)",errno);
                desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},d->width,d->height);
            } else if (d->shell_test && d->shell_stage==1 && !strcmp(d->programs[selected].name,"demo")) {
                d->shell_pid=(uint32_t)pid; d->shell_stage=2;
            }
        }
        if (d->shell_test && d->shell_stage!=shell_stage_shown) {
            /* Lead diagnostics: the shell test's stage is visible in the runner's screenshot. */
            shell_stage_shown=d->shell_stage;
            snprintf(d->notice,sizeof(d->notice),"shell stage %u programs %u",d->shell_stage,d->program_count);
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},d->width,d->height);
        }
        if (interaction) desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
        if (desk_redraw(d) && errno!=EINTR && !display_reported) {
            fprintf(stderr,"desktop: present failed (errno=%d)\n",errno); display_reported=1;
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
        if (test && victim && victim_pid<0 && completed<cycles && now>=next_victim)
            victim_pid=spawn_demo(d,victim,0);
        struct timespec pause={.tv_sec=0,.tv_nsec=gate ? 1000000 : 10000000}; nanosleep(&pause,NULL);
    }
}
