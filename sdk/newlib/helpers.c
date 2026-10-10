/* Target-only integer compiler helpers. SPDX-License-Identifier: MIT */
#include <stdint.h>
static uint64_t divide(uint64_t a,uint64_t b,uint64_t *rem) {
    if(!b)__builtin_trap();uint64_t q=0,r=0;
    for(int i=63;i>=0;--i) { unsigned carry=r>>63;r=(r<<1)|((a>>i)&1);if(carry||r>=b) { r-=b;q|=UINT64_C(1)<<i; } }
    if(rem)*rem=r;return q;
}
uint64_t __udivdi3(uint64_t a,uint64_t b) { return divide(a,b,0); }
uint64_t __umoddi3(uint64_t a,uint64_t b) { uint64_t r;divide(a,b,&r);return r; }
uint64_t __udivmoddi4(uint64_t a,uint64_t b,uint64_t *r) { return divide(a,b,r); }
int64_t __divdi3(int64_t a,int64_t b) { uint64_t ua=a<0?-(uint64_t)a:(uint64_t)a,ub=b<0?-(uint64_t)b:(uint64_t)b;uint64_t q=divide(ua,ub,0);return (a<0)!=(b<0)?-q:q; }
int64_t __moddi3(int64_t a,int64_t b) { uint64_t r;divide(a<0?-(uint64_t)a:(uint64_t)a,b<0?-(uint64_t)b:(uint64_t)b,&r);return a<0?-r:r; }
