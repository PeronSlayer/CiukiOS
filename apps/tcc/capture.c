/* SPDX-License-Identifier: MIT
 * Target compiler stdio on the existing message channels. Ordinary console
 * and file output still uses the unchanged SDK newlib write hook. */
#include <ciuki/channel.h>
#include <errno.h>
#include <reent.h>
#include <string.h>
#include <sys/types.h>
extern _ssize_t __real__write_r(struct _reent *,int,const void *,size_t);
_ssize_t __wrap__write_r(struct _reent *reent,int fd,const void *data,size_t size)
{
    if (fd!=1 && fd!=2) return __real__write_r(reent,fd,data,size);
    size_t sent=0; int previous=errno;
    while (sent<size) {
        struct ciuki_message m={0};
        size_t n=size-sent; if (n>sizeof(m.data)) n=sizeof(m.data);
        m.length=(uint32_t)n; memcpy(m.data,(const char *)data+sent,n);
        int r=ciuki_channel_send(fd,&m,0); /* bounded blocking send: IDE drains concurrently */
        if (r<0) {
            if (!sent && errno==EBADF) { errno=previous; return __real__write_r(reent,fd,data,size); }
            if (errno==EINTR) continue;
            reent->_errno=errno;
            return sent ? (_ssize_t)sent : -1;
        }
        sent+=n;
    }
    errno=previous; return (_ssize_t)sent;
}
