/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_FACADE_FILES_H
#define W32_FACADE_FILES_H
#include <w32/base.h>
struct w32v_file { uintptr_t handle; uint32_t size; };
int w32v_open(const void *,int,struct w32v_file *);
int w32v_read(struct w32v_file *,uint32_t,void *,uint32_t);
void w32v_close(struct w32v_file *);
void w32r_last_error(uint32_t);
void *w32lz_alloc(uint32_t);
void w32lz_free(void *);
#endif
