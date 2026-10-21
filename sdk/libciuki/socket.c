/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include <errno.h>
static int result(uint32_t r) { int e = ciuki_error(r); if (e) { errno = e; return -1; } return (int32_t)r; }
int socket(int d, int t, int p) { return result(CU_CALL(SOCKET,d,t,p,0,0,0)); }
int bind(int fd,const struct sockaddr *a,socklen_t n) { return result(CU_CALL(BIND,fd,CU_PTR(a),n,0,0,0)); }
int connect(int fd,const struct sockaddr *a,socklen_t n) { return result(CU_CALL(CONNECT,fd,CU_PTR(a),n,0,0,0)); }
int listen(int fd,int n) { return result(CU_CALL(LISTEN,fd,n,0,0,0,0)); }
int accept(int fd,struct sockaddr *a,socklen_t *n) { return result(CU_CALL(ACCEPT,fd,CU_PTR(a),CU_PTR(n),0,0,0)); }
int shutdown(int fd,int how) { return result(CU_CALL(SHUTDOWN,fd,how,0,0,0,0)); }
int getsockname(int fd,struct sockaddr *a,socklen_t *n) { return result(CU_CALL(SOCKNAME,fd,0,CU_PTR(a),CU_PTR(n),0,0)); }
int getpeername(int fd,struct sockaddr *a,socklen_t *n) { return result(CU_CALL(SOCKNAME,fd,1,CU_PTR(a),CU_PTR(n),0,0)); }
ssize_t sendto(int fd,const void *data,size_t bytes,int flags,const struct sockaddr *a,socklen_t n)
{
    struct ciuki_socket_io io = { .size=sizeof(io), .version=1, .fd=fd, .buffer=CU_PTR(data), .bytes=bytes,
        .flags=(uint32_t)flags, .address=CU_PTR(a), .address_length=n };
    return result(CU_CALL(SENDTO,CU_PTR(&io),0,0,0,0,0));
}
ssize_t recvfrom(int fd,void *data,size_t bytes,int flags,struct sockaddr *a,socklen_t *n)
{
    struct ciuki_socket_io io = { .size=sizeof(io), .version=1, .fd=fd, .buffer=CU_PTR(data), .bytes=bytes,
        .flags=(uint32_t)flags, .address=CU_PTR(a), .address_length=CU_PTR(n) };
    return result(CU_CALL(RECVFROM,CU_PTR(&io),0,0,0,0,0));
}
ssize_t send(int fd,const void *p,size_t n,int f) { return sendto(fd,p,n,f,0,0); }
ssize_t recv(int fd,void *p,size_t n,int f) { return recvfrom(fd,p,n,f,0,0); }
int setsockopt(int fd,int level,int name,const void *value,socklen_t n)
{
    struct ciuki_socket_option r = { .size=sizeof(r), .version=1, .fd=fd, .operation=1,
        .level=level, .name=name, .value=CU_PTR(value), .length=n };
    return result(CU_CALL(SOCKOPT,CU_PTR(&r),0,0,0,0,0));
}
int getsockopt(int fd,int level,int name,void *value,socklen_t *n)
{
    struct ciuki_socket_option r = { .size=sizeof(r), .version=1, .fd=fd,
        .level=level, .name=name, .value=CU_PTR(value), .length=CU_PTR(n) };
    return result(CU_CALL(SOCKOPT,CU_PTR(&r),0,0,0,0,0));
}
int poll(struct pollfd *fds,nfds_t n,int timeout) { return result(CU_CALL(POLL,CU_PTR(fds),n,timeout,0,0,0)); }
int select(int nfds,fd_set *read,fd_set *write,fd_set *except,struct timeval *timeout)
{
    if (nfds < 0 || nfds > FD_SETSIZE) { errno=EINVAL; return -1; }
    if (except && (except->bits[0] || except->bits[1] || except->bits[2] || except->bits[3])) { errno=EOPNOTSUPP; return -1; }
    int ms=-1;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_sec > INT32_MAX/1000 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000 || timeout->reserved) { errno=EINVAL; return -1; }
        int64_t n=timeout->tv_sec*1000+(timeout->tv_usec+999)/1000;
        if (n>INT32_MAX) { errno=EINVAL; return -1; } ms=(int)n;
    }
    struct pollfd fds[FD_SETSIZE]; unsigned count=0;
    for (int fd=0;fd<nfds;fd++) {
        int events=(read && FD_ISSET(fd,read) ? POLLIN : 0) | (write && FD_ISSET(fd,write) ? POLLOUT : 0);
        if (events) fds[count++]=(struct pollfd){fd,(int16_t)events,0};
    }
    int n=poll(fds,count,ms); if (n<0) return -1;
    for (unsigned i=0;i<count;i++) if (fds[i].revents&POLLNVAL) { errno=EBADF; return -1; }
    if (read) FD_ZERO(read); if (write) FD_ZERO(write); if (except) FD_ZERO(except);
    int ready=0;
    for (unsigned i=0;i<count;i++) {
        int e=fds[i].revents;
        if (read && (fds[i].events&POLLIN) && (e&(POLLIN|POLLHUP|POLLERR))) { FD_SET(fds[i].fd,read); ready++; }
        if (write && (fds[i].events&POLLOUT) && (e&(POLLOUT|POLLERR|POLLHUP))) { FD_SET(fds[i].fd,write); ready++; }
    }
    return ready;
}
