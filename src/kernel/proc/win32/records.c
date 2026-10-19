/* Copied-record ABI and native/NTSTATUS/LastError boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/w32_objects.h>
#include <ciuki/w32_core.h>
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
    case W32_COMPAT: return sizeof(struct ciuki_w32_compat_args);
    case W32_REPORT: return sizeof(struct ciuki_w32_report_args);
    case W32_IDENTITY: return sizeof(struct ciuki_w32_identity);
    case W32_COUNTER: return sizeof(struct ciuki_w32_counter);
    case W32_VM_ALLOC: case W32_VM_FREE: case W32_VM_PROTECT: return sizeof(struct ciuki_w32_vm);
    case W32_VM_QUERY: return sizeof(struct ciuki_w32_query);
    case W32_DISPATCHER: return sizeof(struct ciuki_w32_dispatcher);
    case W32_CONTINUE: return sizeof(struct ciuki_w32_continue);
    case W32_RAISE: return sizeof(struct ciuki_w32_raise);
    case W32_THREAD: return sizeof(struct ciuki_w32_thread);
    case W32_DIAGNOSTIC: return sizeof(struct ciuki_w32_diagnostic);
    case W32_SNAPSHOT: return sizeof(struct ciuki_w32_snapshot_args);
    case W32_PROCESS: return sizeof(struct ciuki_w32_spawn_args);
    case W32_FILE: return sizeof(struct ciuki_w32_file_args);
    case W32_SYSTEM: return sizeof(struct ciuki_w32_system_args);
    case W32_LOADER: return sizeof(struct ciuki_w32_loader_args);
    case W32_WORD: return sizeof(struct ciuki_w32_word_args);
    case W32_OBJECT: return sizeof(struct ciuki_w32_object_args);
    case W32_WAIT: return sizeof(struct ciuki_w32_wait_args);
    case W32_APC: return sizeof(struct ciuki_w32_apc_args);
    case W32_CONTEXT: return sizeof(struct ciuki_w32_context_args);
    case W32_MESSAGE: return sizeof(struct ciuki_w32_message_args);
    case W32_SECTION: return sizeof(struct ciuki_w32_section_args);
    case W32_VIEW: return sizeof(struct ciuki_w32_view_args);
    case W32_SERVICE: return sizeof(struct ciuki_w32_service_args);
    case W32_DISPLAY: return sizeof(struct ciuki_w32_display_args);
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

    if (bytes < sizeof(struct ciuki_w32_header)) return -EINVAL;
    const struct ciuki_w32_header *h = record;
    if (h->version != 1 || h->size != bytes || h->reserved) return -EINVAL;
    if(!win32 && h->operation!=W32_SERVICE && h->operation!=W32_DISPLAY && h->operation!=W32_REPORT)return -EPERM;
    uint32_t size = w32_record_size(h->operation);
    if (!size) return -ENOSYS;
    if (size != bytes) return -EINVAL;
    switch (h->operation) {
    case W32_COMPAT:{const struct ciuki_w32_compat_args *a=record;
        return !a->settings||a->timestamp||a->image_size||a->file_size||
            (a->action!=1&&a->action!=2)||a->bytes!=(a->action==1?65u:sizeof(struct ciuki_w32_compat))||
            (a->action==2&&(a->source||a->source_bytes))?-EINVAL:0;}
    case W32_REPORT:{const struct ciuki_w32_report_args *a=record;
        if(a->result||a->action<1||a->action>5)return -EINVAL;
        if(a->action==W32_REPORT_NEXT)return a->code||a->context||a->module||a->base||!a->buffer||a->bytes!=sizeof(struct ciuki_w32_fault)?-EINVAL:0;
        if(a->action==W32_REPORT_CLOSE||a->action==W32_REPORT_REASON)return a->context||a->module||a->base||a->buffer||a->bytes?-EINVAL:0;
        return a->buffer||a->bytes||(a->action==W32_REPORT_CRASH&&!a->context)?-EINVAL:0;}
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
        return a->tid || a->flags & ~(W32_THREAD_RESERVATION|W32_CREATE_SUSPENDED) ? -EINVAL : 0;
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
    case W32_SNAPSHOT: {
        const struct ciuki_w32_snapshot_args *a=record;
        return a->result||a->error||a->action<1||a->action>3?-EINVAL:0;
    }
    case W32_PROCESS: {
        const struct ciuki_w32_spawn_args *a=record;
        return !a->image||!a->command||!a->environment||a->environment_bytes<2||a->environment_bytes>32768||a->inherit>1||a->handle_flags&~3u||a->process_handle||a->thread_handle||a->pid||a->tid||a->error?-EINVAL:0;
    }
    case W32_FILE: {
        const struct ciuki_w32_file_args *a=record;
        return a->result||a->error||a->action<1||a->action>W32_F_DISK?-EINVAL:0;
    }
    case W32_SYSTEM: {
        const struct ciuki_w32_system_args *a=record;
        return a->result||a->error||a->low||a->high||(a->extra&&(a->action!=W32_S_TERMINATE_THREAD||a->handle))||a->action<1||a->action>W32_S_QUERY_PRIORITY?-EINVAL:0;
    }
    case W32_LOADER: {
        const struct ciuki_w32_loader_args *a=record;
        if(a->action==W32_LDR_OPEN)return !a->path||a->source||a->bytes||a->address||a->flags?-EINVAL:0;
        if(a->action==W32_LDR_IMAGE)return a->path||!a->source||!a->bytes||a->flags&~7u?-EINVAL:0;
        if(a->action==W32_LDR_UNMAP)return a->path||a->source||a->bytes||!a->address||a->flags?-EINVAL:0;
        return -EINVAL;
    }
    case W32_WORD: {
        const struct ciuki_w32_word_args *a=record;
        return a->reserved||a->wake>1||(a->address&3)||!a->address?-EINVAL:0;
    }
    case W32_OBJECT: {const struct ciuki_w32_object_args *a=record;return a->reserved||a->result||a->error||a->action<1||a->action>W32_OBJ_TIMER_CANCEL?-EINVAL:0;}
    case W32_WAIT: {const struct ciuki_w32_wait_args *a=record;return a->reserved||a->result||a->error||a->count>64||a->all>1||a->alertable>1||a->wake_mask&~W32_QS_ALL?-EINVAL:0;}
    case W32_APC: {const struct ciuki_w32_apc_args *a=record;return a->result||a->error||a->action<1||a->action>3?-EINVAL:0;}
    case W32_CONTEXT: {const struct ciuki_w32_context_args *a=record;return a->result||a->error||a->action<1||a->action>4?-EINVAL:0;}
    case W32_MESSAGE: {const struct ciuki_w32_message_args *a=record;return a->result||a->error||a->action<1||a->action>6||a->bits&~W32_QS_ALL?-EINVAL:0;}
    case W32_SECTION: {const struct ciuki_w32_section_args *a=record;return a->result||a->error||a->flags&~1u?-EINVAL:0;}
    case W32_VIEW: {const struct ciuki_w32_view_args *a=record;return a->reserved||a->result||a->error||a->action<1||a->action>3?-EINVAL:0;}
    case W32_SERVICE: {const struct ciuki_w32_service_args *a=record;return a->result||a->error||a->action<1||a->action>6?-EINVAL:0;}
    case W32_DISPLAY: {
        const struct ciuki_w32_display_args *a=record;
        if (a->result || a->error || a->action<1 || a->action>5) return -EINVAL;
        if (a->action==W32_DISPLAY_GRANT) return a->token || a->generation || a->address || a->first || a->count ? -EINVAL : 0;
        if (a->mode || a->scale || !zero_bytes(&a->surface,sizeof(a->surface))) return -EINVAL;
        if (a->action!=W32_DISPLAY_PALETTE && (a->first || a->count)) return -EINVAL;
        return a->action==W32_DISPLAY_REVOKE && a->address ? -EINVAL : 0;
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
