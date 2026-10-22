/* Freestanding single-owner jitterentropy adapter. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ENTROPY_JENT_H
#define CIUKI_ENTROPY_JENT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ENTROPY_COLLECT_MS 5000u
#define ENTROPY_COLLECT_ROUNDS 131072u
/* Upstream JENT_MIN_OSR/JENT_MAX_OSR (jitterentropy-internal.h, v3.7.0). */
#define ENTROPY_JENT_MIN_OSR 3u
#define ENTROPY_JENT_MAX_OSR 20u
/* The measurement a round belongs to: the jent_entropy_init_ex power-up test
 * (combined noise), the FIPS/NTG.1 startup stages that sample the memory
 * access and the SHA-3 hash loop separately, then output generation. */
enum { ENTROPY_JENT_POWERUP, ENTROPY_JENT_MEMORY, ENTROPY_JENT_SHA3, ENTROPY_JENT_OUTPUT, ENTROPY_JENT_STAGES };
struct entropy_jent_ops {
    uint64_t (*clock)(void);
    uint64_t (*ms)(void);
    void *(*alloc)(size_t);
    void (*free)(void *);
    void (*yield)(void);
    void (*raw)(uint64_t delta, uint64_t gcd, unsigned osr);
};
/* First health alarm the library reported (jent_set_fips_failure_callback)
 * during the current attempt; budget expiry is not a health alarm. */
struct entropy_jent_report {
    unsigned alarm, alarm_stage, alarm_osr, alarm_round;
    unsigned osr, rounds[ENTROPY_JENT_STAGES];
    int result;          /* last jent_entropy_init_ex/jent_read_entropy_safe code, or -1 */
    size_t refused;      /* largest allocation refused by the 256 KiB bound */
    bool permanent;      /* a permanent health failure, not this adapter's budget */
};
/* begin bounds a new collection attempt (time and rounds), including startup work.
 * init runs jent_entropy_init_ex(osr, flags) and allocates one persistent
 * collector with the identical osr/flags (jitterentropy(3)); a failed startup
 * frees it and allocates a fresh instance with osr+1, up to last_osr. */
void entropy_jent_begin(const struct entropy_jent_ops *);
int entropy_jent_init(unsigned first_osr, unsigned last_osr);
/* jent_read_entropy_safe: intermittent alarms re-allocate with osr+1. */
int entropy_jent_read(uint8_t out[32]);
void entropy_jent_close(void);
unsigned entropy_jent_rounds(void);
bool entropy_jent_expired(void);
unsigned entropy_jent_stage(void);       /* stage of the current/last round */
void entropy_jent_report(struct entropy_jent_report *);
#endif
