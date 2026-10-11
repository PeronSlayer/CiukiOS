/* SPDX-License-Identifier: MIT
 * ciuki_power (ciuki/power.h). Weak, so that a kernel-side SDK update that
 * names call 73 in abi.h can provide its own definition without a clash. */
#include <ciuki/runtime.h>
#include <ciuki/power.h>
__attribute__((weak)) int ciuki_power(uint32_t action)
{
    uint32_t value = ciuki_syscall(CIUKI_POWER_CALL, action, 0, 0, 0, 0, 0);
    int error = ciuki_error(value);
    if (error) { __getreent()->_errno = error; return -1; }
    return (int)value;
}
