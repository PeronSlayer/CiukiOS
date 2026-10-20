/* SPDX-License-Identifier: LGPL-2.1-or-later
 * H1-only transport of the adapter's PE ntdll (never part of an image): it
 * replaces services/ciuki_stub.c and ntdll/server.c. CIUKI_SYS_W32 records
 * and w32srv exchanges go to the adapter host through two function pointers
 * the host stores in its gate page (0x7ffe0200) before any PE code runs;
 * without them a record fails with ENOSYS and an exchange with 1062. */
#include <w32/ntdll.h>
#define H1_HOOKS ((void *volatile *)(uintptr_t)0x7ffe0200u)
int32_t w32_ciuki_call(void *record, uint32_t size) {
    int32_t (*hook)(void *, uint32_t) = (int32_t (*)(void *, uint32_t))H1_HOOKS[0];
    return hook ? hook(record, size) : -38;
}
uint32_t w32_server_exchange(const void *in, uint32_t n, void *out, uint32_t cap, uint32_t *used) {
    uint32_t (*hook)(const void *, uint32_t, void *, uint32_t, uint32_t *) =
        (uint32_t (*)(const void *, uint32_t, void *, uint32_t, uint32_t *))H1_HOOKS[1];
    if (used)
        *used = 0;
    return hook ? hook(in, n, out, cap, used) : 1062;
}
