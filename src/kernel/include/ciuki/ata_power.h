/* G11 drive standby timer (docs/design/power-policy.md): ATA/ATAPI-6 8.14
 * IDLE (E3h) Count register encoding, shared by drivers/ata.c and call 73.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ATA_POWER_H
#define CIUKI_ATA_POWER_H
#include <stdint.h>
struct ata_device;
struct ata_standby_result {
    uint32_t disks;           /* fixed disks that accepted the timer */
    uint32_t refused;         /* without the feature set, or ABRT */
    uint32_t applied_seconds; /* the programmed period */
    int error;                /* negative errno of the first failure */
};
/* 0 disables the timer; 1..240 count 5 s; 241..251 count (n - 240) x 30 min.
 * The period is rounded up, so a drive never spins down earlier than asked;
 * anything above 5 h 30 min is clamped to count 251. */
static inline uint8_t ata_standby_count(uint32_t seconds, uint32_t *applied)
{
    uint32_t n;
    if (!seconds) { *applied = 0; return 0; }
    if (seconds <= 240u * 5u) { n = (seconds + 4u) / 5u; *applied = n * 5u; return (uint8_t)n; }
    n = (seconds + 1799u) / 1800u;
    if (n > 11u) n = 11u;
    *applied = n * 1800u;
    return (uint8_t)(240u + n);
}
/* Programs every identified fixed disk; returns how many there are (a
 * refusal is FS_EOPNOTSUPP in error, the ENOTSUP value). */
int ata_standby_timer_all(uint32_t seconds, struct ata_standby_result *out);
/* One disk: 0, -FS_EOPNOTSUPP (no feature set, or ABRT), or a channel fault. */
int ata_standby_timer(struct ata_device *, uint8_t count);
#endif
