/* SPDX-License-Identifier: MIT */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#include <sys/types.h>
enum { __ciuki_map_failed=MAP_FAILED };
#undef MAP_FAILED
#define MAP_FAILED ((void *)(uintptr_t)__ciuki_map_failed)
void *mmap(void *,size_t,int,int,int,off_t);
int munmap(void *,size_t);
int mprotect(void *,size_t,int);
#endif
