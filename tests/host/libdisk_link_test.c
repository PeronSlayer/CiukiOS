/* A client linking only -ldisk must extract the default backend member.
 * Compile twice: backend member and client. No client backend reference.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <libdisk.h>
#include <errno.h>
#include <stdio.h>

#ifdef LIBDISK_LINK_BACKEND
static int absent(void *ctx, unsigned disk, bool write)
{ (void)ctx; (void)disk; (void)write; return -ENXIO; }
const struct disk_backend *disk_native_backend(void)
{
    static const struct disk_backend b = { .open=absent };
    return &b;
}
#else
int main(void)
{
    struct disk_list list;
    if (disk_enumerate(&list) || list.count != DISK_SLOTS) return 1;
    for (unsigned i = 0; i < list.count; i++)
        if (list.disk[i].present || list.disk[i].refusal != DISK_REFUSE_ABSENT) return 1;
    puts("PASS libdisk archive: -ldisk extracts the default backend without a direct client reference");
    return 0;
}
#endif
