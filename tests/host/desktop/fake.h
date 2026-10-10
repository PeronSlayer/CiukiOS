/* SPDX-License-Identifier: MIT */
#ifndef DESKTOP_FAKE_H
#define DESKTOP_FAKE_H
#include "desktop.h"
extern const struct desk_ops fake_ops;
extern unsigned fake_closed, fake_unmapped, fake_sent, fake_presents;
extern int fake_block, fake_fail_map, fake_fail_present;
extern int fake_closed_peer;
extern struct ciuki_message fake_messages[2048];
extern int fake_destinations[2048];
extern struct ciuki_rect fake_rects[64];
void fake_reset(void);
void fake_channel(int);
void fake_surface(int, uint32_t, uint32_t, uint32_t);
unsigned fake_live(void);
#endif
