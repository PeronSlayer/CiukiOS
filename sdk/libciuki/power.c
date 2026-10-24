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
/* G11 (docs/design/power-policy.md): the cached EVENTS counters and the
 * desktop's drive standby timer. */
int ciuki_power_events(struct ciuki_power_events *events)
{
    if (!events) { __getreent()->_errno = EINVAL; return -1; }
    events->version = CIUKI_POWER_EVENTS_VERSION; events->size = sizeof(*events); events->reserved = 0;
    uint32_t result = CU_CALL(POWER, CIUKI_POWER_EVENTS, CU_PTR(events), sizeof(*events), 0, 0, 0);
    int error = ciuki_error(result);
    if (error) { __getreent()->_errno = error; return -1; }
    return 0;
}
int ciuki_power_policy(struct ciuki_power_policy *policy)
{
    if (!policy) { __getreent()->_errno = EINVAL; return -1; }
    policy->version = CIUKI_POWER_POLICY_VERSION; policy->size = sizeof(*policy); policy->reserved = 0;
    uint32_t result = CU_CALL(POWER, CIUKI_POWER_POLICY, CU_PTR(policy), sizeof(*policy), 0, 0, 0);
    int error = ciuki_error(result);
    if (error) { __getreent()->_errno = error; return -1; }
    return 0;
}
