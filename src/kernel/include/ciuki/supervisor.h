/* PID 1 bootstrap/controller services. No user text enters record grammar.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SUPERVISOR_H
#define CIUKI_SUPERVISOR_H
#include <ciuki/desktop.h>
#include <ciuki/sha256.h>
#define SUPERVISOR_CAPTURE_BYTES 1024u
struct supervisor_capture {
    struct sha256_ctx digest;
    uint64_t bytes;
    uint32_t head_bytes, tail_bytes, tail_at;
    uint32_t final_ok, assertion_failures, scan_bytes;
    uint8_t scan[32];
    uint8_t head[SUPERVISOR_CAPTURE_BYTES], tail[SUPERVISOR_CAPTURE_BYTES];
};
void supervisor_capture_init(struct supervisor_capture *c);
void supervisor_capture_add(struct supervisor_capture *c, const void *bytes, uint32_t length);
void supervisor_capture_digest(const struct supervisor_capture *c, char out[65]);
void supervisor_capture_tail(const struct supervisor_capture *c, uint8_t *out);
/* Called after drivers and executable VFS/console services are ready. */
void supervisor_bootstrap(void);
void supervisor_poll(void);
uint32_t supervisor_desktop_deaths(void);
/* f2-03 registers console/null installation and a retained cwd resolver.
 * Preparation may block; supervisor publishes only after all setup succeeds. */
struct supervisor_io_ops {
    int (*stdio)(struct process *child); /* fd0 null, fd1/fd2 console */
    int (*cwd)(struct process *child, const char *path);
};
void supervisor_set_io_ops(const struct supervisor_io_ops *ops);
int supervisor_spawn(const char *path, const char *const argv[], const char *cwd,
                      bool desktop, struct process **out);
int supervisor_spawn_gate(bool supplement, struct process **out);
int supervisor_spawn_standin(struct process *parent, struct process **out);
/* The selected controller owns the observer. Capture associates every byte
 * with the emitting PID, including writes through duplicated console fds. */
void supervisor_observe(const char *probe, uint32_t pid);
void supervisor_observe_end(void);
bool supervisor_report(struct task *task, const void *bytes, uint32_t length);
bool supervisor_output(struct task *task, unsigned stream, const void *bytes, uint32_t length);
const struct supervisor_capture *supervisor_captured(unsigned stream);
int probe_f2_crash_isolation(void);
int probe_f2_libc_smoke(void);
#endif
