/* Freestanding single-owner jitterentropy adapter. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ENTROPY_JENT_H
#define CIUKI_ENTROPY_JENT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ENTROPY_COLLECT_MS 5000u
#define ENTROPY_COLLECT_ROUNDS 131072u
struct entropy_jent_ops {
    uint64_t (*clock)(void);
    uint64_t (*ms)(void);
    void *(*alloc)(size_t);
    void (*free)(void *);
    void (*yield)(void);
    void (*raw)(uint64_t delta, uint64_t gcd, unsigned osr);
};
/* init performs upstream startup tests and allocates one persistent collector.
 * begin bounds a new collection attempt, including any startup work. */
void entropy_jent_begin(const struct entropy_jent_ops *);
int entropy_jent_init(void);
int entropy_jent_read(uint8_t out[32]);
void entropy_jent_close(void);
unsigned entropy_jent_rounds(void);
bool entropy_jent_expired(void);
#endif
