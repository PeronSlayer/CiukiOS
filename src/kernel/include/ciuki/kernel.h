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
/* One queue publication; includes console prefixes and final newline.
 * CRLF expansion needs at most twice this many bytes in the byte ring. */
#define SERIAL_RECORD_MAX 2048u
void serial_write(const char *s, size_t n); /* whole drop at diagnostic capacity/oversize */
/* Zero refused; sent >= ticket means final byte submitted to the UART. */
uint64_t serial_write_ticket(const char *s, size_t n);
/* Wait for this many wire bytes only in preemptible task context. Returns
 * saved EFLAGS with IF clear: caller restores after formatting/publication,
 * so evidence sequence allocation happens AFTER any wait. Protects the next
 * publication with reserved evidence capacity in early/atomic context too
 * (queue bounds still apply). Publish exactly once before restoring IF.
 * Weak for fixtures. */
uint32_t serial_evidence_begin(size_t wire_bytes) __attribute__((weak));
void serial_buffered(bool enabled);       /* compatibility: all normal logs queue */
void serial_writer_init(void);            /* scheduler startup: register/unmask TX IRQ once */
uint32_t serial_dropped(void);
/* Queued counts expanded wire bytes; resent counts software retries, never
 * remote delivery. Unprotected publications use the periodic/diagnostic share. */
struct serial_stats {
    uint64_t accepted, sent;
    uint32_t queued, dropped;
    bool present;
    uint32_t periodic_dropped, evidence_dropped, resent;
};
void serial_snapshot(struct serial_stats *);
void serial_panic(void);                  /* IF clear: drain, then synchronous records */
void serial_tick(void);                   /* IRQ0 idle rearm; UART IRQ alone drains */
void console_init_text(void);
bool console_init_lfb(void);
void console_write(const char *s, size_t n);
void console_worker_init(void);
void console_service(void);               /* one bounded step of a coalesced repaint */
void console_set_color(uint32_t fg, uint32_t bg);
void console_display_owned(bool owned);   /* display grant held: no console pixels */
void console_reclaim(void);               /* panic: the console draws again */
void console_set_quiet(bool on);          /* quiet boot: history and serial only */
bool console_quiet(void);
void console_wake(void);                  /* quiet boot failure: draw from history */
void console_pin_page(const char *);      /* pinned text until panic/reboot; caller retains storage */
void klog(const char *fmt, ...);          /* serial + screen, plain log line */
void klog_periodic(const char *fmt, ...); /* same sinks, disk runtime ring only */
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
