/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_KERNEL_DISPLAY_H
#define CIUKI_KERNEL_DISPLAY_H
#include <ciuki/desktop.h>
int32_t display_syscall(struct trap_frame *);
int display_query(struct ciuki_display_info *);
int display_set(struct process *, uint16_t mode);
uint32_t display_change_counter(void);
void display_frame_complete(uint32_t generation);
void display_watchdog_poll(void);
int display_worker_start(void);
int display_fullscreen_begin(uint8_t mode);
int display_fullscreen_end(void);
bool display_fullscreen_active(void);
int grants_display_suspend(bool suspended);
uint32_t grants_display_pid(void);
bool grants_display_owner(const struct process *);
bool grants_display_busy(void);
#endif
