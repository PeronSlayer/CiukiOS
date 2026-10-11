/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_PROC_H
#define CIUKI_SDK_PROC_H
#include <ciuki/abi.h>
/* f2-33: process enumeration (call 70) and termination requests (call 71).
 * Records and errors: docs/design/execution-abi.md, f2-33 amendment. */
#define __CIUKI_PROC_VERSION__ 1
/* Environment variable naming the delegated authority descriptor that PID 1
 * installs in the desktop; the desktop passes it to the Task Manager only. */
#define CIUKI_TASKS_ENV "CIUKI_TASKS_FD"
/* Fills one ciuki_proc_system record and then process records in PID order;
 * returns the number of process records written, or -1 with errno. */
int ciuki_proc_list(void *buffer, uint32_t bytes);
/* SIGTERM or SIGKILL; authority_fd < 0 requests without the delegated
 * authority (descendants only). Returns 0 or -1 with errno. */
int ciuki_proc_terminate(ciuki_pid_t pid, int signo, int authority_fd);
#endif
