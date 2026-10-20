/* Socket/platform substitution only; server main and registry are production.
 * SPDX-License-Identifier: MIT */
#include "../../../apps/w32srv/registry.h"
#include <ciuki/channel.h>
#include <ciuki/proc.h>
#include <errno.h>
#include <fcntl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>
static int accepted;
uint32_t host_w32_call(void *record)
{
    struct w32r_service *a=record;
    if(a->action==1){
        prctl(PR_SET_PDEATHSIG,SIGKILL);a->result=1;char byte='R';int ready=atoi(getenv("W32_READY"));
        if(write(ready,&byte,1)!=1)_exit(99);close(ready);
    }else if(a->action==3 && !accepted){
        accepted=1;a->result=1;a->endpoint=(uint32_t)atoi(getenv("W32_SOCKET"));a->pid=(uint32_t)getppid();
    }
    return 0;
}
uint32_t host_w32_W32(void *record,uint32_t bytes){(void)bytes;return host_w32_call(record);}
uint32_t host_w32_DEBUG_WRITE(const void *text,uint32_t bytes){return fwrite(text,1,bytes,stderr)==bytes?0:1;}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    (void)flags;memset(m,0,sizeof(*m));ssize_t n=read(fd,m->data,256);
    if(n<=0){if(n==0 || errno!=EAGAIN)fprintf(stderr,"host recv fd=%d n=%ld errno=%d pid=%ld\n",fd,(long)n,errno,(long)getpid());return (int)n;}
    m->length=(uint32_t)n;m->sender_pid=(uint32_t)getppid();return 1;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{(void)flags;return write(fd,m->data,m->length)==(ssize_t)m->length ? 0 : -1;}
int ciuki_display_query(struct ciuki_display_info *info){(void)info;return -ENOSYS;}
int ciuki_proc_list(void *out,uint32_t size)
{
    if(size<sizeof(struct ciuki_proc_system))return -1;
    struct ciuki_proc_system *system=out;memset(system,0,sizeof(*system));system->version=CIUKI_PROC_VERSION;return 0;
}
void host_posix_io(struct wr_io *,const char *);
void wr_posix_io(struct wr_io *io,const char *directory)
{(void)directory;host_posix_io(io,getenv("W32_HIVES"));}
