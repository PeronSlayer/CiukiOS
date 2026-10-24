/* RFC 792 wire helpers. No unaligned loads. SPDX-License-Identifier: MIT */
#ifndef CIUKI_TERMINAL_ICMP_H
#define CIUKI_TERMINAL_ICMP_H
#include <stddef.h>
#include <stdint.h>
static unsigned term_icmp_u16(const uint8_t *p) { return (unsigned)p[0]*256+p[1]; }
static uint16_t term_icmp_checksum(const uint8_t *p,size_t n)
{
    uint32_t sum=0;
    while(n>1) { sum+=term_icmp_u16(p);p+=2;n-=2; }
    if(n)sum+=(unsigned)*p*256;
    while(sum>>16)sum=(sum&65535)+(sum>>16);
    return (uint16_t)~sum;
}
/* 1 echo reply, 2 intermediate TTL expiry, 3 destination unreachable.
 * Validate the quoted request so another process's ICMP cannot end a probe. */
static int term_icmp_match(const uint8_t *p,size_t n,unsigned id,unsigned seq,uint32_t target)
{
    if(n<20 || p[0]>>4!=4 || p[9]!=1)return 0;
    unsigned h=(p[0]&15)*4,total=term_icmp_u16(p+2);
    if(h<20 || total>n || total<h+8 || (term_icmp_u16(p+6)&0x3fff))return 0;
    const uint8_t *icmp=p+h;size_t bytes=total-h;
    if(term_icmp_checksum(icmp,bytes))return 0;
    if(icmp[0]==0 && !icmp[1]) {
        uint32_t source=(uint32_t)p[12]|(uint32_t)p[13]<<8|(uint32_t)p[14]<<16|(uint32_t)p[15]<<24;
        return source==target && term_icmp_u16(icmp+4)==id && term_icmp_u16(icmp+6)==seq;
    }
    int result=icmp[0]==11 && !icmp[1] ? 2 : icmp[0]==3 ? 3 : 0;
    if(!result || bytes<8+20+8)return 0;
    const uint8_t *original=icmp+8;unsigned ih=(original[0]&15)*4;
    if(original[0]>>4!=4 || ih<20 || bytes<8+ih+8 || original[9]!=1)return 0;
    uint32_t destination=(uint32_t)original[16]|(uint32_t)original[17]<<8|(uint32_t)original[18]<<16|(uint32_t)original[19]<<24;
    const uint8_t *echo=original+ih;
    return destination==target && echo[0]==8 && !echo[1] && term_icmp_u16(echo+4)==id && term_icmp_u16(echo+6)==seq ? result : 0;
}
#endif
