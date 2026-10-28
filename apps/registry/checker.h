/* sec-sysvc2: Registry Checker as the SYSTEM registry broker's work
 * (checker.c; engine apps/sysrecovery/regcheck.c; wire ops in
 * sdk/private/registry_protocol.h). SPDX-License-Identifier: MIT */
#ifndef CIUKI_REGISTRY_CHECKER_H
#define CIUKI_REGISTRY_CHECKER_H
#include <stddef.h>
#include <stdint.h>
struct rb_identity;
struct rb_checker {
    const char *root;                              /* "" on CiukiOS, a fixture directory on the host */
    void *context;
    int (*started)(void *);                        /* 1: this start completed (call 99 STATUS) */
    int64_t (*now)(void *, int *offset_minutes);   /* UTC seconds and the local offset */
    void (*log)(void *, const char *);             /* optional, one line */
    /* This start's work (rb_checker_boot), reported by STATUS. */
    unsigned restored;
    char from[16], applied[16];
    int boot_error;
};
/* At the broker's start, before any hive or root is loaded: a restore an
 * administrator requested, then each damaged root from the newest good
 * backup (Windows 98 ScanRegW at every start). */
void rb_checker_boot(struct rb_checker *);
/* One RB_CHECKER_* request of a live peer (identity already revalidated by
 * rb_exchange): 0 or -errno; administrators only except DAILY and STATUS. */
int rb_checker_exchange(struct rb_checker *, const struct rb_identity *, unsigned op,
                        const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *used);
#endif
