/* NT registry entry points over the w32srv registry protocol (f5-07A).
 * Key handles are the service's key identifiers, so ADVAPI32 accepts them as
 * HKEYs and the reverse. NtClose cannot tell a registry identifier from a
 * kernel handle by value, so keys opened here are remembered until closed;
 * ADVAPI32's RegCloseKey reports its closes through w32_registry_forget.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include <w32/ldr.h>
#include <w32/server.h>
#define STATUS_OBJECT_NAME_INVALID ((NTSTATUS)0xc0000033)
#define STATUS_OBJECT_PATH_SYNTAX_BAD ((NTSTATUS)0xc000003b)
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xc0000005)
NTSTATUS w32_status_from_error(ULONG);
struct w32_object_attributes {
    ULONG Length;
    void *RootDirectory;
    const UNICODE_STRING *ObjectName;
    ULONG Attributes;
    void *SecurityDescriptor, *SecurityQualityOfService;
};
#define TRACKED 256u
static volatile ULONG tracked_lock;
static ULONG tracked[TRACKED];
static void lock(void) {
    while (__atomic_exchange_n(&tracked_lock, 1, __ATOMIC_ACQUIRE))
        ;
}
static void unlock(void) {
    __atomic_store_n(&tracked_lock, 0, __ATOMIC_RELEASE);
}
static void remember(ULONG key) {
    lock();
    for (ULONG i = 0; i < TRACKED; i++)
        if (!tracked[i]) {
            tracked[i] = key;
            break;
        }
    unlock();
}
static int forget(ULONG key) {
    int found = 0;
    lock();
    for (ULONG i = 0; i < TRACKED && !found; i++)
        if (key && tracked[i] == key) {
            tracked[i] = 0;
            found = 1;
        }
    unlock();
    return found;
}
void w32_registry_forget(ULONG key) {
    forget(key);
}
/* Win9x registries carry no security: generic and MAXIMUM_ALLOWED rights
 * become the corresponding KEY_* rights and the WOW64 view bits, meaningless
 * on 32-bit Windows, are dropped before the service validates the mask. */
