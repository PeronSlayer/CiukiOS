/* SPDX-License-Identifier: GPL-2.0-only
 * Compile on i386; assembly callers test exact stack/register conventions. */
#include "src/kernel/ndis/callback.h"
_Static_assert(sizeof(void *)==4,"i386 ABI fixture");
struct unicode32 { uint16_t length, maximum; uint32_t buffer; };
_Static_assert(sizeof(struct unicode32)==8,"UNICODE_STRING32");
_Static_assert(offsetof(struct unicode32,buffer)==4,"UNICODE_STRING32 buffer");
volatile uint32_t abi_tick_count[3]={7,7,7};
__attribute__((noinline)) int32_t NDIS_STDCALL abi_std(uint32_t a,uint32_t b) { return a*10+b; }
__attribute__((noinline)) uint32_t NDIS_FASTCALL abi_kf(uint32_t a,uint32_t b,uint32_t c) { return a*100+b*10+c; }
__attribute__((noinline)) uint32_t NDIS_CDECL abi_cdecl(uint32_t a,uint32_t b) { return a-b; }
extern int abi_probe(void);
int abi_main(void) { return abi_probe(); }

bool registry_valid(int handle,gen_t generation) { (void)handle;(void)generation;return true; }
int registry_quarantine(int handle,gen_t generation) { (void)handle;(void)generation;return 0; }
static bool fence(void *ctx,const char *why) { (void)ctx;(void)why;return true; }
static struct ntimage image={.pe={.base=0xc1000000,.size=0x2000,.section_count=1,
    .sections={{.rva=0x1000,.span=512,.flags=0x60000020}}},.loaded=true};
static struct ndis_callback callback={.image=&image,.ops={.fence=fence},.dirql=5};
int abi_callback_probe(void) {
    int32_t result=0;
    if(!ndis_callback_model_call2(&callback,0xc1001000,abi_std,3,4,0,1,10,&result)) return 1;
    return result!=34 || callback.irql!=0 || callback.state!=NDIS_CALLBACK_IDLE;
}
