/* SPDX-License-Identifier: MIT
 * The kernel ABI's socket numbers (ciuki/abi.h, G26) are macros; glibc declares the same names as enum members.
 * Host fixtures that include <sys/socket.h> or <netdb.h> after the ABI header drop the macros first. */
#undef AF_UNSPEC
#undef AF_INET
#undef SOCK_STREAM
#undef SOCK_DGRAM
#undef SOCK_RAW
#undef SOCK_NONBLOCK
#undef SOCK_CLOEXEC
#undef IPPROTO_IP
#undef IPPROTO_ICMP
#undef IPPROTO_TCP
#undef IPPROTO_UDP
#undef IP_TTL
#undef SHUT_RD
#undef SHUT_WR
#undef SHUT_RDWR
#undef MSG_TRUNC
#undef MSG_PEEK
#undef MSG_DONTWAIT
#undef MSG_NOSIGNAL
#undef SOL_SOCKET
#undef SO_REUSEADDR
#undef SO_TYPE
#undef SO_ERROR
#undef SO_BROADCAST
#undef SO_RCVTIMEO
#undef SO_SNDTIMEO
#undef SO_RX_BYTES
#undef TCP_NODELAY
