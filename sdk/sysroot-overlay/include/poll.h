/* SPDX-License-Identifier: MIT */
#ifndef _POLL_H
#define _POLL_H
#include <ciuki/abi.h>
typedef uint32_t nfds_t;
#define pollfd ciuki_pollfd
#ifdef __cplusplus
extern "C" {
#endif
int poll(struct pollfd *, nfds_t, int);
#ifdef __cplusplus
}
#endif
#endif
