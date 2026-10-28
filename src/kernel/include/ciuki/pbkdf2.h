/* sec-k08: PBKDF2-HMAC-SHA-256 (RFC 8018 5.2) for the self-test's account
 * verifier timing (docs/validation/sec-k08.md). The same derivation as the
 * ACCOUNT service's acct_derive (apps/accounts/crypto.c): the password is
 * the HMAC key (at most 64 bytes, zero-padded), one 32-byte block, salt
 * followed by INT_32_BE(1). H13 checks both give the same bytes.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_PBKDF2_H
#define CIUKI_PBKDF2_H
#include <stdint.h>
#include <stddef.h>
/* The ACCOUNT service's work factor (apps/accounts/accounts.h ACCT_ITERATIONS). */
#define CIUKI_ACCOUNT_KDF_ITERATIONS 10000u
/* 0, or -1 for a key over 64 bytes, a salt over 60 bytes or no iteration. */
int pbkdf2_hmac_sha256(const void *key, size_t key_bytes, const void *salt, size_t salt_bytes,
                       uint32_t iterations, uint8_t out[32]);
#endif
