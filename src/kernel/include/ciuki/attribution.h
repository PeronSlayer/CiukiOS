/* Verbose CPU ownership and non-preemptible path windows. GPL-2.0-only */
#ifndef CIUKI_ATTRIBUTION_H
#define CIUKI_ATTRIBUTION_H
#include <stdint.h>
#include <stdbool.h>
struct task;
enum long_path { LONG_NONE, LONG_CONSOLE, LONG_BOOTLOG, LONG_ELF, LONG_SYSCALL, LONG_SERIAL, LONG_COUNT };
struct long_path_scope { unsigned path; const char *site; };
/* Optional in standalone host fixtures. The canonical kernel links the module. */
void attribution_switch(struct task *next) __attribute__((weak));
void attribution_poll(bool force) __attribute__((weak));
struct long_path_scope attribution_enter(unsigned path, const char *site) __attribute__((weak));
void attribution_leave(struct long_path_scope previous) __attribute__((weak));
static inline struct long_path_scope long_path_enter(unsigned path, const char *site)
{
    return attribution_enter ? attribution_enter(path, site) : (struct long_path_scope){0};
}
static inline void long_path_leave(struct long_path_scope previous)
{ if (attribution_leave) attribution_leave(previous); }
#endif
