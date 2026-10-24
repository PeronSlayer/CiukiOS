/* Bounded LMHOSTS and DNS/NBNS wire helpers. SPDX-License-Identifier: MIT */
#ifndef CIUKI_NAME_WIRE_H
#define CIUKI_NAME_WIRE_H
#include <stdint.h>
#include <stddef.h>
static inline unsigned cu_name_lower(unsigned c) { return c>='A' && c<='Z'?c+32:c; }
static inline int cu_name_equal(const char *a,const char *b)
{ while(*a && *b)if(cu_name_lower((unsigned char)*a++)!=cu_name_lower((unsigned char)*b++))return 0;return *a==*b; }
static inline int cu_name_space(unsigned c) { return c==' ' || c=='\t' || c=='\r'; }
static inline unsigned cu_name_u16(const unsigned char *p) { return (unsigned)p[0]*256+p[1]; }
static inline void cu_name_put16(unsigned char *p,unsigned n) { p[0]=(unsigned char)(n>>8);p[1]=(unsigned char)n; }
static inline int cu_name_ip(const char **cursor,uint32_t *address)
{
    const char *p=*cursor;uint32_t ip=0;
    for(unsigned i=0;i<4;i++) {
        unsigned value=0,digits=0;
        while(*p>='0' && *p<='9') { if(++digits>3)return 0;value=value*10+(unsigned)(*p++-'0'); }
        if(!digits || value>255 || (i<3 && *p++!='.'))return 0;
        ip|=(uint32_t)value<<(8*i);
    }
    if(!cu_name_space((unsigned char)*p))return 0;
    *cursor=p;*address=ip;return 1;
}
static inline int cu_hosts_reverse(int (*next)(void *),void *ctx,uint32_t address,char *name,size_t cap)
{
    char line[1024];unsigned used=0;int overlong=0;
    for(unsigned total=0;total<=65536;total++) {
        int c=total==65536?-1:next(ctx);
        if(c<0 || c=='\n') {
            if(!overlong && total<65536) {
                line[used]=0;const char *p=line;while(cu_name_space((unsigned char)*p))p++;
                uint32_t ip;
                if(cu_name_ip(&p,&ip) && ip==address) {
                    while(cu_name_space((unsigned char)*p))p++;
                    size_t n=0;while(p[n] && p[n]!='#' && !cu_name_space((unsigned char)p[n]))n++;
                    if(n && n<cap) { for(size_t i=0;i<n;i++)name[i]=p[i];name[n]=0;return 1; }
                }
            }
            if(c<0)return 0;used=0;overlong=0;
        } else if(used+1<sizeof(line))line[used++]=(char)c;else overlong=1;
    }
    return 0;
}
static inline int cu_name_hex(unsigned c)
{ if(c>='0' && c<='9')return (int)c-'0';c=cu_name_lower(c);return c>='a' && c<='f'?(int)c-'a'+10:-1; }
static inline int cu_lmhosts_line(const char *line,const char *name,unsigned suffix,uint32_t *address)
{
    uint32_t ip;const char *p=line;while(cu_name_space((unsigned char)*p))p++;
    if(!cu_name_ip(&p,&ip))return 0;
    while(cu_name_space((unsigned char)*p))p++;
    unsigned char decoded[16];unsigned count=0;int quote=*p=='"';if(quote)p++;
    while(*p && (quote?*p!='"':!cu_name_space((unsigned char)*p) && *p!='#')) {
        unsigned value=(unsigned char)*p++;
        if(quote && value=='\\' && p[0]=='0' && p[1]=='x') {
            if(!p[2] || !p[3])return 0;
            int a=cu_name_hex((unsigned char)p[2]),b=cu_name_hex((unsigned char)p[3]);if(a<0 || b<0)return 0;
            value=(unsigned)(a*16+b);p+=4;
        }
        if(count==16)return 0;decoded[count++]=(unsigned char)value;
    }
    if(quote && *p++!='"')return 0;
    if(*p && !cu_name_space((unsigned char)*p) && *p!='#')return 0;
    if(!count || (count==16 && decoded[15]!=suffix))return 0;
    unsigned base=count==16?15:count;while(base && decoded[base-1]==' ')base--;
    unsigned n=0;while(name[n]) { if(n>=base || cu_name_lower((unsigned char)name[n])!=cu_name_lower(decoded[n]))return 0;n++; }
    if(n!=base)return 0;*address=ip;return 1;
}
static inline int cu_lmhosts_lookup(int (*next)(void *),void *ctx,const char *name,unsigned suffix,uint32_t *address)
{
    char line[1024];unsigned at=0;int longline=0;
    for(unsigned total=0;total<=65536;total++) {
        int c=total==65536?-1:next(ctx);
        if(c<0 || c=='\n') {
            if(!longline && total<65536) { line[at]=0;if(cu_lmhosts_line(line,name,suffix,address))return 1; }
            if(c<0)return 0;at=0;longline=0;
        } else if(at+1<sizeof(line))line[at++]=(char)c;else longline=1;
    }
    return 0;
}
static inline int cu_dns_read_name(const unsigned char *p,size_t n,size_t *cursor,char *out,size_t cap)
{
    size_t at=*cursor,used=0,end=0;unsigned hops=0;
    for(;;) {
        if(at>=n || ++hops>128)return 0;unsigned c=p[at++];
        if((c&192)==192) {
            if(at>=n)return 0;if(!end)end=at+1;
            at=((size_t)(c&63)<<8)|p[at];continue;
        }
        if(c&192 || c>63 || c>n-at)return 0;
        if(!c) { if(used>=cap)return 0;out[used]=0;*cursor=end?end:at;return 1; }
        if(used) { if(used+1>=cap)return 0;out[used++]='.'; }
        if(c>=cap-used)return 0;
        while(c--) { unsigned ch=p[at++];if(ch<=32 || ch==127 || ch=='.')return 0;out[used++]=(char)ch; }
    }
}
static inline size_t cu_nbns_query(unsigned char p[50],uint16_t id,const char *name,unsigned suffix)
{
    unsigned n=0;while(name[n]) { if(n==15 || (unsigned char)name[n]<=32 || name[n]=='.')return 0;n++; }
    if(!n || suffix>255)return 0;
    for(unsigned i=0;i<50;i++)p[i]=0;
    cu_name_put16(p,id);cu_name_put16(p+2,0x0110);p[5]=1;p[12]=32;
    for(unsigned i=0;i<16;i++) {
        unsigned c=i==15?suffix:i<n?(unsigned char)name[i]:' ';
        if(i<15 && c>='a' && c<='z')c-=32;
        p[13+2*i]=(unsigned char)('A'+(c>>4));p[14+2*i]=(unsigned char)('A'+(c&15));
    }
    p[47]=32;p[49]=1;return 50;
}
static inline size_t cu_ptr_query(unsigned char p[90],uint16_t id,uint32_t address)
{
    for(unsigned i=0;i<90;i++)p[i]=0;
    cu_name_put16(p,id);p[2]=1;p[5]=1;size_t at=12;
    for(unsigned i=4;i;i--) {
        unsigned c=(address>>((i-1)*8))&255,offset=(unsigned)at++;unsigned digits=c>=100?3:c>=10?2:1;p[offset]=(unsigned char)digits;
        if(c>=100)p[at++]=(unsigned char)('0'+c/100);
        if(c>=10)p[at++]=(unsigned char)('0'+c/10%10);
        p[at++]=(unsigned char)('0'+c%10);
    }
    p[at++]=7;const char *arpa="in-addr";for(unsigned i=0;i<7;i++)p[at++]=(unsigned char)arpa[i];
    p[at++]=4;arpa="arpa";for(unsigned i=0;i<4;i++)p[at++]=(unsigned char)arpa[i];
    p[at++]=0;p[at++]=0;p[at++]=12;p[at++]=0;p[at++]=1;return at;
}
/* Verify question/owner, reject compression cycles, bound every RDATA access.
 * NBNS commonly omits its question section in positive responses. */
