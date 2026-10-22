/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "gate.h"
#include "theme.h"
#include <ciuki/raw.h>
#include <ciuki/runtime.h>
#include <ciuki/channel.h>
#include <ciuki/surface.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define WIDTH 240u
#define HEIGHT 160u
static unsigned protocol_version=DESK_VERSION;
static void handler(int signal_number)
{
    (void)signal_number;
    *(volatile uint32_t *)(uintptr_t)0x30000000u = 1;
}
static int send_wait(int fd, const struct ciuki_message *m)
{
    /* Ordinary client may block; only the desktop must isolate backpressure. */
    int r;
    struct ciuki_message out=*m;
    out.data[0]=(uint8_t)protocol_version;
    do { r=ciuki_channel_send(fd,&out,0); } while (r<0 && errno==EINTR);
    return r;
}
static void damage(int channel)
{
    struct ciuki_message m;
    desk_message(&m,DESK_DAMAGE,20);
    desk_put32(m.data+12,WIDTH); desk_put32(m.data+16,HEIGHT);
    send_wait(channel,&m);
}
static void text(uint32_t *pixels, unsigned x, unsigned y, const char *s)
{
    struct desk_paint p = {pixels, WIDTH, HEIGHT, 0, 0, WIDTH, HEIGHT};
    desk_paint_text(&p, DESK_FONT_MONO, (int)x, (int)y + desk_font_cap(DESK_FONT_MONO), s, 0x213c4cu);
}
static void draw_region(uint32_t *pixels, const char *typed, unsigned motion, unsigned buttons, int y0, int y1)
{
    for (int y=y0;y<y1;y++) for (unsigned x=0;x<WIDTH;x++)
        pixels[y*WIDTH+x]=y<58 ? 0xe8d7b9u : ((x/16+y/16)&1) ? 0x607a85u : 0x375564u;
    if (!y0) text(pixels,8,8,"Type here:");
    text(pixels,8,22,typed);
    char s[64]; snprintf(s,sizeof(s),"Motion %u  Buttons %u",motion,buttons); text(pixels,8,40,s);
}
static void draw(uint32_t *pixels, const char *typed, unsigned motion, unsigned buttons)
{ draw_region(pixels,typed,motion,buttons,0,HEIGHT); }
static void demo_report(unsigned stage, unsigned turns, unsigned unauthorized, unsigned generation)
{
    char line[CIUKI_PROBE_REPORT_MAX+1];
    int n=snprintf(line,sizeof(line),"case=native-demo stage=%u turns=%u unauthorized=%u generation=%u",stage,turns,unauthorized,generation);
    int error=n<=0 || n>CIUKI_PROBE_REPORT_MAX ? EINVAL : ciuki_error(ciuki_raw_probe_report(CU_PTR(line),n,0,0,0,0));
    if (error) { fprintf(stderr,"demo: gate report failed length=%d error=%d\n",n,error); _exit(126); }
}
static void fault(int channel, int surface, const char *kind, int gate)
{
    if (!strcmp(kind,"none")) return;
    /* PING is queued and its reply has not been consumed: transaction pending.
     * CONFIGURE receipt before this proves the surface was accepted. */
    if (!strcmp(kind,"bad-pointer")) {
        struct ciuki_message *bad=(void *)(uintptr_t)0x30000000u;
        int r=ciuki_channel_recv(channel,bad,DONTWAIT);
        if (r!=-1 || (errno!=EFAULT && errno!=EAGAIN)) _exit(71);
        if (gate) demo_report(3,0,0,0);
        *(volatile uint32_t *)(uintptr_t)0x30000000u=1;
    } else if (!strcmp(kind,"closed-peer")) {
        int pair[2];
        if (ciuki_channel_pair(pair)) _exit(71);
        close(pair[1]);
        struct ciuki_message m;
        desk_message(&m,DESK_PING,8);
        if (ciuki_channel_send(pair[0],&m,DONTWAIT)!=-1 || errno!=EPIPE) _exit(71);
        if (gate) {
            demo_report(3,0,0,0);
            if (signal(SIGPIPE,SIG_DFL)==SIG_ERR) _exit(71);
            ciuki_channel_send(pair[0],&m,DONTWAIT); _exit(71);
        }
        close(pair[0]);
        close(channel);
        if (ciuki_channel_recv(channel,&m,DONTWAIT)!=-1 || errno!=EBADF) _exit(71);
        _exit(70);
    } else if (!strcmp(kind,"forged-fd") || !strcmp(kind,"grant-fd")) {
        struct ciuki_message m;
        if (!strcmp(kind,"grant-fd")) {
            /* Forge display/input operations on an owned endpoint. An actual
             * grant cannot be inherited; wrong descriptor kinds must be EBADF. */
            struct ciuki_framebuffer_info display={.size=sizeof(display)};
            struct ciuki_input_event event;
            struct ciuki_rect rect={0,0,0,0,1,1};
            if (ciuki_display_info(channel,&display)!=-1 || errno!=EBADF ||
                ciuki_present(channel,surface,&rect)!=-1 || errno!=EBADF ||
                ciuki_input_read(channel,&event,1)!=-1 || errno!=EBADF) _exit(71);
        }
        desk_message(&m,DESK_PING,8); m.fd_count=1;
        m.fds[0]=!strcmp(kind,"forged-fd") ? 63 : channel;
        int r=ciuki_channel_send(channel,&m,DONTWAIT);
        if (r!=-1 || errno!=EBADF) _exit(71);
        /* channel/grant attachment rejection uses the same kernel kind check;
         * real grant-fd injection is supervisor-owned (grants cannot inherit). */
        if (gate) {
            demo_report(3,0,0,0);
            *(volatile uint32_t *)(uintptr_t)0x30000000u=1;
        }
        close(surface); _exit(70);
    } else if (!strcmp(kind,"handler-fault")) {
        if (gate) demo_report(3,0,0,0);
        if (signal(SIGUSR1,handler)==SIG_ERR || raise(SIGUSR1)) _exit(71);
        _exit(71);
    }
    _exit(71);
}
int main(int argc, char **argv)
{
    if (signal(SIGPIPE,SIG_IGN)==SIG_ERR) return 1;
    int channel=3, gate=0;
    const char *kind="none";
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i],"--test=crash-isolation")) gate=1;
        else if (!strncmp(argv[i],"--channel-fd=",13)) {
            char *end; long n=strtol(argv[i]+13,&end,10);
            if (!argv[i][13] || *end || n<3 || n>63) return 2;
            channel=(int)n;
        } else if (!strncmp(argv[i],"--fault=",8)) kind=argv[i]+8;
        else return 2;
    }
    if (strcmp(kind,"none") && strcmp(kind,"bad-pointer") && strcmp(kind,"closed-peer") &&
        strcmp(kind,"forged-fd") && strcmp(kind,"grant-fd") && strcmp(kind,"handler-fault")) return 2;
    if (!gate) protocol_version=DESK_SHELL_VERSION;
    int surface=ciuki_surface_create(WIDTH,HEIGHT,CIUKI_SURFACE_XRGB8888);
    uint32_t *pixels=surface<0 ? MAP_FAILED : ciuki_surface_map(surface,PROT_READ|PROT_WRITE);
    if (pixels==MAP_FAILED) return 1;
    char typed[37]=""; unsigned chars=0, motion=0, buttons=0;
    draw(pixels,typed,motion,buttons);
    struct ciuki_message m;
    desk_message(&m,DESK_HELLO,4); if (send_wait(channel,&m)) return 1;
    const char *title=!strcmp(kind,"none") ? "Ciuki Demo" : kind;
    unsigned len=(unsigned)strlen(title);
    desk_message(&m,DESK_CREATE_WINDOW,16+len);
    desk_put32(m.data+4,WIDTH); desk_put32(m.data+8,HEIGHT); desk_put32(m.data+12,len);
    memcpy(m.data+16,title,len); m.fd_count=1; m.fds[0]=surface;
    if (send_wait(channel,&m)) return 1;
    unsigned turns=0, target=0; int announced=0, released=0;
    int configured=0, pending=0; uint32_t serial=0, server_pid=0;
    for (int open=1; open;) {
        /* A turn handles the queued input events and the protocol message after them, then redraws once
         * (f2-65). With one message per turn and a redraw per input event, a close request queued behind
         * pointer motion waited many turns; on a loaded system (image eb774bb9) the desktop's DESK_CLOSE_NS
         * timeout terminated the demo first. A protocol message still ends the turn, as before, and one
         * channel queue at most is read per turn. */
        int dirty=0;
        for (unsigned n=0; n<CIUKI_CHANNEL_QUEUE_MAX; n++) {
            int r=ciuki_channel_recv(channel,&m,DONTWAIT);
            if (!r) { open=0; break; }
            if (r<0) { if (errno!=EAGAIN && errno!=EINTR) return 1; break; }
            if (gate && m.length==12 && !m.fd_count && desk_get32(m.data)==GATE_MAGIC && m.sender_pid==server_pid) {
                unsigned command=desk_get32(m.data+4);
                if (command==GATE_RUN && !strcmp(kind,"none") && !pending) { target=turns+100; announced=0; }
                else if (command==GATE_SNAPSHOT && !strcmp(kind,"none") && !pending && turns==target)
                    demo_report(2,turns,0,desk_get32(m.data+8));
                else if (command==GATE_RELEASE && strcmp(kind,"none")) released=1;
                else return 1;
                continue;
            }
            struct desk_packet p;
            if (!m.sender_pid || m.data[0]!=protocol_version ||
                (server_pid && m.sender_pid!=server_pid) || !desk_parse(&m,0,NULL,&p)) return 1;
            server_pid=m.sender_pid;
            if (p.opcode==DESK_CLOSED) { open=0; break; }
            if (p.opcode==DESK_CLOSE_REQUEST) {
                desk_message(&m,DESK_CLOSE,4);
                if (send_wait(channel,&m)) return 1;
                open=0; break;
            }
            if (p.opcode==DESK_CONFIGURE) {
                if (p.width!=WIDTH || p.height!=HEIGHT) return 1;
                configured=1; damage(channel);
            } else if (p.opcode==DESK_PING) {
                desk_message(&m,DESK_PONG,8); desk_put32(m.data+4,p.serial);
                if (send_wait(channel,&m)) return 1;
            } else if (p.opcode==DESK_PONG && p.serial==serial && pending) { pending=0; turns++; }
            else if (p.opcode==DESK_KEY || p.opcode==DESK_MOTION || p.opcode==DESK_BUTTON) {
                if (p.event.type==CIUKI_INPUT_TEXT && p.event.code>=32 && p.event.code<=126) {
                    if (chars==36) chars=0;
                    typed[chars++]=(char)p.event.code; typed[chars]=0;
                } else if (p.event.type==CIUKI_INPUT_KEY && p.event.code==0x0e && p.event.value && chars)
                    typed[--chars]=0;
                else if (p.opcode==DESK_MOTION) motion++;
                else if (p.opcode==DESK_BUTTON) buttons++;
                dirty=1;
                continue;
            }
            break;
        }
        if (!open) break;
        if (dirty) {
            draw_region(pixels,typed,motion,buttons,20,58);
            desk_message(&m,DESK_DAMAGE,20); desk_put32(m.data+8,20); desk_put32(m.data+12,WIDTH); desk_put32(m.data+16,38);
            if (send_wait(channel,&m)) return 1;
        }
        if (gate && configured && !pending && !announced && turns==target) {
            demo_report(!strcmp(kind,"none") ? 2 : 1,turns,0,0); announced=1;
        }
        if (configured && !pending && (gate ? turns<target || released : strcmp(kind,"none")!=0)) {
            desk_message(&m,DESK_PING,8); desk_put32(m.data+4,++serial);
            if (send_wait(channel,&m)) break;
            pending=1; fault(channel,surface,kind,gate);
        }
        if (gate) { /* explicit diagnostic workload/controller cadence */
            struct timespec pause={.tv_sec=0,.tv_nsec=1000000};nanosleep(&pause,NULL);
        } else {
            struct pollfd fd={channel,POLLIN,0};
            int r=poll(&fd,1,-1);
            if ((r<0 && errno!=EINTR) || (fd.revents&POLLNVAL)) return 1;
        }
    }
    struct ciuki_surface_info info;
    if (!ciuki_surface_info(surface,&info)) munmap(pixels,info.allocation_bytes);
    close(surface); close(channel); return 0;
}
