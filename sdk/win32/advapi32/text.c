/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
static const uint16_t cp1252[32]={0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
    0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,0x201d,
    0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
uint32_t w32r_ansi_scalar(uint8_t c){return c>=128&&c<160?cp1252[c-128]:c;}
uint8_t w32r_scalar_ansi(uint32_t c){if(c<256&&(c<128||c>=160))return (uint8_t)c;for(unsigned i=0;i<32;i++)if(cp1252[i]==c)return (uint8_t)(i+128);return '?';}
int w32r_to_utf8(char *out,uint32_t cap,const void *input,int wide)
{
    const uint8_t *a=input;const uint16_t *w=input;uint32_t at=0;
    if(!input){if(cap)out[0]=0;return cap!=0;}
    for(uint32_t i=0;;i++){
        uint32_t c=wide?w[i]:w32r_ansi_scalar(a[i]);if(!c){if(at>=cap)return 0;out[at]=0;return 1;}
        if(wide&&c>=0xd800&&c<=0xdbff){uint32_t low=w[++i];if(low<0xdc00||low>0xdfff)return 0;c=0x10000+((c-0xd800)<<10)+(low-0xdc00);}
        else if(wide&&c>=0xdc00&&c<=0xdfff)return 0;
        unsigned n=c<128?1:c<2048?2:c<65536?3:4;if(n>=cap-at)return 0;
        if(n==1)out[at++]=(char)c;
        else{for(unsigned j=0;j<n;j++){unsigned shift=(n-1-j)*6;out[at++]=(char)(j?0x80|((c>>shift)&63):(n==2?0xc0:n==3?0xe0:0xf0)|(c>>shift));}}
    }
}
uint32_t w32r_from_utf8(void *out,uint32_t cap,const char *s,int wide)
{
    uint32_t n=0;const uint8_t *p=(const uint8_t *)s;
    while(*p){uint32_t c=*p++;unsigned extra=c<128?0:c<224?1:c<240?2:3;c&=extra?((1u<<(6-extra))-1):127;
        while(extra--)c=(c<<6)|(*p++&63);
        if(wide&&c>65535){if(out&&n<cap)((uint16_t *)out)[n]=(uint16_t)(0xd800+((c-0x10000)>>10));n++;c=0xdc00+((c-0x10000)&1023);}
        if(out&&n<cap){if(wide)((uint16_t *)out)[n]=(uint16_t)c;else((uint8_t *)out)[n]=w32r_scalar_ansi(c);}n++;
    }
    if(out&&n<cap){if(wide)((uint16_t *)out)[n]=0;else((uint8_t *)out)[n]=0;}return n;
}
