/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors.
 *
 * CiukiOS backend of the private w32_services boundary, fail-closed until
 * the f5-03a CIUKI_SYS_W32 record header is frozen and reviewed. No syscall
 * numbers or record layouts are guessed here: every resource request fails,
 * so the loader refuses images with a named reason instead of calling through
 * a NULL pointer, and the noreturn paths stop the thread with ud2.
 * Binding plan (f5-03/f5-05 part B): alloc/free/map/protect/unmap -> the
 * virtual-memory and image-section records; open/close -> image section
 * views of VFS files; thread_id -> TEB ClientId; wait_word/wake_word -> the
 * kernel wait primitive on a user address; diagnostic -> [win32] records;
 * continue_context -> validated "continue with context"; terminate -> exit. */
#include <w32/ntdll.h>
static void *no_alloc(void *o, uint32_t n) {
    (void)o;
    (void)n;
    return 0;
}
static void no_free(void *o, void *p, uint32_t n) {
    (void)o;
    (void)p;
    (void)n;
}
static void *no_map(void *o, uint32_t at, uint32_t n, int exact) {
    (void)o;
    (void)at;
    (void)n;
    (void)exact;
    return 0;
}
static int no_protect(void *o, void *p, uint32_t n, uint32_t prot) {
    (void)o;
    (void)p;
    (void)n;
    (void)prot;
    return 0;
}
static int no_open(void *o, const char *name, struct w32_file *f) {
    (void)o;
    (void)name;
    (void)f;
    return 0;
}
static uint32_t teb_thread(void *o) {
    (void)o;
    struct w32_teb *t = NtCurrentTeb();
    return (uint32_t)(uintptr_t)t->ClientId.UniqueThread;
}
static void no_wait(void *o, volatile uint32_t *p, uint32_t v) {
    (void)o;
    (void)p;
    (void)v;
}
static void no_wake(void *o, volatile uint32_t *p) {
    (void)o;
    (void)p;
}
static void no_diagnostic(void *o, const char *a, const char *b, const char *c) {
    (void)o;
    (void)a;
    (void)b;
    (void)c;
}
static W32_NORETURN void stop(void) {
    for (;;)
        __builtin_trap();
}
static void no_continue(void *o, struct w32_context *c) {
    (void)o;
    (void)c;
    stop();
}
static void no_terminate(void *o, uint32_t code) {
    (void)o;
    (void)code;
    stop();
}
const struct w32_services w32_ciuki_services_stub = {
    .opaque = 0,
    .alloc = no_alloc,
    .free = no_free,
    .map = no_map,
    .protect = no_protect,
    .unmap = no_free,
    .open = no_open,
    .close = 0, /* never reached: open always fails */
    .thread_id = teb_thread,
    .wait_word = no_wait,
    .wake_word = no_wake,
    .diagnostic = no_diagnostic,
    .continue_context = no_continue,
    .terminate = no_terminate,
    .invoke = 0, /* real i386 stdcall entry points */
};
