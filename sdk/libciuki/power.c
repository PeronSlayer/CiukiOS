/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/power.h>
#include <errno.h>
int ciuki_power(uint32_t action)
{
    uint32_t result = CU_CALL(POWER, action, 0, 0, 0, 0, 0);
    int error = ciuki_error(result);
    if (error) { __getreent()->_errno = error; return -1; }
    return (int)result;
}
int ciuki_power_status(struct ciuki_power_status *status)
{
    if (!status) { __getreent()->_errno = EINVAL; return -1; }
    status->version = CIUKI_POWER_VERSION; status->size = sizeof(*status); status->reserved = 0;
    uint32_t result = CU_CALL(POWER, CIUKI_POWER_STATUS, CU_PTR(status), sizeof(*status), 0, 0, 0);
    int error = ciuki_error(result);
    if (error) { __getreent()->_errno = error; return -1; }
    return 0;
}
