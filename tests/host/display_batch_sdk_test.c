/* Execute the real SDK wrapper bodies against literal register/envelope
 * expectations. Host pointers are retained only by this transport fixture.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned calls, wanted;
static void *blob_expected;
static struct ciuki_batch_reply *reply_expected;
static uint32_t raw(uint32_t number, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t s, uintptr_t i, uintptr_t p)
{
    assert(b==7 && !i && !p); calls++;
    assert(number==CIUKI_SYS_DISPLAY_ACCEL && !s);
    const struct ciuki_accel_request *q=(void *)c;
    assert(q->size==308 && q->operation==wanted);
    if (wanted==12) {
        assert(q->colour==1 && q->background==256 && q->generation==55);
        struct ciuki_command_info *out=(void *)d;
        *out=(struct ciuki_command_info){.size=256,.version=1,.capabilities=4}; return 0;
    }
    if (wanted==13) {
        assert(q->colour==1 && q->background==36 && q->rect.src_x==17);
        struct ciuki_batch_surface_info *out=(void *)d;
        *out=(struct ciuki_batch_surface_info){.size=36,.version=1,.generation=99}; return 0;
    }
    struct ciuki_accel_request canonical={.size=308,.operation=wanted};
    assert(!memcmp(q,&canonical,308));
    if (wanted==10) {
        const struct ciuki_batch_submit *e=(void *)d;
        assert(e->size==32 && e->version==1 && !e->flags && !e->reserved[0] && !e->reserved[1]);
        assert(e->input_address==(uint32_t)(uintptr_t)blob_expected && e->input_bytes==64);
        assert(e->reply_address==(uint32_t)(uintptr_t)reply_expected && e->reply_bytes==64);
        assert(reply_expected->size==64 && reply_expected->version==1 && !reply_expected->fence);
        *reply_expected=(struct ciuki_batch_reply){.size=64,.version=1,.state=3,.fence=UINT64_C(0x1122334455667788),
            .completed=2,.first_uncompleted=2,.error=-EIO,.flags=3,.admitted=3,.generation=55};
        return (uint32_t)-EIO;
    }
    const struct ciuki_fence_control *e=(void *)d;
    assert(e->size==32 && e->version==1 && e->action==2 && e->timeout_ms==17 && !e->reserved);
    assert(e->fence==UINT64_C(0x1122334455667788) && e->reply_bytes==64 && e->reply_address==(uint32_t)(uintptr_t)reply_expected);
    return 0; /* transport success; snapshot's operation error stays -EIO */
}
#define CU_PTR(p) ((uintptr_t)(p))
#define CU_CALL(n,b,c,d,s,i,p) raw(CIUKI_SYS_##n,(uintptr_t)(b),(uintptr_t)(c),(uintptr_t)(d),(uintptr_t)(s),(uintptr_t)(i),(uintptr_t)(p))
#include "batch_wrappers.inc"
int main(void)
{
    uint8_t blob[64]={0}; blob_expected=blob;
    struct ciuki_batch_reply reply; reply_expected=&reply;
    wanted=12; struct ciuki_command_info info;
    assert(!ciuki_display_commands(7,55,&info) && info.size==256 && info.capabilities==4);
    wanted=10; assert(ciuki_display_batch(7,blob,64,&reply)==-EIO && reply.fence && reply.completed==2 && reply.error==-EIO);
    wanted=11; assert(!ciuki_display_fence(7,reply.fence,2,17,&reply) && reply.error==-EIO);
    assert(ciuki_display_batch(7,blob,64,NULL)==-EINVAL && ciuki_display_fence(7,1,1,0,NULL)==-EINVAL);
    wanted=13; struct ciuki_batch_surface_info surface;
    assert(!ciuki_batch_surface_info(7,17,&surface) && surface.generation==99 && surface.size==36);
    assert(calls==4);
    puts("batch SDK: real wrappers, literal selectors/layouts/capacities, 64-bit fence, admitted error prefix and managed surface negotiation PASS");
    return 0;
}
