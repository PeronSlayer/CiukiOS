/* SPDX-License-Identifier: MIT */
#ifndef _SYS_ERRNO_H_
#define _SYS_ERRNO_H_
#include <ciuki/abi.h>
#ifdef __cplusplus
extern "C" {
#endif
int *__errno(void);
#define errno (*__errno())
#ifdef __cplusplus
}
#endif
#endif
