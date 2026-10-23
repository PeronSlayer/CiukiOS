/* SPDX-License-Identifier: MIT
 * See report_link.h. The status of the child and of the transfer are both
 * required to be clean: any other outcome is a failure and Setup shows its own
 * report instead. */
#include "report_link.h"
#include "../hwreport/textframe.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void fail(char reason[ST_LINK_REASON], const char *why) { snprintf(reason, ST_LINK_REASON, "%s", why); }

/* Kill and reap; bounded, never blocks beyond ST_LINK_KILL_MS. */
static void destroy(const struct st_link_io *io, int pid)
{
    int status = 0;
    if (io->reap(io->ctx, pid, &status) != 0) return;   /* already gone, or reaped */
    io->kill(io->ctx, pid);
    for (unsigned waited = 0; waited < ST_LINK_KILL_MS; waited += 10) {
        if (io->reap(io->ctx, pid, &status) != 0) return;
        io->sleep_ms(io->ctx, 10);
    }
}

/* A normal child exits right after END. */
static int wait_exit(const struct st_link_io *io, int pid)
{
    int status = 0;
    for (unsigned waited = 0; waited <= ST_LINK_EXIT_MS; waited += 10) {
        int r = io->reap(io->ctx, pid, &status);
        if (r < 0) return -EIO;
        if (r > 0) return (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0 ? 0 : -ECHILD;
        io->sleep_ms(io->ctx, 10);
    }
    destroy(io, pid);
    return -ETIMEDOUT;
}

int st_link_report(const struct st_link_io *io, char *buffer, size_t cap, size_t *length, char reason[ST_LINK_REASON])
{
    *length = 0; reason[0] = 0;
    if (!io || !io->pair || !io->spawn || !io->wait_readable || !io->recv || !io->reap || !io->kill ||
        !io->sleep_ms || !io->close_fd || !io->now_ms || !buffer || !cap) { fail(reason, "memory"); return -EINVAL; }
    int fds[2];
    if (io->pair(io->ctx, fds)) { fail(reason, "pair"); return -EIO; }
    char *argv[] = {"hwreport", "--rescue", "--text-fd=3", NULL};
    int pid = io->spawn(io->ctx, ST_LINK_PROGRAM, argv, fds[1], ST_LINK_CHILD_FD);
    io->close_fd(io->ctx, fds[1]);                     /* the child holds its own copy */
    if (pid <= 0) {
        io->close_fd(io->ctx, fds[0]);
        fail(reason, pid == -ENOENT ? "missing" : "spawn");
        return pid < 0 ? pid : -EIO;
    }
    uint64_t start = io->now_ms(io->ctx);
    struct hr_frame_rx rx; hr_frame_rx_init(&rx, buffer, cap);
    int result = 0, done = 0;
    while (!done) {
        uint64_t elapsed = io->now_ms(io->ctx) - start;
        if (elapsed >= ST_LINK_DEADLINE_MS) { result = -ETIMEDOUT; fail(reason, "timeout"); break; }
        unsigned slice = ST_LINK_DEADLINE_MS - (unsigned)elapsed;
        if (slice > ST_LINK_SLICE_MS) slice = ST_LINK_SLICE_MS;
        int ready = io->wait_readable(io->ctx, fds[0], slice);
        if (ready < 0 && ready != -EINTR) { result = ready; fail(reason, "closed"); break; }
        if (ready <= 0) continue;                        /* timeout slice: recheck the deadline */
        uint8_t data[HR_FRAME_MAX]; size_t bytes = 0;
        int r = io->recv(io->ctx, fds[0], data, &bytes);
        if (r == -EAGAIN || r == -EINTR) continue;
        if (r < 0) { result = r; fail(reason, "closed"); break; }
        if (r == 0) { result = -EPIPE; fail(reason, "closed"); break; }   /* peer gone before END */
        int f = hr_frame_rx_feed(&rx, data, bytes);
        if (f < 0) { result = f; fail(reason, f == -EMSGSIZE ? "size" : f == -HR_ECHECKSUM ? "checksum" : "frame"); break; }
        done = f;
    }
    io->close_fd(io->ctx, fds[0]);
    if (result) { destroy(io, pid); return result; }
    int exit_result = wait_exit(io, pid);
    if (exit_result == -ETIMEDOUT) { fail(reason, "timeout"); return exit_result; }
    if (exit_result) { fail(reason, "exit"); return exit_result; }
    if (rx.status) { fail(reason, "status"); return -EIO; }
    *length = rx.received;
    return 0;
}
