/* SPDX-License-Identifier: MIT
 * Host packet-channel boundary, isolated from guest ABI POSIX constants. */
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
struct ciuki_message;
static int send_error;
void event_channel_fail(int error) { send_error=error; }
int ciuki_channel_pair(int fds[2])
{
    if(socketpair(AF_UNIX,SOCK_SEQPACKET,0,fds))return -1;
    for(unsigned i=0;i<2;i++)if(fcntl(fds[i],F_SETFL,O_NONBLOCK)){close(fds[0]);close(fds[1]);return -1;}
    return 0;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    (void)flags;
    if (send_error) { errno=send_error;send_error=0;return -1; }
    return write(fd,m,288)==288 ? 0 : -1;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    (void)flags;ssize_t n=read(fd,m,288);
    return n==288 ? 1 : n==0 ? 0 : -1;
}
