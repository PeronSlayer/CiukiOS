/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "gate.h"
#include <ciuki/raw.h>
#include <ciuki/runtime.h>
#include <ciuki/channel.h>
#include <ciuki/spawn.h>
#include <ciuki/surface.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
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
static void gate_report(struct desktop *d, uint32_t control, unsigned generation, int survivor, int victim, unsigned cycle)
{
    char line[CIUKI_PROBE_REPORT_MAX+1];
    int n=snprintf(line,sizeof(line),"case=native-desktop control=%u generation=%u survivor=%d victim=%d cycle=%u replies=%llu keys=%u motion=%u buttons=%u",
        control,generation,survivor,victim,cycle,(unsigned long long)d->replied,gate_keys,gate_motion,gate_buttons);
    if (n<=0 || n>CIUKI_PROBE_REPORT_MAX || ciuki_error(ciuki_raw_probe_report(CU_PTR(line),n,0,0,0,0))) _exit(126);
}
static int gate_send(struct desktop *d, int pid, unsigned command, unsigned generation)
{
    for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].channel>=0 && d->clients[i].pid==(uint32_t)pid) {
        struct ciuki_message m={.length=12};
        desk_put32(m.data,GATE_MAGIC); desk_put32(m.data+4,command); desk_put32(m.data+8,generation);
        return ciuki_channel_send(d->clients[i].channel,&m,DONTWAIT);
    }
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
    int i = desk_add_client(d,pair[0]);
    ciuki_pid_t pid = i < 0 ? -1 : ciuki_spawn("/bin/demo",argv,env,&mapping,1,CIUKI_SPAWN_NEW_GROUP);
    close(pair[1]);
    if (pid < 0) {
        if (i >= 0) desk_drop_client(d,i); else close(pair[0]);
        return -1;
    }
    return (int)pid;
}
int main(int argc, char **argv)
{
    /* Kernel channel_send posts SIGPIPE as well as returning EPIPE. */
    if (desk_ignore_sigpipe()) return 1;
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
    struct ciuki_display_info info = {.size=sizeof(info)};
    if (ciuki_display_info(display,&info)) {
        fprintf(stderr,"desktop: display unavailable (errno=%d); console ready\n",errno);
        return 1; /* Lead uses the contracted stand-in on no-LFB profiles. */
    }
    if (!info.width || !info.height || info.width>2048 || info.height>2048) return 1;
    struct desktop *d = calloc(1,sizeof(*d));
    uint32_t *portrait = malloc(DESK_PORTRAIT_SIZE*DESK_PORTRAIT_SIZE*4);
    if (!d || !portrait) { free(d); free(portrait); return 1; }
    const struct desk_ops ops = {ciuki_channel_send,ciuki_surface_info,ciuki_surface_map,munmap,close,ciuki_present};
    desk_init(d,&ops,info.width,info.height); d->display=display; d->input=input;
    if (desk_load_portrait(portrait)) { fprintf(stderr,"desktop: approved Ciuki asset unavailable\n"); free(portrait); free(d); return 1; }
    d->portrait=portrait;
    d->output=ciuki_surface_create(info.width,info.height,CIUKI_SURFACE_XRGB8888);
    d->pixels=d->output < 0 ? MAP_FAILED : ciuki_surface_map(d->output,PROT_READ|PROT_WRITE);
    if (d->pixels==MAP_FAILED) { if (d->output>=0) close(d->output); free(portrait); free(d); return 1; }
    int flags = fcntl(input,F_GETFL);
    if (flags < 0 || fcntl(input,F_SETFL,flags|O_NONBLOCK)) return 1;
    /* The ABI has no timed input read or poll/select. A bounded nonblocking
     * drain followed by an interruptible 10 ms sleep keeps IPC/time moving. */
    for (int i=0; i<channel_count; i++) desk_add_client(d,channels[i]);
    for (int i=0; i<demo_count; i++) if (spawn_demo(d,demos[i],0) < 0) fprintf(stderr,"desktop: demo spawn failed\n");
    struct gate_control *control = NULL;
    int survivor_pid=0, gate_victim=0, gate_cycle=0, interaction=0;
    unsigned generation=0;
    if (gate) {
        control=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (control==MAP_FAILED || (survivor_pid=spawn_demo(d,"none",1))<0) return 1;
        gate_report(d,CU_PTR(control),0,survivor_pid,0,0);
    }
    int victim_pid = -1, completed = 0;
    uint64_t next_victim = 0;
    int input_reported = 0, display_reported = 0;
    for (;;) {
        uint64_t now = monotonic();
        /* Test pacing suppresses unsolicited heartbeat PINGs only. */
        if (gate) for (int i=0; i<DESK_CLIENTS; i++) d->clients[i].next_ping_ns=UINT64_MAX;
        desk_heartbeat(d,now);
        int report=0;
        if (gate && control->command) {
            unsigned command=control->command; generation=control->generation;
            control->command=0;
            switch (command) {
            case GATE_SPAWN:
                if (gate_victim || gate_cycle>=100) return 1;
                gate_victim=spawn_demo(d,gate_faults[gate_cycle%5],1);
                if (gate_victim<0) return 1;
                /* Register the PID before any CONFIGURE send can schedule
                 * the child and let its armed report reach the controller. */
                gate_report(d,CU_PTR(control),generation,survivor_pid,gate_victim,(unsigned)gate_cycle);
                report=1; break;
            case GATE_RELEASE: if (gate_send(d,gate_victim,GATE_RELEASE,generation)) return 1; report=1; break;
            case GATE_REAP: {
                int status;
                if (waitpid(gate_victim,&status,WNOHANG)!=gate_victim) return 1;
                for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].pid==(uint32_t)gate_victim) desk_drop_client(d,i);
                gate_victim=0; gate_cycle++; report=1; break;
            }
            case GATE_RUN: if (gate_send(d,survivor_pid,GATE_RUN,generation)) return 1; report=1; break;
            case GATE_SNAPSHOT: report=2; break;
            case GATE_INTERACT: interaction=1; report=1; break;
            default: return 1;
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
            desk_input(d,&events[i]);
            if (gate && interaction) {
                if (events[i].type==CIUKI_INPUT_KEY && events[i].code==0x1e) gate_keys++;
                if (events[i].type==CIUKI_INPUT_MOTION) gate_motion++;
                if (events[i].type==CIUKI_INPUT_BUTTON) gate_buttons++;
                report=1;
            }
        } input_reported=0; }
        else if (n<0 && errno!=EAGAIN && errno!=EINTR && !input_reported) {
            fprintf(stderr,"desktop: input unavailable (errno=%d)\n",errno); input_reported=1;
            struct ciuki_input_event resync={.type=CIUKI_INPUT_RESYNC}; desk_input(d,&resync);
        }
        if (interaction) desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
        if (desk_redraw(d) && errno!=EINTR && !display_reported) {
            fprintf(stderr,"desktop: present failed (errno=%d)\n",errno); display_reported=1;
        }
        if (report) {
            if (control->command==0) {
                for (int i=0;i<DESK_CLIENTS;i++) {
                    desk_flush(d,i);
                    if (d->clients[i].tx_count) return 1;
                }
            }
            if (report==2 && gate_send(d,survivor_pid,GATE_SNAPSHOT,generation)) return 1;
            gate_report(d,CU_PTR(control),generation,survivor_pid,gate_victim,(unsigned)gate_cycle);
            /* Reports use the console too; restore the complete frame. */
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
            if (desk_redraw(d)) return 1;
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
        if (!gate && victim_pid<0) { int status; while (waitpid(-1,&status,WNOHANG)>0) {} }
        if (test && victim && victim_pid<0 && completed<cycles && now>=next_victim)
            victim_pid=spawn_demo(d,victim,0);
        struct timespec pause={.tv_sec=0,.tv_nsec=gate ? 1000000 : 10000000}; nanosleep(&pause,NULL);
    }
}
