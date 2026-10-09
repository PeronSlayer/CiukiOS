/* Common kernel declarations for Ciuki VMM.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_KERNEL_H
#define CIUKI_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "boot_info.h"

#define PAGE_SIZE       4096u
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))

/* errors, returned negated (docs/design/execution-abi.md) */
#define EFAULT  14
#define EINVAL  22
#define ENOSPC  28
#define ENOSYS  38
#define ENOMEM  12

/* lib */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);
int ksnprintf(char *buf, size_t size, const char *fmt, ...);
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
uint32_t fnv1a32(const void *data, size_t len, uint32_t seed);

/* boot info (copied, validated) */
extern struct ciuki_boot_info g_boot;
int bootinfo_validate(const struct ciuki_boot_info *bi, const char **why);

/* output sinks */
void serial_init(uint16_t base, uint16_t divisor);
bool serial_present(void);
void serial_write(const char *s, size_t n);
void console_init_text(void);
bool console_init_lfb(void);
void console_write(const char *s, size_t n);
void console_set_color(uint32_t fg, uint32_t bg);
void klog(const char *fmt, ...);          /* serial + screen, plain log line */
void kputs_raw(const char *s);            /* panic-safe output */

/* panic */
struct trap_frame;
__attribute__((noreturn)) void panic(const char *fmt, ...);
__attribute__((noreturn)) void panic_frame(struct trap_frame *tf, const char *why);

/* time */
extern volatile uint64_t g_ticks;         /* PIT ticks, nominal 1000 Hz */
void pit_init(void);
uint32_t ticks_lo(void);

/* cpu features */
extern bool g_cpu_fxsr, g_cpu_sse, g_cpu_pse, g_cpu_pge, g_cpu_tsc;
extern char g_cpu_vendor[13];
extern uint32_t g_cpu_signature;
void cpu_detect(void);

#endif
