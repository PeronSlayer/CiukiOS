/* The only wire definitions are ciuki/abi.h. Validate outputs before commit.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/desktop.h>

int32_t desktop_syscall(struct trap_frame *tf)
{
    struct process *p = proc_current();
    if (!p) return -ENOSYS;
    switch (tf->eax) {
    case CIUKI_SYS_SURFACE_CREATE: return surface_create(p, tf->ebx, tf->ecx, tf->edx);
    case CIUKI_SYS_SURFACE_MAP: return surface_map(p, (int32_t)tf->ebx, tf->ecx);
    case CIUKI_SYS_SURFACE_INFO: {
        struct ciuki_surface_info out;
        int err = surface_info(p, (int32_t)tf->ebx, &out);
        return err ? err : copy_to_user(tf->ecx, &out, sizeof(out));
    }
    case CIUKI_SYS_DISPLAY_INFO: {
        struct ciuki_display_info out;
        int err = grant_display_info(p, (int32_t)tf->ebx, &out);
        return err ? err : copy_to_user(tf->ecx, &out, sizeof(out));
    }
    case CIUKI_SYS_PRESENT: {
        struct ciuki_rect rect;
        /* Unknown/stale descriptor numbers win over pointer/authority errors. */
        struct desktop_description *display = desktop_fd(p, (int32_t)tf->ebx);
        struct desktop_description *surface = desktop_fd(p, (int32_t)tf->ecx);
        if (!display || display->kind != DESKTOP_DISPLAY || !surface || surface->kind != DESKTOP_SURFACE)
            return -EBADF;
        int err = copy_from_user(&rect, tf->edx, sizeof(rect));
        return err ? err : grant_present(p, (int32_t)tf->ebx, (int32_t)tf->ecx, &rect);
    }
    case CIUKI_SYS_INPUT_READ: return grant_input_read(p, (int32_t)tf->ebx, tf->ecx, tf->edx);
    case CIUKI_SYS_CHANNEL_PAIR: {
        struct ua_pin pin;
        int err = ua_pin(p->memory, &pin, tf->ebx, sizeof(int32_t) * 2, true);
        if (err) return err;
        int32_t fds[2];
        err = channel_pair(p, fds);
        if (!err) {
            err = copy_to_user(tf->ebx, fds, sizeof(fds));
            if (err) { desktop_close(p, fds[0]); desktop_close(p, fds[1]); }
        }
        ua_unpin(p->memory, &pin);
        return err;
    }
    case CIUKI_SYS_CHANNEL_SEND: return channel_send(p, (int32_t)tf->ebx, tf->ecx, tf->edx);
    case CIUKI_SYS_CHANNEL_RECV: return channel_recv(p, (int32_t)tf->ebx, tf->ecx, tf->edx);
    default: return -ENOSYS;
    }
}

bool desktop_fd_syscall(struct trap_frame *tf)
{
    struct process *p = proc_current();
    if (!p) return false;
    switch (tf->eax) {
    case CIUKI_SYS_CLOSE: case CIUKI_SYS_DUP: case CIUKI_SYS_DUP2: case CIUKI_SYS_FCNTL:
    case CIUKI_SYS_FSTAT: case CIUKI_SYS_READ: case CIUKI_SYS_WRITE:
    case CIUKI_SYS_LSEEK64: case CIUKI_SYS_PREAD: case CIUKI_SYS_PWRITE: break;
    default: return false;
    }
    int32_t fd = (int32_t)tf->ebx;
    if (!p->fds || fd < 0 || fd >= CIUKI_OPEN_MAX || !p->fds[fd].object) {
        tf->eax = (uint32_t)-EBADF;
        return true;
    }
    if (!desktop_fd(p, fd)) return false; /* f2-03 owns ordinary files/devices */
    int32_t result;
    switch (tf->eax) {
    case CIUKI_SYS_CLOSE: result = desktop_close(p, (int32_t)tf->ebx); break;
    case CIUKI_SYS_DUP: result = desktop_dup(p, (int32_t)tf->ebx, 0, false, 0); break;
    case CIUKI_SYS_DUP2: result = desktop_dup(p, (int32_t)tf->ebx, (int32_t)tf->ecx, true, 0); break;
    case CIUKI_SYS_FCNTL: result = desktop_fcntl(p, (int32_t)tf->ebx, tf->ecx, tf->edx); break;
    case CIUKI_SYS_FSTAT: result = -EOPNOTSUPP; break;
    case CIUKI_SYS_READ: case CIUKI_SYS_WRITE: result = -EBADF; break;
    case CIUKI_SYS_LSEEK64: case CIUKI_SYS_PREAD: case CIUKI_SYS_PWRITE: result = -ESPIPE; break;
    default: return false;
    }
    tf->eax = (uint32_t)result;
    return true;
}
