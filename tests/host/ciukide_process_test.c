/* SPDX-License-Identifier: MIT
 * Exercise the production channel/spawn boundary, including every failure path. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ciuki/spawn.h>
#include <ciuki/channel.h>
#include "../../apps/ciukide/job.h"
static int pipe_calls,fcntl_calls,close_calls,spawn_calls,pipe_fail,fcntl_fail,spawn_fail;
static int descriptor_flags[9];
int ciuki_channel_pair(int *fds)
{
    if (++pipe_calls==pipe_fail) { errno=EMFILE; return -1; }
    fds[0]=2*pipe_calls+3; fds[1]=fds[0]+1; return 0;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{ assert(fd==5 && flags==DONTWAIT); memset(m,0,sizeof(*m)); memcpy(m->data,"hello",5); m->length=5; return 1; }
static int fake_fcntl(int fd,int command,...)
{
    if (++fcntl_calls==fcntl_fail) { errno=EIO; return -1; }
    if (command==F_GETFL) return 0;
    va_list args; va_start(args,command); int value=va_arg(args,int); va_end(args);
    if (command==F_SETFD) descriptor_flags[fd]=value;
    else assert(command==F_SETFL && value==O_NONBLOCK);
    return 0;
}
static int fake_close(int fd) { assert(fd>=5 && fd<=8); ++close_calls; return 0; }
static int fake_waitpid(int pid,int *status,int flags) { assert(pid==42 && flags==WNOHANG); *status=0; return 42; }
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *maps,uint32_t count,uint32_t flags)
{
    assert(!strcmp(path,"/Programs/tcc") && argv[0] && env && count==2 && !flags);
    assert(maps[0].source==6 && maps[0].target==1 && maps[1].source==8 && maps[1].target==2);
    assert(descriptor_flags[5]==FD_CLOEXEC && descriptor_flags[7]==FD_CLOEXEC);
    assert(!descriptor_flags[6] && !descriptor_flags[8]); ++spawn_calls;
    if (spawn_fail) { errno=ENOENT; return -1; } return 42;
}
#define fcntl fake_fcntl
#define close fake_close
#define waitpid fake_waitpid
#include "../../apps/ciukide/process.c"
#undef close
#undef fcntl
#undef waitpid
static void reset(void)
{ pipe_calls=fcntl_calls=close_calls=spawn_calls=pipe_fail=fcntl_fail=spawn_fail=0; memset(descriptor_flags,0,sizeof(descriptor_flags)); }
int main(void)
{
    const struct ide_process_ops *ops=ide_native_process(); int fds[2]={-1,-1};
    char *argv[]={"tcc",NULL}; reset(); assert(ops->start(NULL,"/Programs/tcc",argv,fds)==42);
    assert(fds[0]==5 && fds[1]==7 && close_calls==2 && spawn_calls==1);
    char buffer[512]; assert(ops->read(NULL,fds[0],buffer,sizeof(buffer))==5 && !memcmp(buffer,"hello",5));
    ops->close(NULL,fds[0]); ops->close(NULL,fds[1]); assert(close_calls==4);
    for (int fail=1;fail<=2;++fail) { reset(); pipe_fail=fail;
        assert(ops->start(NULL,"/Programs/tcc",argv,fds)==-EMFILE && !spawn_calls);
        assert(close_calls==(fail==1 ? 0 : 2)); }
    for (int fail=1;fail<=10;++fail) { reset(); fcntl_fail=fail;
        assert(ops->start(NULL,"/Programs/tcc",argv,fds)==-EIO && !spawn_calls && close_calls==4); }
    reset(); spawn_fail=1; assert(ops->start(NULL,"/Programs/tcc",argv,fds)==-ENOENT && close_calls==4);
    puts("CiukIDE channel/spawn descriptor cleanup: PASS"); return 0;
}
