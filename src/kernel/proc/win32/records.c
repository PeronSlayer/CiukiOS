/* Copied-record ABI and native/NTSTATUS/LastError boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/win32.h>
uint32_t w32_status(int error)
{
    switch (error) {
    case 0: return 0;
    case -EFAULT: return W32_STATUS_ACCESS_VIOLATION;
    case -ENOMEM: case -EAGAIN: return W32_STATUS_NO_MEMORY;
    case -EACCES: case -EPERM: return W32_STATUS_ACCESS_DENIED;
    case -EINVAL: return W32_STATUS_INVALID_PARAMETER;
    case -EEXIST: return W32_STATUS_CONFLICTING_ADDRESSES;
    case -ENOEXEC: return W32_STATUS_INVALID_IMAGE_FORMAT;
    case -ENOENT: return W32_STATUS_OBJECT_NAME_NOT_FOUND;
    case -EBUSY: return W32_STATUS_DEVICE_BUSY;
    case -ENOSYS: return W32_STATUS_NOT_IMPLEMENTED;
    default: return W32_STATUS_UNSUCCESSFUL;
    }
}
uint32_t w32_last_error(uint32_t status)
{
    switch (status) {
    case 0: return 0;
    case W32_STATUS_ACCESS_VIOLATION: return 998;
    case W32_STATUS_NO_MEMORY: return 8;
    case W32_STATUS_ACCESS_DENIED: return 5;
    case W32_STATUS_INVALID_PARAMETER: return 87;
    case W32_STATUS_CONFLICTING_ADDRESSES: return 487;
    case W32_STATUS_INVALID_IMAGE_FORMAT: return 193;
    case W32_STATUS_OBJECT_NAME_NOT_FOUND: return 2;
    case W32_STATUS_DEVICE_BUSY: return 170;
    case W32_STATUS_NOT_IMPLEMENTED: return 120;
    default: return 31;
    }
}
uint32_t w32_record_size(uint32_t operation)
{
    switch (operation) {
    case W32_IDENTITY: return sizeof(struct ciuki_w32_identity);
    case W32_COUNTER: return sizeof(struct ciuki_w32_counter);
    case W32_VM_ALLOC: case W32_VM_FREE: case W32_VM_PROTECT: return sizeof(struct ciuki_w32_vm);
    case W32_VM_QUERY: return sizeof(struct ciuki_w32_query);
    case W32_DISPATCHER: return sizeof(struct ciuki_w32_dispatcher);
    case W32_CONTINUE: return sizeof(struct ciuki_w32_continue);
    case W32_RAISE: return sizeof(struct ciuki_w32_raise);
    case W32_THREAD: return sizeof(struct ciuki_w32_thread);
    case W32_DIAGNOSTIC: return sizeof(struct ciuki_w32_diagnostic);
    default: return 0;
    }
}
static bool zero_bytes(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    while (n--) if (*b++) return false;
    return true;
}
int w32_record_validate(const void *record, uint32_t bytes, bool win32)
{
    if (!win32) return -EPERM;
    if (bytes < sizeof(struct ciuki_w32_header)) return -EINVAL;
    const struct ciuki_w32_header *h = record;
    if (h->version != 1 || h->size != bytes || h->reserved) return -EINVAL;
    uint32_t size = w32_record_size(h->operation);
    if (!size) return -ENOSYS;
    if (size != bytes) return -EINVAL;
    switch (h->operation) {
    case W32_IDENTITY: case W32_COUNTER:
        return zero_bytes((const uint8_t *)record + sizeof(*h), bytes - sizeof(*h)) ? 0 : -EINVAL;
    case W32_VM_ALLOC: case W32_VM_FREE: case W32_VM_PROTECT: {
        const struct ciuki_w32_vm *a = record;
        if (a->reserved || a->old_protect || (h->operation == W32_VM_FREE && a->protect)) return -EINVAL;
        uint32_t native;
        if (h->operation != W32_VM_FREE && w32_protection(a->protect, &native)) return -EINVAL;
        if (h->operation == W32_VM_ALLOC && (a->flags & ~(W32_MEM_RESERVE | W32_MEM_COMMIT | W32_MEM_TOP_DOWN) ||
            !(a->flags & (W32_MEM_RESERVE | W32_MEM_COMMIT)))) return -EINVAL;
        if (h->operation == W32_VM_FREE && a->flags != W32_MEM_RELEASE && a->flags != W32_MEM_DECOMMIT) return -EINVAL;
        return h->operation == W32_VM_PROTECT && a->flags ? -EINVAL : 0;
    }
    case W32_VM_QUERY: return zero_bytes(&((const struct ciuki_w32_query *)record)->info, sizeof(struct ciuki_w32_memory_info)) ? 0 : -EINVAL;
    case W32_DISPATCHER: return ((const struct ciuki_w32_dispatcher *)record)->reserved ? -EINVAL : 0;
    case W32_CONTINUE: return ((const struct ciuki_w32_continue *)record)->reserved ? -EINVAL : 0;
    case W32_THREAD: {
        const struct ciuki_w32_thread *a = record;
        return a->tid || a->flags & ~W32_THREAD_RESERVATION ? -EINVAL : 0;
    }
    case W32_DIAGNOSTIC: {
        const struct ciuki_w32_diagnostic *a = record;
        if (a->kind < 1 || a->kind > 3 || a->pid || a->tid) return -EINVAL;
        bool terminated = false;
        for (unsigned i = 0; i < sizeof(a->payload); i++) {
            uint8_t c = a->payload[i];
            if (!c) terminated = true;
            else if (terminated || c < 32 || c > 126) return -EINVAL;
        }
        return terminated ? 0 : -EINVAL;
    }
    default: return 0;
    }
}
void w32_identity(struct ciuki_w32_identity *a)
{
    memset((uint8_t *)a + 16, 0, sizeof(*a) - 16);
    a->major = 4; a->minor = 10; a->build = 2222; a->platform = 1;
    a->get_version = 0x80000000 | (2222u << 16) | (10u << 8) | 4;
    a->directx = 0x0007000a; /* installation identity, no D3D capability claim */
    a->cpu_features = (g_cpu_fxsr ? 1u : 0u) | (g_cpu_sse ? 2u : 0u);
    memcpy(a->csd, " A ", 4);
}
