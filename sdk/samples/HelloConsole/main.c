/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdint.h>
int main(void)
{
    uint64_t count = UINT64_C(4294967297);
    printf("Hello from CiukiOS! %llu\n", (unsigned long long)count);
    return 0;
}
