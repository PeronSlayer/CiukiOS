/* f5-15C installers-tier driver (installer_title.c). SPDX-License-Identifier: MIT */
#ifndef DESK_INSTALLER_TITLE_H
#define DESK_INSTALLER_TITLE_H
#include <stdint.h>
struct desktop;
int desk_title_begin(struct desktop *, const char *argument); /* "<run><cd>" drive letters */
void desk_title_tick(struct desktop *);
int desk_title_active(void);
void desk_title_notice(unsigned kind, uint32_t pid, uint32_t flags); /* virtual restart notices */
#endif
