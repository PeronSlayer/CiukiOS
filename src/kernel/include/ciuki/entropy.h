/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ENTROPY_H
#define CIUKI_ENTROPY_H
#include <stdint.h>
#include <stddef.h>
struct trap_frame;
void entropy_boot(void);
void entropy_irq(unsigned irq);
void entropy_start(void);
int entropy_get(void *, size_t);
int32_t entropy_syscall(struct trap_frame *);
#endif
