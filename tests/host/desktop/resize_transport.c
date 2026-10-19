/* Small channel/surface replay: duplicated received fds are read-only;
 * mappings and descriptors independently retain backing allocations.
 * SPDX-License-Identifier: MIT */
#include "resize_transport.h"
#include <ciuki/channel.h>
#include <ciuki/surface.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
struct surface { struct ciuki_surface_info info; uint32_t *pixels; unsigned refs, maps; };
static struct surface surfaces[128];
static struct { struct surface *s; int writable; } fds[256];
static struct ciuki_message in[256], out[256];
static unsigned in_head, in_count, out_head, out_count;
int rt_block, rt_fail_create, rt_fail_map, rt_eof;
unsigned rt_surfaces(void) { unsigned n=0; for(unsigned i=0;i<128;i++) n+=surfaces[i].pixels!=NULL; return n; }
unsigned rt_mappings(void) { unsigned n=0; for(unsigned i=0;i<128;i++) n+=surfaces[i].maps; return n; }
unsigned rt_handles(void) { unsigned n=0; for(unsigned i=0;i<256;i++) n+=fds[i].s!=NULL; return n; }
static void release(struct surface *s)
{ if (!s->refs && !s->maps) { free(s->pixels); memset(s,0,sizeof(*s)); } }
void rt_reset(void)
{
    assert(!rt_surfaces() && !rt_handles() && !rt_mappings());
    assert(!in_count && !out_count);
    memset(fds,0,sizeof(fds)); in_head=out_head=0;
    rt_block=rt_fail_create=rt_fail_map=rt_eof=0;
}
static int fd_new(struct surface *s, int writable)
{
    for(int fd=10;fd<256;fd++) if(!fds[fd].s) {
        fds[fd].s=s; fds[fd].writable=writable; s->refs++; return fd;
    }
    assert(0); return -1;
}
int ciuki_surface_create(uint32_t w,uint32_t h,uint32_t format)
{
    if(rt_fail_create) { errno=ENOMEM; return -1; }
    assert(w && h && w<=2048 && h<=2048 && format==CIUKI_SURFACE_XRGB8888);
    for(unsigned i=0;i<128;i++) if(!surfaces[i].pixels) {
        struct surface *s=&surfaces[i];
        s->info=(struct ciuki_surface_info){24,w,h,w*4,format,(w*h*4+4095)&~4095u};
        s->pixels=calloc(1,s->info.allocation_bytes); assert(s->pixels); return fd_new(s,1);
    }
    assert(0); return -1;
}
int ciuki_surface_info(int fd,struct ciuki_surface_info *info)
{ assert(fd>=10 && fd<256 && fds[fd].s); *info=fds[fd].s->info; return 0; }
void *ciuki_surface_map(int fd,int prot)
{
    assert(fd>=10 && fd<256 && fds[fd].s);
    if(rt_fail_map) { errno=ENOMEM; return MAP_FAILED; }
    if((prot&PROT_WRITE) && !fds[fd].writable) { errno=EACCES; return MAP_FAILED; }
    fds[fd].s->maps++; return fds[fd].s->pixels;
}
int rt_close(int fd)
{
    if(fd==3) {
        /* Closing the replay channel drops messages and their attachments. */
        while(in_count) {
            struct ciuki_message *m=&in[in_head];
            for(unsigned k=0;k<m->fd_count;k++) rt_close(m->fds[k]);
            in_head=(in_head+1)%256;in_count--;
        }
        while(out_count) {
            struct ciuki_message *m=&out[out_head];
            for(unsigned k=0;k<m->fd_count;k++) rt_close(m->fds[k]);
            out_head=(out_head+1)%256;out_count--;
        }
        return 0;
    }
    assert(fd>=10 && fd<256 && fds[fd].s);
    struct surface *s=fds[fd].s; fds[fd].s=NULL; s->refs--; release(s); return 0;
}
int rt_unmap(void *pixels,size_t bytes)
{
    for(unsigned i=0;i<128;i++) if(surfaces[i].pixels==pixels) {
        struct surface *s=&surfaces[i]; assert(s->maps && s->info.allocation_bytes==bytes);
        s->maps--; release(s); return 0;
    }
    assert(0); return -1;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    assert(fd==3 && (flags & DONTWAIT) && !(flags & ~(DONTWAIT | CIUKI_CHANNEL_FOCUS | CIUKI_CHANNEL_UNFOCUS | CIUKI_CHANNEL_INPUT)));
    if(rt_block) { errno=EAGAIN; return -1; }
    assert(out_count<256 && m->fd_count<=1);
    struct ciuki_message copy=*m; copy.sender_pid=42;
    for(unsigned k=0;k<m->fd_count;k++) copy.fds[k]=fd_new(fds[m->fds[k]].s,0);
    out[(out_head+out_count++)%256]=copy; return 0;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    assert(fd==3 && flags==DONTWAIT);
    if(!in_count) { if(rt_eof) return 0; errno=EAGAIN; return -1; }
    *m=in[in_head]; in_head=(in_head+1)%256; in_count--; return 1;
}
int rt_pop(struct ciuki_message *m)
{
    if(!out_count) return 0;
    *m=out[out_head]; out_head=(out_head+1)%256; out_count--; return 1;
}
void rt_push(const struct ciuki_message *m)
{
    assert(in_count<256 && !m->fd_count);
    in[(in_head+in_count)%256]=*m;
    in[(in_head+in_count++)%256].sender_pid=7;
}
static int server_send(int fd,const struct ciuki_message *m,uint32_t flags)
{ assert(fd==3 && (flags & DONTWAIT) && !(flags & ~(DONTWAIT | CIUKI_CHANNEL_FOCUS | CIUKI_CHANNEL_UNFOCUS | CIUKI_CHANNEL_INPUT))); rt_push(m); return 0; }
static int present(int d,int s,const struct ciuki_rect *r) { (void)d;(void)s;(void)r; return 0; }
const struct desk_ops rt_ops={server_send,ciuki_surface_info,ciuki_surface_map,rt_unmap,rt_close,present};
