/* SPDX-License-Identifier: MIT */
#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>
#define INET_ADDRSTRLEN 16
#ifdef __cplusplus
extern "C" {
#endif
int inet_pton(int, const char *, void *);
const char *inet_ntop(int, const void *, char *, socklen_t);
int inet_aton(const char *, struct in_addr *);
char *inet_ntoa(struct in_addr);
in_addr_t inet_addr(const char *);
#ifdef __cplusplus
}
#endif
#endif
