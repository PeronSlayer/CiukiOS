/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#ifndef CIUKI_W32_SERVICES_H
#define CIUKI_W32_SERVICES_H
#include "base.h"
struct w32_context;
struct w32_exception;
struct w32_file {
    const uint8_t *bytes;
    uint32_t size;
    void *cookie;
};
enum { W32_PROT_READ = 1, W32_PROT_WRITE = 2, W32_PROT_EXEC = 4 };
/* Private core boundary. The CiukiOS implementation transports versioned
 * CIUKI_SYS_W32 records; the Linux adapter remains a separate host oracle.
 * map() returns zeroed private pages. image() returns a populated section
 * view without copying over a live image. Binding requests write access only
 * to pages containing relocation, IAT or TLS-index targets.
 * alloc/free manage metadata, not application HeapAlloc blocks.
 * wait/wake permit spurious wakes. open owns an immutable raw snapshot. */
struct w32_services {
    void *opaque;
    void *(*alloc)(void *, uint32_t);
    void (*free)(void *, void *, uint32_t);
    void *(*map)(void *, uint32_t, uint32_t, int);
    int (*protect)(void *, void *, uint32_t, uint32_t);
    void (*unmap)(void *, void *, uint32_t);
    int (*open)(void *, const char *, struct w32_file *);
    void (*close)(void *, struct w32_file *);
    uint32_t (*thread_id)(void *);
    void (*wait_word)(void *, volatile uint32_t *, uint32_t);
    void (*wake_word)(void *, volatile uint32_t *);
    void (*diagnostic)(void *, const char *, const char *, const char *);
    void (*continue_context)(void *, struct w32_context *);
    void (*terminate)(void *, uint32_t);
    /* NULL executes actual i386 stdcall callbacks. Tests may install an
     * observer for structural-only synthetic fixture entry points. */
    int (*invoke)(void *, void *, void *, uint32_t, int);
    void *(*image)(void *, const struct w32_file *, uint32_t, int, int);
    int (*raise)(void *, struct w32_exception *, struct w32_context *);

};
extern const struct w32_services w32_ciuki_services;
extern const struct w32_services w32_ciuki_services_stub; /* part A compatibility */
int32_t w32_ciuki_call(void *, uint32_t);

void w32_set_services(const struct w32_services *);
const struct w32_services *w32_get_services(void);
void w32_diag(const char *, const char *, const char *);
W32_NORETURN void w32_terminate(uint32_t);
#endif
