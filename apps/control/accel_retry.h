/* SPDX-License-Identifier: MIT -- private action shared with first boot.
 * Only re-arm the kernel's existing marker. Activation remains a boot-time
 * kernel operation; a successful return requires a restart to take effect. */
#ifndef CIUKI_ACCEL_RETRY_H
#define CIUKI_ACCEL_RETRY_H
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static inline int cp_accel_retry_at(const char *marker)
{
    int fd=open(marker,O_WRONLY|O_CLOEXEC);if(fd<0)return -errno;
    int r=ftruncate(fd,0)?-errno:0;
    if(!r && fsync(fd))r=-errno;
    if(close(fd)&&!r)r=-errno;
    return r;
}
#endif
