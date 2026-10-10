/* SPDX-License-Identifier: MIT */
#include "fake.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
struct fake_fd { int live, mapped; struct ciuki_surface_info info; uint32_t *pixels; };
static struct fake_fd fds[128];
unsigned fake_closed, fake_unmapped, fake_sent, fake_presents;
int fake_block, fake_fail_map, fake_fail_present;
int fake_closed_peer;
struct ciuki_message fake_messages[2048];
int fake_destinations[2048];
struct ciuki_rect fake_rects[64];
void fake_reset(void)
{
    for (int i=0; i<128; i++) { assert(!fds[i].live && !fds[i].mapped); free(fds[i].pixels); }
    memset(fds,0,sizeof(fds));
    fake_closed=fake_unmapped=fake_sent=fake_presents=0;
    fake_block=fake_fail_map=fake_fail_present=0;
    fake_closed_peer=-1;
}
void fake_channel(int fd) { assert(!fds[fd].live); fds[fd].live=1; }
void fake_surface(int fd, uint32_t w, uint32_t h, uint32_t colour)
{
    assert(!fds[fd].live && !fds[fd].mapped);
    free(fds[fd].pixels);
    fake_channel(fd);
    fds[fd].info=(struct ciuki_surface_info){24,w,h,w*4,1,(w*h*4+4095)&~4095u};
    fds[fd].pixels=calloc(1,fds[fd].info.allocation_bytes);
    assert(fds[fd].pixels);
    for (unsigned i=0; i<w*h; i++) fds[fd].pixels[i]=colour;
}
unsigned fake_live(void)
{
    unsigned n=0; for (int i=0; i<128; i++) n+=(unsigned)fds[i].live+(unsigned)fds[i].mapped;
    return n;
}
static int send(int fd, const struct ciuki_message *m, uint32_t flags)
{
    assert(flags==DONTWAIT);
    if (fd==fake_closed_peer) { raise(SIGPIPE); errno=EPIPE; return -1; }
    if (!fds[fd].live) { errno=EPIPE; return -1; }
    if (fake_block) { errno=EAGAIN; return -1; }
    assert(fake_sent<2048 && !m->sender_pid && !m->fd_count);
    struct desk_packet p; assert(desk_parse(m,0,NULL,&p));
    fake_destinations[fake_sent]=fd; fake_messages[fake_sent++]=*m; return 0;
}
static int info(int fd, struct ciuki_surface_info *s)
{
    if (fd<0 || fd>=128 || !fds[fd].pixels || !fds[fd].live) { errno=EBADF; return -1; }
    *s=fds[fd].info; return 0;
}
static void *map(int fd, int prot)
{
    assert(prot==PROT_READ);
    if (fake_fail_map) { errno=ENOMEM; return MAP_FAILED; }
    assert(fds[fd].live && fds[fd].pixels); fds[fd].mapped++; return fds[fd].pixels;
}
static int unmap(void *p, size_t n)
{
    for (int fd=0; fd<128; fd++) if (fds[fd].pixels==p) {
        assert(fds[fd].mapped==1 && n==fds[fd].info.allocation_bytes);
        fds[fd].mapped--; fake_unmapped++; return 0;
    }
    assert(0); return -1;
}
static int close_fd(int fd)
{
    assert(fd>=0 && fd<128 && fds[fd].live); fds[fd].live=0; fake_closed++; return 0;
}
static int present(int display, int surface, const struct ciuki_rect *r)
{
    (void)display; (void)surface;
    if (fake_fail_present) { errno=EINTR; return -1; }
    assert(fake_presents<64); fake_rects[fake_presents++]=*r; return 0;
}
const struct desk_ops fake_ops={send,info,map,unmap,close_fd,present};
