/* SPDX-License-Identifier: MIT */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/time.h>
#define FD_SETSIZE 128
typedef struct { uint32_t bits[4]; } fd_set;
#define FD_ZERO(s) ((void)((s)->bits[0]=(s)->bits[1]=(s)->bits[2]=(s)->bits[3]=0))
#define FD_SET(n,s) ((void)((s)->bits[(unsigned)(n)/32] |= 1u << ((unsigned)(n)%32)))
#define FD_CLR(n,s) ((void)((s)->bits[(unsigned)(n)/32] &= ~(1u << ((unsigned)(n)%32))))
#define FD_ISSET(n,s) (((s)->bits[(unsigned)(n)/32] >> ((unsigned)(n)%32)) & 1u)
#ifdef __cplusplus
extern "C" {
#endif
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
#ifdef __cplusplus
}
#endif
#endif
