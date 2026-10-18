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
/* Entry refused safely: desktop is still/restored writable. No guest lease.
 * Negative entry results mean recovery failed; zero grants full screen. */
#define DISPLAY_FULLSCREEN_WINDOWED 1
int display_fullscreen_begin(uint16_t mode);
int display_fullscreen_end(void);
bool display_fullscreen_supported(uint16_t mode);
int display_fullscreen_write(uint32_t offset, const void *, uint32_t bytes);
bool display_fullscreen_active(void);
/* IRQ safe, records the hotkey/fault time without doing any I/O. */
void display_fullscreen_return_request(void);
/* Boot check only, no grants/desktop; end verifies the original VBE mode. */
int display_check_begin(uint16_t mode);
int display_check_end(void);
/* Pin the temporary mode while the offscreen engine owns VGA/VRAM leases.
 * A deadline requests rollback; only a restored/idle sweep may permit it. */
int display_check_sweep_begin(void);
void display_check_sweep_end(bool restored);
/* Thread context; entry commits the preceding records on hardware. Return
 * queues the watermark for the storage worker without waiting for device I/O. */
int display_fullscreen_checkpoint(const char *step, int result);
int grants_display_suspend(bool suspended);
uint32_t grants_display_pid(void);
bool grants_display_owner(const struct process *);
bool grants_display_busy(void);
#endif
