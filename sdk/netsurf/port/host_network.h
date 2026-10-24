/* NetSurf host replay only: native socket declarations after guest ABI types.
 * Keep this out of surface/desktop fixtures with private bind/sendmsg names.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_HOST_NETWORK_H
#define CIUKI_NETSURF_HOST_NETWORK_H
#include "tests/host/desktop/replay_base.h"
#undef SOCK_STREAM
#undef SOCK_DGRAM
#undef SOCK_RAW
#undef SOCK_NONBLOCK
#undef SOCK_CLOEXEC
#undef IPPROTO_IP
#undef IPPROTO_ICMP
#undef IPPROTO_TCP
#undef IPPROTO_UDP
#undef MSG_TRUNC
#undef MSG_PEEK
#undef MSG_DONTWAIT
#undef MSG_NOSIGNAL
#undef SHUT_RD
#undef SHUT_WR
#undef SHUT_RDWR
#include <sys/socket.h>
#include <netinet/in.h>
#endif
