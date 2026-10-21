/* Non-entropy harnesses exercise the existing stack-guard fallback.
 * The real entropy/guard paths are covered by entropy/kernel_test.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/entropy.h>
#include <ciuki/entropy_pool.h>

void entropy_boot(void) { }
int entropy_get(void *out, size_t bytes)
{
    (void)out; (void)bytes;
    return -EAGAIN;
}
void entropy_erase(void *out, size_t bytes)
{
    volatile uint8_t *p = out;
    while (bytes--) *p++ = 0;
}
