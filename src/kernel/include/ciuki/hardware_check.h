/* Boot-only diagnostic page; no public ABI. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_HARDWARE_CHECK_H
#define CIUKI_HARDWARE_CHECK_H
#include <stdint.h>
#include <stdbool.h>
#define HW_CHECK_ITEMS 9u
#define HW_CHECK_LIMIT_MS 15000u
enum hw_check_status { HW_PASS, HW_FAIL, HW_SKIP };
struct hw_check_result { enum hw_check_status status; char detail[144]; };
struct hw_check_ops {
    void *ctx;
    uint64_t (*now)(void *);
    void (*step)(void *, unsigned, uint64_t, struct hw_check_result *);
    void (*line)(void *, const char *);
};
void hardware_check_model(const struct hw_check_ops *, struct hw_check_result [HW_CHECK_ITEMS]);
void hardware_check_run(void);
/* Observe the ordinary workers only. A first scan is required before an
 * absent device can skip; enumeration/publication may still be pending. */
bool hardware_check_usb_wait_first_scan(uint64_t until, struct hw_check_result *, uint64_t (*now)(void *));
void hardware_check_usb_storage(uint64_t until, struct hw_check_result *, uint64_t (*now)(void *));
/* Current diagnostic task only; zero for every other task. Device commands
 * cap their existing deadline before issuance; a timed-out writer quarantines
 * through its existing completion/rollback protocol. */
uint64_t hardware_check_io_deadline(void) __attribute__((weak));
bool hardware_check_io_cancelled(void) __attribute__((weak));
#endif
