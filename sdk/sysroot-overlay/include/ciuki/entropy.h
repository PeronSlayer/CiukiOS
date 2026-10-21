/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_ENTROPY_H
#define CIUKI_SDK_ENTROPY_H
#include <stddef.h>
#include <stdint.h>
/* getentropy is nonblocking: EAGAIN means unavailable, EINVAL means >256.
 * The wait helper retries EAGAIN with 10 ms sleeps, potentially indefinitely;
 * it returns -1 on an interrupted sleep or any permanent error. */
int getentropy(void *, size_t);
int ciuki_entropy_wait(void *, size_t);
/* Compatibility alias for ciuki_entropy_wait. */
int ciuki_getentropy_wait(void *, size_t);
/* arc4random* aborts with a diagnostic after 5 s of EAGAIN or on permanent
 * errors; the deadline spans the entire call, including chunks/draws. */
uint32_t arc4random(void);
void arc4random_buf(void *, size_t);
uint32_t arc4random_uniform(uint32_t);
#endif
