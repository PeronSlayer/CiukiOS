/* Production lwIP seed switch with endpoint stubs; no network worker needed.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/net_stack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL entropy lwIP %u: %s\n",__LINE__,#x); abort(); } } while(0)
static unsigned requests, seeds;
static uint64_t last_seed;
static bool ready;
int entropy_get(void *out,size_t bytes)
{
    requests++;CHECK(bytes==8);
    if(!ready)return -EAGAIN;
    uint64_t secret=UINT64_C(0x123456789abcdef0);memcpy(out,&secret,sizeof(secret));return 0;
}
void ciuki_lwip_seed(uint64_t seed) { last_seed=seed;seeds++; }
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    net_stack_entropy(123);CHECK(requests==1 && seeds==1 && last_seed==123);
    net_stack_entropy(456);CHECK(requests==2 && seeds==2 && last_seed==456);
    ready=true;net_stack_entropy(789);CHECK(requests==3 && seeds==3 && last_seed==UINT64_C(0x123456789abcdef0));
    net_stack_entropy(999);CHECK(requests==3 && seeds==3 && last_seed==UINT64_C(0x123456789abcdef0));
    puts("PASS production lwIP legacy mix until DRBG readiness, one-way seed switch and later legacy input refusal");
    return 0;
}
