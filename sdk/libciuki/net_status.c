/* Public /dev/net snapshot and privileged f2-100 syscall 90 wrapper.
 * SPDX-License-Identifier: MIT */
#include <ciuki/net.h>
#include <ciuki/runtime.h>
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

int ciuki_net_control(unsigned operation, struct ciuki_net_control *record)
{
    uint32_t r = CU_CALL(NETCTL, operation, CU_PTR(record), sizeof(*record), 0, 0, 0);
    int e = ciuki_error(r); if (e) { errno=e; return -1; } return (int32_t)r;
}
int ciuki_net_info(unsigned operation,void *record,uint32_t bytes)
{
    uint32_t r=CU_CALL(NET_INFO,operation,CU_PTR(record),bytes,0,0,0);
    int e=ciuki_error(r);if(e) { errno=e;return -1; }return (int32_t)r;
}

int ciuki_resolve4_query(struct ciuki_resolve_query *q)
{
    int e=ciuki_error(CU_CALL(RESOLVE4,CU_PTR(q),0,0,0,0,0));
    if(e) { errno=e;return -1; }return 0;
}
