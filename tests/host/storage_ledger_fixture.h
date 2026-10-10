/* Host storage fixtures supply a retained boot snapshot to production probes.
 * Production append/order behavior is tested by runtime_init_test.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/init.h>
static struct activation_entry host_mounts[26];
static void host_mount_snapshot(struct storage *s)
{
    memset(host_mounts, 0, sizeof(host_mounts));
    for (unsigned d = 2; d < 26; d++) {
        const struct storage_volume *v = &s->volumes[d];
        if (!v->present) continue;
        host_mounts[d] = (struct activation_entry){ .kind = ACTIVATION_MOUNT,
            .present = true, .drive = d, .disk = v->disk, .partition = v->partition,
            .type = v->fat.type, .readonly = v->fat.readonly, .read_gate = v->read_gate,
            .error = v->error, .reasons = v->fat.ro_reasons, .writes = v->writes,
            .reason = "mount" };
    }
}
const struct activation_entry *drivers_mount_get(unsigned drive)
{
    return drive < 26 && host_mounts[drive].present ? &host_mounts[drive] : 0;
}
