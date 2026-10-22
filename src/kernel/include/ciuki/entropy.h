/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ENTROPY_H
#define CIUKI_ENTROPY_H
#include <stdint.h>
#include <stddef.h>
struct trap_frame;
void *entropy_alloc(size_t);
void entropy_free(void *);
void entropy_boot(void);
void entropy_irq(unsigned irq);
void entropy_start(void);
/* f6-21: the desktop presented its first full frame, or no desktop frame
 * will come. Gates the jitter collector's runtime retries. Any context. */
void entropy_desktop_presented(void) __attribute__((weak));
int entropy_get(void *, size_t);
int32_t entropy_syscall(struct trap_frame *);
#endif
