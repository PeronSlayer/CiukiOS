/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Linux host evidence only. */
#ifndef W32_HOST_LINUX_H
#define W32_HOST_LINUX_H
#include <w32/ldr.h>
struct host_state {
    struct w32_services services;
    struct w32_teb teb;
    struct w32_peb peb;
    struct w32_process_parameters parameters;
    ULONG next_allocation, allocations, mappings, closes, invocations;
    LONG fail_after;
    char last_event[128], last_module[1024], last_symbol[256];
    int observer, fail_attach;
    void *dispatcher; /* KiUserExceptionDispatcher of the core in use */
};
extern struct host_state host;
LONG host_syscall(ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG);
void host_init(void);
void host_fs(struct w32_teb *);
void host_signals(void);
void host_print(const char *);
void host_number(ULONG);
void host_exit(ULONG) W32_NORETURN;
int host_entry(void *, void *);
void host_resume(struct w32_context *) W32_NORETURN;
void host_start(ULONG *) W32_NORETURN;
int host_main(int, char **);
#endif
