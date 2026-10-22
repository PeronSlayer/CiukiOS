/* Frozen native calls 78--89. User pointers are copied/pinned at this boundary
 * and never published to the socket/core workers.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/net_stack.h>
#include <ciuki/net_control.h>
#include <ciuki/files.h>
#include <ciuki/signal.h>
#include "lwip/sys.h"
struct user_operation {
    struct process *process;
    struct ua_pin pins[4];
    unsigned pinned;
    bool used;
    uint8_t *buffer;
    struct net_socket_ref socket;
    void *poll;
};
static struct user_operation operations[8];
static uint8_t io_buffers[8][CIUKI_SOCKET_IO_MAX];
static void user_free(void *arg)
{
    struct user_operation *o = arg;
    while (o->pinned) ua_unpin(o->process->memory, &o->pins[--o->pinned]);
    net_socket_drop(&o->socket);
    if (o->poll) { net_poll_drop(o->poll); o->poll = NULL; }
    sys_prot_t f = sys_arch_protect(); o->used = false; sys_arch_unprotect(f);
}
static struct user_operation *user_new(struct process *p)
{
    struct user_operation *o = NULL; sys_prot_t f = sys_arch_protect();
    for (unsigned i = 0; i < 8; i++) if (!operations[i].used) { o = &operations[i]; memset(o, 0, sizeof(*o)); o->used = true; o->process = p; o->buffer = io_buffers[i]; break; }
    sys_arch_unprotect(f); return o;
}
static int pin(struct user_operation *o, uint32_t address, unsigned bytes)
{
    if (!bytes) return 0;
    if (!address || bytes > UINT32_MAX - address || o->pinned == 4) return -EFAULT;
    int e = ua_pin(o->process->memory, &o->pins[o->pinned], address, bytes, true);
    if (!e) o->pinned++;
    return e;
}
static int address_input(uint32_t pointer, uint32_t length, struct ciuki_sockaddr_in *a)
{
    if (length != sizeof(*a)) return -EINVAL;
    if (!pointer) return -EFAULT;
    return copy_from_user(a, pointer, sizeof(*a));
}
static int address_output(struct user_operation *o, uint32_t address, uint32_t length, unsigned *capacity)
{
    if (!address && !length) { *capacity = 0; return 0; }
    if (!address || !length) return -EFAULT;
    int e = copy_from_user(capacity, length, 4);
    if (!e && *capacity > 65535) e = -EINVAL;
    if (!e) e = pin(o, length, 4);
    if (!e) e = pin(o, address, *capacity < 16 ? *capacity : 16);
    return e;
}
static int address_copy(uint32_t address, uint32_t length, unsigned cap, const struct ciuki_sockaddr_in *a)
{
    if (!address) return 0;
    int e = copy_to_user(address, a, cap < 16 ? cap : 16); unsigned actual = 16;
    return e ? e : copy_to_user(length, &actual, 4);
}
static int io_call(struct user_operation *o, const struct ciuki_socket_io *io, bool send)
{
    if (io->size != sizeof(*io) || io->version != 1 || io->reserved[0] || io->reserved[1] ||
        io->bytes > CIUKI_SOCKET_IO_MAX || io->flags & ~(MSG_DONTWAIT | (send ? MSG_NOSIGNAL : MSG_PEEK)) ||
        (!io->address && io->address_length)) return -EINVAL;
    int e = net_socket_capture(o->process, io->fd, &o->socket); if (e) return e;
    struct ciuki_sockaddr_in address = {0}; unsigned cap = 0;
    if (send) {
        if (io->address) e = address_input(io->address, io->address_length, &address);
        if (!e && io->bytes) e = copy_from_user(o->buffer, io->buffer, io->bytes);
    } else {
        e = address_output(o, io->address, io->address_length, &cap);
        if (!e) e = pin(o, io->buffer, io->bytes);
    }
    if (e) return e;
    e = net_socket_io(o->process, io->fd, o->buffer, io->bytes, io->flags, io->address ? &address : NULL, send, &o->socket);
    if (!send && e >= 0) {
        int copy = e ? copy_to_user(io->buffer, o->buffer, (unsigned)e) : 0;
        if (!copy) copy = address_copy(io->address, io->address_length, cap, &address);
        if (copy) return copy;
    }
    return e;
}
int32_t net_socket_rw(struct process *p, int fd, uint32_t buffer, uint32_t bytes, bool write)
{
    if (bytes > CIUKI_SOCKET_IO_MAX) return -EINVAL;
    struct user_operation *o = user_new(p); if (!o) return -ENOMEM;
    struct proc_thread *t = proc_thread_for(g_current);
    if (t) { t->cleanup = user_free; t->operation = o; }
    struct ciuki_socket_io io = { .size = sizeof(io), .version = 1, .fd = fd, .buffer = buffer, .bytes = bytes };
    int e = io_call(o, &io, write);
    if (t) { t->cleanup = NULL; t->operation = NULL; }
    user_free(o); return e;
}
static int dispatch(struct user_operation *o, struct trap_frame *tf)
{
    struct process *p = o->process; unsigned nr = tf->eax;
    if (tf->edi || tf->ebp) return -EINVAL;
    if (nr != CIUKI_SYS_SOCKNAME && tf->esi) return -EINVAL;
    if (nr == CIUKI_SYS_SOCKET) return net_socket_create(p, (int)tf->ebx, (int)tf->ecx, (int)tf->edx);
    if (nr == CIUKI_SYS_SENDTO || nr == CIUKI_SYS_RECVFROM) {
        if (tf->ecx || tf->edx) return -EINVAL;
        struct ciuki_socket_io io; int e = copy_from_user(&io, tf->ebx, sizeof(io));
        return e ? e : io_call(o, &io, nr == CIUKI_SYS_SENDTO);
    }
    if (nr == CIUKI_SYS_POLL) {
        if (tf->ecx > CIUKI_OPEN_MAX || (int32_t)tf->edx < -1) return -EINVAL;
        unsigned bytes = tf->ecx * sizeof(struct ciuki_pollfd);
        int e = bytes ? copy_from_user(o->buffer, tf->ebx, bytes) : 0;
        if (!e) e = net_poll_capture(p, (void *)o->buffer, tf->ecx, &o->poll);
        if (!e) e = pin(o, tf->ebx, bytes);
        if (e) return e;
        e = net_poll(p, (void *)o->buffer, tf->ecx, (int32_t)tf->edx, o->poll);
        if (e >= 0 && bytes) { int c = copy_to_user(tf->ebx, o->buffer, bytes); if (c) return c; }
        return e;
    }
    if (nr == CIUKI_SYS_RESOLVE4) {
        if (tf->ecx || tf->edx) return -EINVAL;
        struct ciuki_resolve4 r; int e = copy_from_user(&r, tf->ebx, sizeof(r));
        if (e) return e;
        if (r.size != sizeof(r) || r.version != 1 || r.reserved[0] || r.reserved[1] || r.name_bytes < 2 || r.name_bytes > 254) return -EINVAL;
        char name[254]; e = copy_from_user(name, r.name, r.name_bytes);
        if (!e && (name[r.name_bytes - 1] || strlen(name) + 1 != r.name_bytes)) e = -EINVAL;
        if (!e) e = pin(o, tf->ebx, sizeof(r));
        if (!e) e = net_resolve(p, name, &r.address, &r.status);
        return e ? e : copy_to_user(tf->ebx, &r, sizeof(r));
    }
    if (nr == CIUKI_SYS_SOCKOPT) {
        if (tf->ecx || tf->edx) return -EINVAL;
        struct ciuki_socket_option r; int e = copy_from_user(&r, tf->ebx, sizeof(r));
        if (e) return e;
        if (r.size != sizeof(r) || r.version != 1 || r.operation > 1 || r.reserved[0] || r.reserved[1]) return -EINVAL;
        e = net_socket_capture(p, r.fd, &o->socket); if (e) return e;
        unsigned length = r.length; uint8_t value[16] = {0};
        if (r.operation) {
            if (length > sizeof(value)) return -EINVAL;
            e = copy_from_user(value, r.value, length);
        } else {
            e = copy_from_user(&length, r.length, 4);
            if (!e && length > 65535) e = -EINVAL;
            if (!e) e = pin(o, r.length, 4);
            unsigned actual = r.level == SOL_SOCKET && (r.name == SO_RCVTIMEO || r.name == SO_SNDTIMEO) ? 16 : 4;
            if (!e) e = pin(o, r.value, length < actual ? length : actual);
        }
        unsigned capacity = length;
        if (!e) e = net_socket_option(p, r.fd, r.operation, r.level, r.name, value, &length, &o->socket);
        if (!e && !r.operation) {
            e = copy_to_user(r.value, value, capacity < length ? capacity : length);
            if (!e) e = copy_to_user(r.length, &length, 4);
        }
        return e;
    }
    if ((nr == CIUKI_SYS_LISTEN || nr == CIUKI_SYS_SHUTDOWN) && tf->edx) return -EINVAL;
    if (nr == CIUKI_SYS_SOCKNAME && tf->ecx > 1) return -EINVAL;
    int e = net_socket_capture(p, (int)tf->ebx, &o->socket); if (e) return e;
    struct ciuki_sockaddr_in a; unsigned cap;
    switch (nr) {
    case CIUKI_SYS_BIND: case CIUKI_SYS_CONNECT:
        e = address_input(tf->ecx, tf->edx, &a);
        return e ? e : nr == CIUKI_SYS_BIND ? net_socket_bind(p, (int)tf->ebx, &a, &o->socket) : net_socket_connect(p, (int)tf->ebx, &a, &o->socket);
    case CIUKI_SYS_LISTEN: return net_socket_listen(p, (int)tf->ebx, (int)tf->ecx, &o->socket);
    case CIUKI_SYS_SHUTDOWN: return net_socket_shutdown(p, (int)tf->ebx, tf->ecx, &o->socket);
    case CIUKI_SYS_ACCEPT:
        e = address_output(o, tf->ecx, tf->edx, &cap); if (e) return e;
        e = net_socket_accept(p, (int)tf->ebx, &a, &o->socket);
        if (e >= 0) { int copy = address_copy(tf->ecx, tf->edx, cap, &a); if (copy) { file_close(p, e); return copy; } }
        return e;
    case CIUKI_SYS_SOCKNAME:
        e = address_output(o, tf->edx, tf->esi, &cap); if (e) return e;
        if (!tf->edx || !tf->esi) return -EFAULT;
        e = net_socket_name(p, (int)tf->ebx, tf->ecx, &a, &o->socket);
        return e ? e : address_copy(tf->edx, tf->esi, cap, &a);
    default: return -ENOSYS;
    }
}
int32_t net_socket_call(struct trap_frame *tf)
{
    if (tf->eax == CIUKI_SYS_NETCTL) {
        struct process *p = proc_current();
        if (!p || !p->network_capability) return -EPERM;
        if (tf->esi || tf->edi || tf->ebp || tf->edx != sizeof(struct ciuki_net_control)) return -EINVAL;
        struct ciuki_net_control record;
        int e = copy_from_user(&record, tf->ecx, sizeof(record));
        if (e) return e;
        if (record.version != CIUKI_NETCTL_VERSION) return -EOPNOTSUPP;
        if (record.size != sizeof(record) || record.reserved0[0] || record.reserved0[1] ||
            record.reserved[0] || record.reserved[1] || record.reserved[2]) return -EINVAL;
        /* Validate writable output before any configuration side effect. */
        struct user_operation output = {.process = p};
        struct proc_thread *t = proc_thread_for(g_current);
        if (t) { t->cleanup = user_free; t->operation = &output; }
        e = pin(&output, tf->ecx, sizeof(record));
        struct ciuki_net_config config = record.config; unsigned index = record.index, generation = record.generation;
        memset(&record, 0, sizeof(record)); record.version = CIUKI_NETCTL_VERSION;
        record.size = sizeof(record); record.index = index; record.config = config; record.generation = generation;
        if (!e) e = net_control(tf->ebx, &record);
        if (!e) e = copy_to_user(tf->ecx, &record, sizeof(record));
        if (t) { t->cleanup = NULL; t->operation = NULL; }
        user_free(&output); return e;
    }
    struct process *p = proc_current(); if (!p) return -ENOSYS;
    struct user_operation local = { .process = p };
    bool data = tf->eax == CIUKI_SYS_SENDTO || tf->eax == CIUKI_SYS_RECVFROM || tf->eax == CIUKI_SYS_POLL;
    struct user_operation *o = data ? user_new(p) : &local;
    if (!o) return -ENOMEM;
    struct proc_thread *t = proc_thread_for(g_current);
    if (t) { t->cleanup = user_free; t->operation = o; }
    int result = dispatch(o, tf);
    if (t) { t->cleanup = NULL; t->operation = NULL; }
    user_free(o); return result;
}
