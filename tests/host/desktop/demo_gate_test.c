/* Native demo argument, pacing, summary and actual fault dispatch boundary.
 * Syscall fakes never claim guest qualification. SPDX-License-Identifier: MIT */
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
#undef ILL_ILLOPC
#undef FPE_INTDIV
#undef SEGV_MAPERR
#undef SEGV_ACCERR
#undef BUS_ADRALN
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
#define CU_PTR(p) ((uintptr_t)(p))
static int ciuki_error(uint32_t r) { return r >= (uint32_t)-4095 ? -(int32_t)r : 0; }
static uint32_t ciuki_raw_probe_report(uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
#define CIUKI_RUNTIME_H
#define CIUKI_RAW_H
#define main demo_main
#include "../../../apps/demo/demo.c"
#undef main
static uint32_t pixels[WIDTH*HEIGHT], last_serial;
static int phase, pipe_fd, grant_checks, forged_checks, turn_reports, expected_fd=3, barrier_sent;
static int fault_mode;
static bool report_error;
static FILE *client_messages, *server_messages;
static uint32_t ciuki_raw_probe_report(uintptr_t address,uint32_t bytes,uint32_t a,uint32_t b,uint32_t c,uint32_t d)
{
    (void)a;(void)b;(void)c;(void)d;
    const char *line=(void *)address;
    assert(bytes && bytes<=CIUKI_PROBE_REPORT_MAX && strlen(line)==bytes);
    if (report_error) return (uint32_t)-EFAULT;
    puts(line); fflush(stdout);
    if (strstr(line,"stage=2 turns=100 ")) turn_reports++;
    return 0;
}
int ciuki_surface_create(uint32_t w,uint32_t h,uint32_t format)
{ assert(w==WIDTH && h==HEIGHT && format==CIUKI_SURFACE_XRGB8888);return 7; }
void *ciuki_surface_map(int fd,int prot) { assert(fd==7 && prot==(PROT_READ|PROT_WRITE));return pixels; }
int ciuki_surface_info(int fd,struct ciuki_surface_info *s)
{ assert(fd==7);*s=(struct ciuki_surface_info){sizeof(*s),WIDTH,HEIGHT,WIDTH*4,CIUKI_SURFACE_XRGB8888,sizeof(pixels)};return 0; }
int ciuki_display_info(int fd,struct ciuki_display_info *s) { (void)s;assert(fd==expected_fd);grant_checks++;errno=EBADF;return -1; }
int ciuki_present(int fd,int surface,const struct ciuki_rect *r) { (void)r;assert(fd==expected_fd && surface==7);grant_checks++;errno=EBADF;return -1; }
int ciuki_input_read(int fd,struct ciuki_input_event *e,uint32_t n) { (void)e;assert(fd==expected_fd && n==1);grant_checks++;errno=EBADF;return -1; }
int ciuki_channel_pair(int pair[2]) { pair[0]=10;pair[1]=11;return 0; }
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    (void)flags;
    if (fd==10 && pipe_fd) { raise(SIGPIPE);errno=EPIPE;return -1; }
    assert(fd==expected_fd);
    if (client_messages) {
        struct ciuki_message delivered=*m; delivered.sender_pid=42;
        assert(fwrite(&delivered,sizeof(delivered),1,client_messages)==1);
    }
    if (m->fd_count) {
        if (m->fds[0]!=7) { forged_checks++;errno=EBADF;return -1; }
    }
    if (desk_get32(m->data)==(DESK_VERSION | (DESK_PING<<8))) last_serial=desk_get32(m->data+4);
    return 0;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    assert(fd==expected_fd && flags==DONTWAIT);
    if (client_messages) return 0;
    if (server_messages) {
        if (fread(m,sizeof(*m),1,server_messages)==1) return 1;
        assert(feof(server_messages));return 0;
    }
    if ((uintptr_t)m==0x30000000u) {errno=EFAULT;return -1;}
    if (!phase++) { desk_message(m,DESK_CONFIGURE,12);desk_put32(m->data+4,WIDTH);desk_put32(m->data+8,HEIGHT); }
    else if (phase==2) { memset(m,0,sizeof(*m));m->length=12;desk_put32(m->data,GATE_MAGIC);desk_put32(m->data+4,fault_mode ? GATE_RELEASE : GATE_RUN); }
    else if (turn_reports) {
        if (!barrier_sent++) { memset(m,0,sizeof(*m));m->length=12;desk_put32(m->data,GATE_MAGIC);desk_put32(m->data+4,GATE_SNAPSHOT);desk_put32(m->data+8,7); }
        else return 0;
    } else { desk_message(m,DESK_PONG,8);desk_put32(m->data+4,last_serial); }
    m->sender_pid=42;return 1;
}
int close(int fd) { if (fd==11) pipe_fd=1;return 0; }
int munmap(void *p,size_t n) { assert(p==pixels && n==sizeof(pixels));return 0; }
int nanosleep(const struct timespec *t,struct timespec *remain)
{ (void)remain;assert(t->tv_nsec==1000000);return 0; }
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--report-error")) { report_error=true; demo_report(2,0,0,0);assert(0); }
    if (argc==3 && (!strcmp(argv[1],"--hello") || !strcmp(argv[1],"--handshake"))) {
        if (!strcmp(argv[1],"--hello")) client_messages=fopen(argv[2],"wb");
        else server_messages=fopen(argv[2],"rb");
        assert(client_messages || server_messages);
        char *args[]={"demo","--channel-fd=3","--fault=none","--test=crash-isolation",NULL};
        expected_fd=3; int result=demo_main(4,args);
        assert(!result && !last_serial && !grant_checks && !forged_checks);
        assert(!fclose(client_messages ? client_messages : server_messages));return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--fault-run")) {
        assert(argc==3);fault_mode=1;expected_fd=5;
        char kind[64];snprintf(kind,sizeof(kind),"--fault=%s",argv[2]);
        char *args[]={"demo","--test=crash-isolation","--channel-fd=5",kind,NULL};
        return demo_main(4,args);
    }
    if (argc>1 && !strcmp(argv[1],"--summary")) { demo_report(2,100,0,7);return 0; }
    if (argc>1 && !strcmp(argv[1],"--arguments")) return demo_main(argc-1,argv+1);
    char *args[]={"demo","--test=crash-isolation","--channel-fd=5","--fault=none",NULL};
    expected_fd=5;
    int result=demo_main(4,args);
    assert(!result && turn_reports==2 && last_serial==100 && !grant_checks && !forged_checks);
    puts("demo 100 acknowledged turns, pause and snapshot barrier PASS");return 0;
}