static inline int cu_name_answer(const unsigned char *p,size_t n,const unsigned char *query,size_t qn,
                                  uint32_t *address,char *hostname,size_t cap)
{
    if(n<12 || qn<17 || p[0]!=query[0] || p[1]!=query[1] ||
       !(p[2]&128) || (p[2]&0x7a) || (p[3]&15))return 0;
    unsigned type=cu_name_u16(query+qn-4),qd=cu_name_u16(p+4),answers=cu_name_u16(p+6);
    if(qd>1 || !answers || answers>64 || (!qd && type!=32))return 0;
    char wanted[254],name[254];size_t q=12,at=12;
    if(!cu_dns_read_name(query,qn,&q,wanted,sizeof(wanted)))return 0;
    if(qd) {
        if(!cu_dns_read_name(p,n,&at,name,sizeof(name)) || !cu_name_equal(name,wanted) ||
            at+4>n || cu_name_u16(p+at)!=type || cu_name_u16(p+at+2)!=1)return 0;
        at+=4;
    }
    for(unsigned i=0;i<answers;i++) {
        if(!cu_dns_read_name(p,n,&at,name,sizeof(name)) || at+10>n)return 0;
        unsigned rr=cu_name_u16(p+at),cls=cu_name_u16(p+at+2),bytes=cu_name_u16(p+at+8);at+=10;
        if(bytes>n-at)return 0;
        if(cls==1 && cu_name_equal(name,wanted)) {
            if(rr==32 && type==32 && bytes>=6 && bytes%6==0) {
                *address=(uint32_t)p[at+2]|((uint32_t)p[at+3]<<8)|((uint32_t)p[at+4]<<16)|((uint32_t)p[at+5]<<24);return 1;
            }
            if(rr==12 && type==12) { size_t r=at;if(!cu_dns_read_name(p,n,&r,hostname,cap) || r!=at+bytes)return 0;return 1; }
            if(rr==5 && type==12) { size_t r=at;if(!cu_dns_read_name(p,n,&r,wanted,sizeof(wanted)) || r!=at+bytes)return 0; }
        }
        at+=bytes;
    }
    return 0;
}
#endif
