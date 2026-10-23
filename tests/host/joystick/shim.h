/* Architectural operations for host models. SPDX-License-Identifier: GPL-2.0-only */
#ifndef JOYSTICK_HOST_SHIM_H
#define JOYSTICK_HOST_SHIM_H
#define CIUKI_CPU_H
#include <stdint.h>
#include <assert.h>
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
extern unsigned joystick_irq_depth;
static inline uint32_t irq_save(void) { return joystick_irq_depth++; }
static inline void irq_restore(uint32_t flags) { joystick_irq_depth=flags; }
static inline uint8_t inb(uint16_t p) { (void)p; assert(!"physical port read"); return 0; }
static inline void outb(uint16_t p, uint8_t v) { (void)p; (void)v; assert(!"physical port write"); }
#endif
