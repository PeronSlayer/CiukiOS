/* Existing public /dev/net snapshot; f6-16 Amendment 1 reserves syscall 90.
 * SPDX-License-Identifier: MIT */
#include <ciuki/net.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
int ciuki_net_status(struct ciuki_net_state *status)
{
    if (!status) { errno = EFAULT; return -1; }
    int fd = open("/dev/net", O_RDONLY);
    if (fd < 0) return -1;
    *status = (struct ciuki_net_state){ .version=CIUKI_NET_VERSION, .size=sizeof(*status) };
    int result = ciuki_devctl(fd, NET_GET_STATE, status, sizeof(*status));
    int saved = errno;
    int closed = close(fd);
    if (result < 0) { errno = saved; return -1; }
    return closed < 0 ? -1 : result;
}
