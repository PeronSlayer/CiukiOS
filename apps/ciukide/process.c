/* SPDX-License-Identifier: MIT */
#include "job.h"
#include <ciuki/spawn.h>
#include <ciuki/channel.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static int start(void *ctx, const char *compiler, char *const argv[], int fds[2])
{
    (void)ctx; int channels[2][2]={{-1,-1},{-1,-1}}, pid=-1, error=0;
    if (ciuki_channel_pair(channels[0]) || ciuki_channel_pair(channels[1])) { error=errno; goto done; }
    for (unsigned i=0;i<2;++i) {
        for (unsigned k=0;k<2;++k) if (fcntl(channels[i][k],F_SETFD,FD_CLOEXEC)) { error=errno; goto done; }
        int flags=fcntl(channels[i][0],F_GETFL);
        if (flags<0 || fcntl(channels[i][0],F_SETFL,flags|O_NONBLOCK)) { error=errno; goto done; }
        /* The spawn ABI inherits only fd 0-2 and explicit mappings; mapped
           sources must not be CLOEXEC. Originals are not inherited. */
        if (fcntl(channels[i][1],F_SETFD,0)) { error=errno; goto done; }
    }
    struct ciuki_spawn_fd maps[2]={{channels[0][1],1},{channels[1][1],2}};
    pid=ciuki_spawn(compiler,argv,environ,maps,2,0);
    if (pid<0) { error=errno; goto done; }
    for (unsigned i=0;i<2;++i) { fds[i]=channels[i][0]; channels[i][0]=-1; }
done:
    for (unsigned i=0;i<2;++i) for (unsigned k=0;k<2;++k) if (channels[i][k]>=0) close(channels[i][k]);
    return pid<0 ? -(error ? error : EIO) : pid;
}
static ssize_t take(void *ctx,int fd,char *b,size_t n)
{
    (void)ctx; struct ciuki_message m;
    int r=ciuki_channel_recv(fd,&m,DONTWAIT); if (r<=0) return r;
    if (m.fd_count || m.length>n) {
        for (unsigned i=0;i<m.fd_count && i<CIUKI_MESSAGE_FD_MAX;++i) close(m.fds[i]);
        errno=EIO; return -1;
    }
    memcpy(b,m.data,m.length); return (ssize_t)m.length;
}
static int reap(void *ctx,int pid,int *status) { (void)ctx; int r=waitpid(pid,status,WNOHANG); return r<0 ? -errno : r; }
static void shut(void *ctx,int fd) { (void)ctx; close(fd); }
const struct ide_process_ops *ide_native_process(void)
{ static const struct ide_process_ops ops={start,take,reap,shut}; return &ops; }
