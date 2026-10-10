/* SPDX-License-Identifier: MIT */
#include "desktop.h"
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
static int spawn_demo(struct desktop *d, const char *kind)
{
    int pair[2];
    if (ciuki_channel_pair(pair)) return -1;
    char fault[48];
    snprintf(fault,sizeof(fault),"--fault=%s",kind);
    char *argv[] = {"demo","--channel-fd=3",fault,NULL};
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
    int display = 3, input = 4, test = 0, cycles = 1, demo_count = 0;
    int channels[DESK_CLIENTS], channel_count = 0;
    const char *demos[DESK_CLIENTS], *victim = NULL;
    for (int a = 1; a < argc; a++) if (!strcmp(argv[a],"--test=crash-isolation")) test = 1;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a],"--test=crash-isolation")) continue;
        if (!strncmp(argv[a],"--display-fd=",13)) display = number(argv[a]+13,63);
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
    for (int i=0; i<demo_count; i++) if (spawn_demo(d,demos[i]) < 0) fprintf(stderr,"desktop: demo spawn failed\n");
    int victim_pid = -1, completed = 0;
    uint64_t next_victim = 0;
    int input_reported = 0, display_reported = 0;
    for (;;) {
        uint64_t now = monotonic();
        desk_heartbeat(d,now);
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
        if (n>0) { for (int i=0; i<n; i++) desk_input(d,&events[i]); input_reported=0; }
        else if (n<0 && errno!=EAGAIN && errno!=EINTR && !input_reported) {
            fprintf(stderr,"desktop: input unavailable (errno=%d)\n",errno); input_reported=1;
            struct ciuki_input_event resync={.type=CIUKI_INPUT_RESYNC}; desk_input(d,&resync);
        }
        if (desk_redraw(d) && errno!=EINTR && !display_reported) {
            fprintf(stderr,"desktop: present failed (errno=%d)\n",errno); display_reported=1;
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
        if (victim_pid<0) { int status; while (waitpid(-1,&status,WNOHANG)>0) {} }
        if (test && victim && victim_pid<0 && completed<cycles && now>=next_victim)
            victim_pid=spawn_demo(d,victim);
        struct timespec pause={.tv_sec=0,.tv_nsec=10000000}; nanosleep(&pause,NULL);
    }
}
