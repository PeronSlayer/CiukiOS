/* SPDX-License-Identifier: MIT
 * Private representation of NetSurf's fdset callback contract, not select(). */
#ifndef CIUKI_NETSURF_FDSET_H
#define CIUKI_NETSURF_FDSET_H
#include <stdint.h>
#include <string.h>
#define FD_SETSIZE 64
typedef struct { uint32_t bits[2]; } fd_set;
#define FD_ZERO(set) memset((set),0,sizeof(fd_set))
static inline void ciuki_fd_set(int fd,fd_set *set)
{ if(fd>=0 && fd<FD_SETSIZE)set->bits[(unsigned)fd/32]|=1u<<((unsigned)fd%32); }
static inline int ciuki_fd_isset(int fd,const fd_set *set)
{ return fd>=0 && fd<FD_SETSIZE && !!(set->bits[(unsigned)fd/32]&(1u<<((unsigned)fd%32))); }
#define FD_SET(fd,set) ciuki_fd_set((fd),(set))
#define FD_ISSET(fd,set) ciuki_fd_isset((fd),(set))
#endif
