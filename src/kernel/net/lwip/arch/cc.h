/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_LWIP_CC_H
#define CIUKI_LWIP_CC_H
#include <stdint.h>
#include <stddef.h>
#ifdef LWIP_HOST
#include <sys/types.h>
/* Native host pointers require eight-byte allocator alignment. Protocol and
 * pool-count options stay identical; target measurements use i686 objects. */
#define LWIP_MEM_ALIGN_SIZE(n) (((n) + 7u) & ~7u)
#define LWIP_MEM_ALIGN_BUFFER(n) ((n) + 7u)
#define LWIP_MEM_ALIGN(p) ((void *)(((uintptr_t)(p) + 7u) & ~(uintptr_t)7u))
#endif
#define BYTE_ORDER LITTLE_ENDIAN
/* Existing physical-driver seam owns netif_poll. */
#define netif_poll ciuki_lwip_netif_poll
#define atoi ciuki_lwip_atoi
#define strcmp ciuki_lwip_strcmp
int ciuki_lwip_atoi(const char *);
int ciuki_lwip_strcmp(const char *, const char *);
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_STDIO_H 1
#define LWIP_NO_LIMITS_H 0
#define LWIP_PLATFORM_DIAG(x) ((void)0)
#define LWIP_PLATFORM_ASSERT(message) ciuki_lwip_assert_fail(message)
void ciuki_dns_servers_changed(void);
void ciuki_lwip_assert_fail(const char *);
void ciuki_lwip_assert_core(void);
void ciuki_lwip_mark_core(void);
void ciuki_lwip_alive(void);
uint32_t ciuki_lwip_rand(void);
void ciuki_lwip_seed(uint64_t);
typedef uintptr_t sys_prot_t;
#endif
