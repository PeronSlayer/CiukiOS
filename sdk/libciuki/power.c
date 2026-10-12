/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/power.h>
int ciuki_power(uint32_t action)
{
    uint32_t result = CU_CALL(POWER, action, 0, 0, 0, 0, 0);
    int error = ciuki_error(result);
    if (error) { __getreent()->_errno = error; return -1; }
    return (int)result;
}
