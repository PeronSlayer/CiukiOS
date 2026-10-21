/* Reconstructed b51f034f trial, not a transcript with measured timestamps.
 * Only histogram counts, maxima and log-write mean are known. Bin interiors
 * and the ordering below are deterministic workload assumptions.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_HOST_T23_TRIAL_H
#define CIUKI_HOST_T23_TRIAL_H
#define T23_DESKTOP_HOLDS 513u
#define T23_LOG_HOLDS 35u
#define T23_SESSION_MS 400000u
#define T23_SERVICE_MS 111u
static inline unsigned t23_desktop_hold_ms(unsigned sequence)
{
    unsigned bin = sequence * 197u % T23_DESKTOP_HOLDS;
    return bin < 406 ? 125 : bin < 497 ? 350 : bin < 507 ? 750 : bin < 512 ? 1250 : 1502;
}
static inline unsigned t23_log_hold_ms(unsigned sequence)
{
    /* (34 * 1627 + 1837) / 35 == the measured mean of 1633 ms. */
    return sequence < 34 ? 1627 : 1837;
}
static inline unsigned t23_arrival_ms(unsigned sequence)
{
    return 1000u + sequence * 770u;
}
/* Device-class work measured in the same trial (directive fact 6):
 * `[work] budget exceeded ... us~2110` on device-work, unclassified
 * IRQ-off sections of ~2.1 ms at ~3.9 per second, USB host scans at
 * 3.980 per second. Reconstructed as one 2110 us device-work turn every
 * 251 ms (3.98 per second); every section at the observed value is an
 * upper-bound assumption, and the phase relative to the holds is arbitrary. */
#define T23_DEVICE_WORK_US 2110u
#define T23_DEVICE_WORK_PERIOD_MS 251u
/* Directive fact 2: device_overrun task=audio-refill in 192 of 201 windows. */
#define T23_OVERRUN_WINDOWS 192u
#define T23_REPORT_WINDOWS 201u
#endif
