/* Exact SDK marshalling over a fake syscall boundary. SPDX-License-Identifier: MIT */
#define CIUKI_RUNTIME_H
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <ciuki/abi.h>
static struct fake_reent { int _errno; } errors;
static struct fake_reent *__getreent(void) { return &errors; }
static uint32_t result, called, action, address, bytes;
static uint32_t call(uint32_t n, uint32_t b, uint32_t c, uint32_t d)
{ called=n; action=b; address=c; bytes=d; return result; }
#define CU_PTR(p) ((uint32_t)(uintptr_t)(p))
#define CU_CALL(n,b,c,d,s,i,p) call(CIUKI_SYS_##n,b,c,d)
static int ciuki_error(uint32_t n) { return n >= (uint32_t)-4095 ? -(int32_t)n : 0; }
#include "../../sdk/libciuki/power.c"
int main(void)
{
    struct ciuki_power_status p={0};
    assert(!ciuki_power_status(&p));
    assert(called==73 && action==4 && bytes==756 && address==CU_PTR(&p));
    assert(p.version==1 && p.size==756 && !p.reserved);
    result=(uint32_t)-EFAULT; assert(ciuki_power_status(&p)==-1 && errors._errno==EFAULT);
    assert(ciuki_power_status(NULL)==-1 && errors._errno==EINVAL);
    result=3; assert(ciuki_power(CIUKI_POWER_QUERY)==3 && action==3 && !address && !bytes);
    /* G11: EVENTS and POLICY records. */
    struct ciuki_power_events e={.reserved=9}; result=0;
    assert(!ciuki_power_events(&e) && called==73 && action==7 && bytes==40 && address==CU_PTR(&e));
    assert(e.version==1 && e.size==40 && !e.reserved);
    result=(uint32_t)-EINVAL; assert(ciuki_power_events(&e)==-1 && errors._errno==EINVAL);
    errors._errno=0; assert(ciuki_power_events(NULL)==-1 && errors._errno==EINVAL);
    struct ciuki_power_policy q={.disk_seconds=600,.reserved=3}; result=0;
    assert(!ciuki_power_policy(&q) && action==6 && bytes==32 && address==CU_PTR(&q) && q.version==1 && q.size==32 && !q.reserved && q.disk_seconds==600);
    result=(uint32_t)-EPERM; assert(ciuki_power_policy(&q)==-1 && errors._errno==EPERM);
    result=(uint32_t)-ENOTSUP; assert(ciuki_power(CIUKI_POWER_STANDBY)==-1 && action==5 && errors._errno==ENOTSUP);
    puts("PASS power SDK: exact call/extent/header, errno and capability query; G11 EVENTS, POLICY, STANDBY"); return 0;
}