ULONG w32_registry_access(ULONG a) {
    static const ULONG map[5][2] = {{0x02000000u, 0xf003fu}, {0x80000000u, 0x20019u},
                                    {0x40000000u, 0x20006u}, {0x20000000u, 0x20019u},
                                    {0x10000000u, 0xf003fu}};
    for (unsigned i = 0; i < 5; i++)
        if (a & map[i][0])
            a = (a & ~map[i][0]) | map[i][1];
    return a & ~0x300u;
}
static int prefix(const WCHAR *s, ULONG n, const char *p, ULONG *used) {
    ULONG i = 0;
    for (; p[i]; i++) {
        if (i >= n)
            return 0;
        WCHAR c = s[i], d = (uint8_t)p[i];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (d >= 'a' && d <= 'z')
            d -= 32;
        if (c != d)
            return 0;
    }
    if (i < n && s[i] != '\\')
        return 0;
    *used = i;
    return 1;
}
static NTSTATUS request(ULONG op, ULONG root, const WCHAR *name, ULONG chars, ULONG access,
                        struct w32r_reply *reply) {
    void *heap = NtCurrentTeb()->ProcessEnvironmentBlock->ProcessHeap;
    ULONG size = sizeof(struct w32r_request) + W32R_PATH + 1;
    uint8_t *in = heap ? RtlAllocateHeap(heap, 0, size) : 0;
    if (!in)
        return STATUS_NO_MEMORY;
    char *path = (char *)in + sizeof(struct w32r_request);
    if (!w32_utf16_path(name, chars, path, W32R_PATH)) {
        RtlFreeHeap(heap, 0, in);
        return STATUS_OBJECT_NAME_INVALID;
    }
    struct w32r_request r = {.version = W32R_VERSION, .op = op, .key = root, .access = access,
                             .path_bytes = w32_strlen(path) + 1, .name_bytes = 1};
    w32_copy(in, &r, sizeof(r));
    path[r.path_bytes] = 0;
    ULONG used = 0;
    ULONG e = w32_server_exchange(in, sizeof(r) + r.path_bytes + 1, reply, sizeof(*reply), &used);
    RtlFreeHeap(heap, 0, in);
    if (!e && (used != sizeof(*reply) || reply->version != W32R_VERSION || reply->name_bytes ||
               reply->data_bytes))
        e = 13;
    if (!e)
        e = reply->error;
    return w32_status_from_error(e);
}
static NTSTATUS open_key(void **out, ULONG access, const struct w32_object_attributes *attr,
                         ULONG op, ULONG *disposition) {
    if (!out || !attr || !attr->ObjectName)
        return STATUS_ACCESS_VIOLATION;
    *out = 0;
    const UNICODE_STRING *s = attr->ObjectName;
    if (s->Length & 1 || (s->Length && !s->Buffer))
        return STATUS_OBJECT_NAME_INVALID;
    const WCHAR *name = s->Buffer;
    ULONG chars = s->Length / 2, root = (ULONG)(uintptr_t)attr->RootDirectory, used = 0;
    if (!root) {
        if (prefix(name, chars, "\\Registry\\Machine", &used))
            root = W32R_HKLM;
        else if (prefix(name, chars, "\\Registry\\User", &used))
            root = W32R_HKU;
        else
            return (NTSTATUS)0xc0000034;
    } else if (chars && name[0] == '\\')
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    while (used < chars && name[used] == '\\')
        used++;
    struct w32r_reply reply = {0};
    NTSTATUS status = request(op, root, name + used, chars - used, w32_registry_access(access), &reply);
    if (status)
        return status;
    *out = (void *)(uintptr_t)reply.key;
    if (disposition)
        *disposition = reply.disposition;
    if (!(reply.key & 0x80000000u))
        remember(reply.key);
    return 0;
}
NTSTATUS W32API NtOpenKey(void **key, ULONG access, const struct w32_object_attributes *attr) {
    return open_key(key, access, attr, W32R_OPEN, 0);
}
/* REG_OPTION_VOLATILE is accepted as a persistent key, as Windows 9x does;
 * link and backup options have no Win9x meaning. Class and title index are
 * not stored (classes are returned empty by the service). */
NTSTATUS W32API NtCreateKey(void **key, ULONG access, const struct w32_object_attributes *attr,
                            ULONG title, const UNICODE_STRING *cls, ULONG options,
                            ULONG *disposition) {
    (void)title;
    (void)cls;
    if (options & ~1u)
        return STATUS_INVALID_PARAMETER;
    return open_key(key, access, attr, W32R_CREATE, disposition);
}
/* Deletion by handle: the f5-07A service deletes only a named subkey of an
 * open key and answers ERROR_ACCESS_DENIED for an empty name, which is
 * returned here unchanged until the service accepts the empty name. */
NTSTATUS W32API NtDeleteKey(void *key) {
    ULONG k = (ULONG)(uintptr_t)key;
    if (!k)
        return (NTSTATUS)0xc0000008;
    struct w32r_reply reply = {0};
    return request(W32R_DELETE_KEY, k, 0, 0, 0, &reply);
}
int w32_nt_key_close(void *handle) {
    ULONG key = (ULONG)(uintptr_t)handle;
    if (!forget(key))
        return 0;
    struct {
        struct w32r_request r;
        char empty[2];
    } a = {.r = {.version = W32R_VERSION, .op = W32R_CLOSE, .key = key, .path_bytes = 1,
                 .name_bytes = 1}};
    struct w32r_reply reply;
    ULONG used = 0;
    w32_server_exchange(&a, sizeof(a.r) + 2, &reply, sizeof(reply), &used);
    return 1;
}
