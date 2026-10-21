/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_SDK_ENTROPY_H
#define CIUKI_SDK_ENTROPY_H
#include <stddef.h>
#include <stdint.h>
/* getentropy is nonblocking: EAGAIN means unavailable, EINVAL means >256.
 * The wait helper retries EAGAIN with 10 ms sleeps, potentially indefinitely;
 * it returns -1 on an interrupted sleep or any permanent error. */
int getentropy(void *, size_t);
int ciuki_getentropy_wait(void *, size_t);
uint32_t arc4random(void);
void arc4random_buf(void *, size_t);
uint32_t arc4random_uniform(uint32_t);
#endif
