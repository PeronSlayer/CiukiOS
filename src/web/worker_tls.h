#ifndef CIUK_WORKER_TLS_H
#define CIUK_WORKER_TLS_H

#include <stddef.h>
#include <stdint.h>
#include "bearssl.h"

typedef int (*wtls_entropy_fn)(void *opaque, void *out, uint32_t bytes);

#define WTLS_OK       0
#define WTLS_AGAIN    1
#define WTLS_CLOSED   2
#define WTLS_FAILED  -1

/* One connection per worker. UTC seconds must be valid and nonzero. The
 * trust anchors and their backing storage must remain alive until close. */
int wtls_init(const char *host, uint32_t unix_utc,
    wtls_entropy_fn entropy, void *opaque,
    const br_x509_trust_anchor *anchors, size_t anchor_count);

/* Feed a byte stream from TCP. `consumed` may be smaller than input_bytes. */
int wtls_step(const void *cipher_input, uint32_t input_bytes,
    uint32_t *consumed);

/* Copy and acknowledge one pending TLS record. The caller must put the copied
 * bytes into its reliable TCP send queue before returning from this call. */
int wtls_pull_record(void *output, uint32_t capacity, uint32_t *produced);

/* Plaintext application stream. Each operation may consume/produce less than
 * requested; the caller repeats it for remaining bytes. */
int wtls_send_plaintext(const void *input, uint32_t input_bytes,
    uint32_t *consumed);
int wtls_pull_plaintext(void *output, uint32_t capacity, uint32_t *produced);

/* Begin TLS close_notify; drain it with wtls_pull_record(). */
int wtls_close(void);
int wtls_ready(void);
int wtls_last_error(void);

#endif
