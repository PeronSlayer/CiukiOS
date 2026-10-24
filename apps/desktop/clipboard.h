/* SPDX-License-Identifier: MIT */
#ifndef DESK_CLIPBOARD_H
#define DESK_CLIPBOARD_H
#include "desktop.h"
void desk_clip_receive(struct desktop *, int, const struct ciuki_message *, const struct desk_packet *);
void desk_clip_tick(struct desktop *);
void desk_clip_drop(struct desktop *, int);
void desk_clip_destroy(struct desktop *);
int desk_clip_local(void *, struct desk_clip_wire *, const void *, struct desk_clip_wire *, void **);
int desk_clip_capture(struct desktop *, int active);
int desk_clip_key(struct desktop *, int code, int down, int alt);
#endif
