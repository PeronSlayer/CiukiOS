/* SPDX-License-Identifier: MIT
 * System seam of the production kernel adapter (kernel_adapter.c): the exact
 * calls the sealed ACCOUNT process makes. The target binds them to the SDK
 * (kernel_sys_ciuki.c); the host integration harness binds them to the kernel
 * host harness (tests/host/security/adapter_test.c). Nothing here interprets
 * records: every record is the frozen call-101/channel ABI.
 */
#ifndef ACCT_KERNEL_SYS_H
#define ACCT_KERNEL_SYS_H
#include <stddef.h>
#include <stdint.h>
struct ciuki_security_args;
struct ciuki_message;
/* Call 101. input/output are the operation's disjoint records (NULL when the
 * operation has none); the seam supplies their addresses. 0 or -errno; on
 * success args carries the kernel's result/state fields. */
int acct_sys_security(struct ciuki_security_args *args, const void *input, uint32_t input_bytes,
                      void *output, uint32_t output_bytes);
int acct_sys_channel_send(int fd, const struct ciuki_message *message); /* 0 or -errno */
/* 1 message copied, 0 end of stream, -EAGAIN when nothing is queued and
 * wait is zero, other -errno (ESRCH: revoked link). */
int acct_sys_channel_recv(int fd, struct ciuki_message *message, int wait);
int acct_sys_close(int fd);
/* Waits until one of the endpoints is readable or hung up: number ready,
 * 0 on timeout, -errno. count may be zero (plain sleep). */
int acct_sys_wait_readable(const int *fds, unsigned count, unsigned timeout_ms);
uint32_t acct_sys_pid(void);
/* Live process ids in PID order: count written, or -errno. */
int acct_sys_pids(uint32_t *pids, unsigned capacity);
uint64_t acct_sys_monotonic_ms(void);
int acct_sys_entropy(void *buffer, size_t bytes); /* 0 or -errno, all bytes or none */
int acct_sys_boot_flags(uint32_t *flags);          /* this start's CIUKI_BOOT_* bits */
int acct_sys_file_present(const char *path);       /* 1 regular file, 0 absent, -errno */
void acct_sys_log(const char *line);               /* kernel debug log, one line */
#endif
