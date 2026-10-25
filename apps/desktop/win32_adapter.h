/* Desktop-owned Win32 adapters. SPDX-License-Identifier: MIT */
#ifndef DESK_WIN32_ADAPTER_H
#define DESK_WIN32_ADAPTER_H
#include "shell_link.h"
#include <stddef.h>
#include <w32/server.h>
/* Transport returns Win32 error; complete copied response only, no retries of
 * mutations after ambiguous disconnect. Handles never survive one operation. */
uint32_t desk_w32_exchange(const void *, uint32_t, void *, uint32_t, uint32_t *);
int desk_w32_dos_command(const struct desk_launch_record *,char *,size_t);
int ciuki_win32_uninstall_list(struct desk_uninstall_entry *, unsigned);
int ciuki_win32_uninstall_registered(const char *); /* exact key: 1 present, 0 absent, -errno */
int ciuki_win32_uninstall_run(const char *);
int ciuki_win32_shell_launch(const struct desk_launch_record *);
int ciuki_win32_shell_launch_wait(const struct desk_launch_record *);
int desk_w32_flush(void);
int desk_w32_ready(void); /* successful read-only registry round trip */
/* Native binding: PE/DOS prepare off the input thread; ELF hands off to
 * the existing desktop launcher and preserves its launch-owned authority. */
enum desk_w32_kind { DESK_W32_PE, DESK_W32_ELF, DESK_W32_DOS };
int desk_w32_launch(enum desk_w32_kind, const struct desk_launch_record *);
int desk_w32_launch_wait(enum desk_w32_kind, const struct desk_launch_record *);
int desk_w32_identify(const char *native_path); /* kind or negative errno */
int desk_w32_native_path(const char *, char *, size_t);
int desk_w32_arguments(char *, char **, unsigned);
/* Supervision is advanced by the desktop service loop, never by clients. */
struct desk_w32_supervisor {
    uint32_t pid, starts;
    uint64_t retry_ns, death_ns;
    int stopping;
    uint64_t start_ns;      /* when the live server was spawned */
    unsigned rapid_exits;   /* consecutive deaths within DESK_W32_RAPID_NS of a start */
};
struct desk_w32_supervisor_ops {
    int (*spawn)(void *); /* must use exact /System/Win32/w32srv */
    void (*record)(void *, const char *, int, uint64_t);
    void *ctx;
};
void desk_w32_supervise(struct desk_w32_supervisor *, const struct desk_w32_supervisor_ops *, uint64_t);
struct ciuki_proc_system; struct ciuki_proc_record;
/* 1 on a cold boot (the directory's process identities are from an old boot,
 * whose PIDs this boot reuses), 0 when this desktop replaces one that died
 * in this boot (surviving Win32 programs keep their atoms and HWNDs). */
int desk_w32_fresh_clients(const struct ciuki_proc_system *, const struct ciuki_proc_record *, uint32_t self_pid);
int desk_w32_service_exited(struct desk_w32_supervisor *, const struct desk_w32_supervisor_ops *, uint32_t, int, uint64_t);
struct desktop;
int desk_w32_native_shortcut(struct desktop *, const struct desk_launch_record *);
void desk_w32_start(struct desktop *);
void desk_w32_presenter_poll(struct desktop *);
void desk_w32_poll(struct desktop *);
void desk_w32_child_exited(struct desktop *, uint32_t, int);
int desk_w32_shutdown(void); /* flush barrier: 1 ready, 0 pending, negative errno */
#endif
