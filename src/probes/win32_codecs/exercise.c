/* Public ACM/VCM decodes compared with authored scalar goldens. LGPL-2.1-or-later */
#include "../../../sdk/win32/codecs/codec.h"
#include "../win32_core/fixture.h"
#include "samples.h"
extern void *W32API LoadLibraryA(const char *);
extern int32_t W32API FreeLibrary(void *);
extern uint32_t W32API acmFormatSuggest(uintptr_t,const struct cc_wave *,struct cc_wave *,uint32_t,uint32_t);
extern uint32_t W32API acmStreamOpen(uintptr_t *,uintptr_t,const struct cc_wave *,const struct cc_wave *,void *,uintptr_t,uintptr_t,uint32_t);
extern uint32_t W32API acmStreamPrepareHeader(uintptr_t,struct cc_acm_header *,uint32_t);
extern uint32_t W32API acmStreamConvert(uintptr_t,struct cc_acm_header *,uint32_t);
extern uint32_t W32API acmStreamUnprepareHeader(uintptr_t,struct cc_acm_header *,uint32_t);
extern uint32_t W32API acmStreamClose(uintptr_t,uint32_t);
extern uintptr_t W32API ICOpen(uint32_t,uint32_t,uint32_t);
extern intptr_t W32API ICSendMessage(uintptr_t,uint32_t,uintptr_t,uintptr_t);
extern int32_t W32API ICDecompress(uintptr_t,uint32_t,struct cc_bitmap *,const void *,struct cc_bitmap *,void *);
extern int32_t W32API ICClose(uintptr_t);
static int equal(const uint8_t *a,const uint8_t *b,unsigned n)
{for(unsigned i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;}
static void audio(const uint8_t *wave,const uint8_t *source,unsigned bytes,const uint8_t *golden,unsigned count)
{
    struct cc_wave dest={0};uintptr_t stream=0;uint8_t decoded[512]={0};
    CHECK(0,!acmFormatSuggest(0,(const struct cc_wave *)wave,&dest,sizeof(dest),0));
    CHECK(0,!acmStreamOpen(&stream,0,(const struct cc_wave *)wave,&dest,0,0,0,0));
    struct cc_acm_header header={.size=sizeof(header),.src=(uint8_t *)source,.src_len=bytes,.dst=decoded,.dst_len=sizeof(decoded)};
    CHECK(0,!acmStreamPrepareHeader(stream,&header,0));
    CHECK(0,!acmStreamConvert(stream,&header,0x30));
    CHECK(0,header.src_used==bytes && header.dst_used==count && equal(decoded,golden,count));
    CHECK(0,!acmStreamUnprepareHeader(stream,&header,0));CHECK(0,!acmStreamClose(stream,0));
}
static void video(uint32_t kind,unsigned bits,uint32_t compression,const uint8_t *frame,unsigned bytes,const uint8_t *golden,unsigned count)
{
    uintptr_t decoder=ICOpen(CC_FOUR('v','i','d','c'),kind,2);uint8_t output[64]={0};
    CHECK(0,decoder);
    struct cc_bitmap in={.size=40,.width=4,.height=4,.planes=1,.bits=(uint16_t)bits,.compression=compression,.bytes=bytes};
    struct cc_bitmap out=in;out.compression=0;out.bits=kind==CC_FOUR('c','v','i','d') ? 24 : (uint16_t)bits;out.bytes=count;
    CHECK(0,!ICSendMessage(decoder,0x400c,(uintptr_t)&in,(uintptr_t)&out));
    CHECK(0,!ICDecompress(decoder,0,&in,frame,&out,output));
    CHECK(0,equal(output,golden,count));CHECK(0,!ICClose(decoder));
}
void W32API entry(void)
{
    fixture_begin(W32_FIXTURE_CODECS);
    const char *names[]={"msacm32.dll","imaadp32.acm","msadp32.acm","msvfw32.dll","avifil32.dll","msrle32.dll","msvidc32.dll","iccvid.dll"};
    void *modules[8]={0};
    for(unsigned i=0;i<8;i++){modules[i]=LoadLibraryA(names[i]);CHECK(0,modules[i]);if(modules[i])result->row[0].value[2]++;}
    unsigned errors=result->row[0].errors;
    audio(ima_wave,ima_block,sizeof(ima_block),ima_golden,sizeof(ima_golden));
    audio(ms_wave,ms_block,sizeof(ms_block),ms_golden,sizeof(ms_golden));
    result->row[0].value[0]=errors==result->row[0].errors;
    errors=result->row[0].errors;
    video(CC_FOUR('m','r','l','e'),8,1,rle_frame,sizeof(rle_frame),rle_golden,sizeof(rle_golden));
    video(CC_FOUR('m','s','v','c'),16,CC_FOUR('m','s','v','c'),msvc_frame,sizeof(msvc_frame),msvc_golden,sizeof(msvc_golden));
    video(CC_FOUR('c','v','i','d'),24,CC_FOUR('c','v','i','d'),cinepak_frame,sizeof(cinepak_frame),cinepak_golden,sizeof(cinepak_golden));
    result->row[0].value[1]=errors==result->row[0].errors;
    for(unsigned i=8;i;i--)if(modules[i-1])CHECK(0,FreeLibrary(modules[i-1]));
    fixture_finish();
}
