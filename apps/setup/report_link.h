/* SPDX-License-Identifier: MIT
 * Setup side of the headless hardware report (f9-07 amendment 1): spawn
 * /Programs/hwreport --rescue --text-fd=3 with a channel, receive the framed
 * text (apps/hwreport/textframe.h) under a deadline, reap the child on every
 * path. Operations are injected so the host tests drive it with fakes. */
#ifndef CIUKI_SETUP_REPORT_LINK_H
#define CIUKI_SETUP_REPORT_LINK_H
#include <stddef.h>
#include <stdint.h>

#define ST_LINK_PROGRAM "/Programs/hwreport"
#define ST_LINK_CHILD_FD 3
#define ST_LINK_DEADLINE_MS 15000u   /* spawn to the last frame */
#define ST_LINK_SLICE_MS 1000u       /* longest single wait for the channel */
#define ST_LINK_EXIT_MS 2000u        /* child exit after END */
#define ST_LINK_KILL_MS 3000u        /* reap after SIGKILL */
#define ST_LINK_REASON 24

struct st_link_io {
    void *ctx;
    int (*pair)(void *, int fds[2]);                        /* [0] parent, [1] child end; 0 or -errno */
    /* Child end mapped to descriptor child_fd; returns the pid, or -errno (-ENOENT: no binary). */
    int (*spawn)(void *, const char *path, char *const argv[], int child_end, int child_fd);
    int (*wait_readable)(void *, int fd, unsigned ms);      /* 1 readable or hung up, 0 timeout, -errno */
    int (*recv)(void *, int fd, uint8_t data[256], size_t *bytes);  /* 1 message, 0 peer closed, -errno */
    int (*reap)(void *, int pid, int *status);              /* WNOHANG: 1 reaped, 0 running, -errno */
    int (*kill)(void *, int pid);                           /* SIGKILL */
    void (*sleep_ms)(void *, unsigned ms);
    void (*close_fd)(void *, int fd);
    uint64_t (*now_ms)(void *);
};

/* 0 with the text in buffer (cap bytes, at most HR_LINK_MAX) and its length,
 * or a negative errno with reason set to one of: memory, pair, missing,
 * spawn, timeout, closed, frame, size, checksum, status, exit. The child is
 * reaped (or killed and reaped) on every path where it was started. */
int st_link_report(const struct st_link_io *io, char *buffer, size_t cap, size_t *length, char reason[ST_LINK_REASON]);
#endif
