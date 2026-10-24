/* SPDX-License-Identifier: MIT */
#ifndef DOSBOX_CLIPBOARD_H
#define DOSBOX_CLIPBOARD_H
#include "../desktop/protocol.h"
void dos_clip_init(int channel, uint32_t *pixels);
void dos_clip_settings(const char *program);
int dos_clip_service(void);
void dos_clip_vm(uint32_t id, uint32_t generation, uint32_t server);
int dos_clip_recv(struct ciuki_message *);
int dos_clip_action(unsigned action); /* 1 Mark, 2 Copy, 3 Paste */
int dos_clip_input(const struct desk_packet *);
void dos_clip_tick(int focused);
int dos_clip_marking(void);
int dos_clip_dirty(void);
void dos_clip_destroy(void);
#endif
