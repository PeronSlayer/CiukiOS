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
    puts("PASS power SDK: exact call/extent/header, errno and capability query"); return 0;
}
