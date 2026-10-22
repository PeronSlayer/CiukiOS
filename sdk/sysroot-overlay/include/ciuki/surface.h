/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SURFACE_H
#define CIUKI_SURFACE_H
#include <ciuki/abi.h>
int ciuki_surface_create(uint32_t,uint32_t,uint32_t);
void *ciuki_surface_map(int,int);
int ciuki_surface_info(int,struct ciuki_surface_info *);
int ciuki_present(int,int,const struct ciuki_rect *);
/* Returns the service result or a negative errno, without errno translation. */
int ciuki_display_accel(int,const struct ciuki_accel_request *);
/* Read-only snapshot; returns zero or a negative errno. */
int ciuki_display_accel_info(int,uint32_t,struct ciuki_accel_info *);
int ciuki_display_engine_info(int,uint32_t,struct ciuki_display_engine_info *);
int ciuki_display_info(int,struct ciuki_framebuffer_info *);
int ciuki_input_read(int,struct ciuki_input_event *,uint32_t);
#endif
