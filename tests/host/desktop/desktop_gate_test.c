/* Production desktop test loop with deterministic syscall boundary fakes.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "desktop.h"
#include "gate.h"
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
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
static int ciuki_error(uint32_t r) { return r >= (uint32_t)-4095 ? -(int32_t)r : 0; }
static uint32_t ciuki_raw_probe_report(uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
static void *gate_mmap(void *,size_t,int,int,int,off_t);
#define CIUKI_RUNTIME_H
#define CIUKI_RAW_H
#define mmap gate_mmap
#define main desktop_main
#include "../../../apps/desktop/desktop.c"
#undef main
#undef mmap
static jmp_buf done;
static struct gate_control page;
static uint32_t output_pixels[320*320];
static int fd_next=10,pair_client[64],client_pid[64],hello[64],next_pid=42;
static unsigned faults, pongs, remaining, running, reaped, presents, summary_count, interaction_sent;
static int victim_live, victim_dead, registered_victim;
static unsigned generation=1;
static bool interact_ready;
static void command(unsigned cmd) { page.generation=generation++;page.command=cmd;if(cmd==GATE_INTERACT)interact_ready=true; }
static void *gate_mmap(void *addr,size_t n,int prot,int flags,int fd,off_t off)
{ assert(!addr && n==4096 && prot==(PROT_READ|PROT_WRITE) && flags==(MAP_PRIVATE|MAP_ANONYMOUS) && fd==-1 && !off);return &page; }
static uint32_t ciuki_raw_probe_report(uintptr_t address,uint32_t n,uint32_t a,uint32_t b,uint32_t c,uint32_t e)
{
    (void)a;(void)b;(void)c;(void)e;
    const char *line=(void *)address;assert(n==strlen(line) && n<=CIUKI_PROBE_REPORT_MAX);
    const char *published=strstr(line," victim=");assert(published);
    registered_victim=atoi(published+8);
    summary_count++;
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
int ciuki_channel_pair(int pair[2]) { pair[0]=fd_next++;pair[1]=fd_next++;assert(fd_next<64);pair_client[pair[1]]=pair[0];return 0; }
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *mapping,uint32_t count,uint32_t flags)
{
    assert(!strcmp(path,"/bin/demo") && !strcmp(argv[0],"demo") && !strcmp(argv[1],"--channel-fd=3") &&
        !strcmp(argv[3],"--test=crash-isolation") && !argv[4]);
    assert(env && !strcmp(env[0],"LC_ALL=C") && mapping && count==1 && mapping->target==3 && flags==CIUKI_SPAWN_NEW_GROUP);
    int pid=next_pid++;client_pid[pair_client[mapping->source]]=pid;
    if (strcmp(argv[2],"--fault=none")) {
        char expected[64];snprintf(expected,sizeof(expected),"--fault=%s",gate_faults[faults%5]);
        assert(!strcmp(argv[2],expected));faults++;victim_live=pid;
    }
    return pid;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    assert(flags==DONTWAIT && fd>=10 && fd<64);
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
    assert(flags==DONTWAIT && fd>=10 && fd<64);
    if(client_pid[fd]!=42) assert(client_pid[fd]==registered_victim);
    if (!hello[fd]++) desk_message(m,DESK_HELLO,4);
    else if (client_pid[fd]==42 && remaining) {desk_message(m,DESK_PING,8);desk_put32(m->data+4,101-remaining--);}
    else {errno=EAGAIN;return -1;}
    m->sender_pid=client_pid[fd];return 1;
}
int ciuki_display_info(int fd,struct ciuki_display_info *s)
{ assert(fd==3);s->width=s->height=320;return 0; }
int ciuki_surface_create(uint32_t w,uint32_t h,uint32_t f)
{assert(w==320 && h==320 && f==CIUKI_SURFACE_XRGB8888);return 6;}
void *ciuki_surface_map(int fd,int prot) {assert(fd==6 && prot==(PROT_READ|PROT_WRITE));return output_pixels;}
int ciuki_surface_info(int fd,struct ciuki_surface_info *s) { (void)fd;(void)s;assert(0);return -1; }
int ciuki_present(int display,int surface,const struct ciuki_rect *r) {assert(display==3 && surface==6 && r);presents++;return 0;}
int ciuki_input_read(int fd,struct ciuki_input_event *events,uint32_t capacity)
{
    assert(fd==4 && capacity==CIUKI_INPUT_READ_MAX);
    if (interact_ready && !interaction_sent++) {
        events[0]=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=0x1e,.value=1};
        events[1]=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=0x1e,.value=0};
        events[2]=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=-20,.value2=20};
        events[3]=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};
        events[4]=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=0};return 5;
    }
    errno=EAGAIN;return -1;
}
int desk_load_portrait(uint32_t *out) {memset(out,0,256*256*4);return 0;}
int fcntl(int fd,int cmd,...) {assert(fd==4 && (cmd==F_GETFL || cmd==F_SETFL));return 0;}
int close(int fd) { (void)fd;return 0; }
int munmap(void *p,size_t n) { (void)p;(void)n;return 0; }
pid_t waitpid(pid_t pid,int *status,int flags) {assert(pid==victim_live && victim_dead && flags==WNOHANG);*status=(faults%5==2) ? SIGPIPE : SIGSEGV;victim_live=victim_dead=0;reaped++;return pid;}
int clock_gettime(clockid_t clock,struct timespec *out) {assert(clock==CLOCK_MONOTONIC);out->tv_sec=0;out->tv_nsec=1000000;return 0;}
int nanosleep(const struct timespec *t,struct timespec *rest)
{ (void)rest;assert(t->tv_nsec==1000000);if (running && !remaining && pongs==running*100 && !page.command) command(GATE_SNAPSHOT);return 0; }
int main(void)
{
    char *invalid[]={"desktop","--gate",NULL};assert(desktop_main(2,invalid)==2);
    char *args[]={"desktop","--test=crash-isolation","--gate",NULL};
    if (!setjmp(done)) {desktop_main(3,args);assert(0);}
    assert(faults==5 && reaped==5 && running==5 && pongs==500 && presents>0 && summary_count>=26);
    puts("desktop production gate: five fault kinds, spawn mappings/groups, controller pacing, flush barriers, input summaries PASS");
    return 0;
}
