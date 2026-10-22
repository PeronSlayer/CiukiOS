/* SPDX-License-Identifier: MIT
 * Host transport only: two anonymous pipes implement the SDK's duplex
 * packet channel. Each 288-byte message fits in PIPE_BUF atomically.
 * Kept apart from the native ABI's POSIX constants. */
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
struct ciuki_message;
#define MESSAGE_BYTES 288
static int output[128], worker_fd, fail_completion;
void cp_channel_host_fail_completion(void) { fail_completion = 1; }
void cp_channel_host_cleanup(void)
{ for (unsigned i = 0; i < 128; i++) if (output[i]) { close(output[i]); output[i] = 0; } }
int ciuki_channel_pair(int fds[2])
{
    int a[2], b[2];
    if (pipe(a)) return -1;
    if (pipe(b)) { close(a[0]); close(a[1]); return -1; }
    if (a[0] >= 128 || b[0] >= 128) { close(a[0]); close(a[1]); close(b[0]); close(b[1]); errno = EMFILE; return -1; }
    fds[0] = a[0]; fds[1] = b[0]; worker_fd = fds[1]; output[fds[0]] = b[1]; output[fds[1]] = a[1]; return 0;
}
int ciuki_channel_send(int fd, const struct ciuki_message *m, uint32_t flags)
{
    (void)flags;
    if (fd == worker_fd && fail_completion) {
        fail_completion = 0; close(output[fd]); output[fd] = 0; errno = ENOMEM; return -1;
    }
    return write(output[fd], m, MESSAGE_BYTES) == MESSAGE_BYTES ? 0 : -1;
}
int ciuki_channel_recv(int fd, struct ciuki_message *m, uint32_t flags)
{
    int old = fcntl(fd, F_GETFL);
    if (flags) fcntl(fd, F_SETFL, old | O_NONBLOCK);
    int r = read(fd, m, MESSAGE_BYTES) == MESSAGE_BYTES ? 0 : -1;
    int saved = errno;
    if (flags) fcntl(fd, F_SETFL, old);
    errno = saved; return r;
}
