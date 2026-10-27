/* Stirling installer archive and PKWARE DCL decoder.
 * Huffman tables/algorithm adapted from Mark Adler's blast 1.3 (zlib 1.3.1).
 * Altered implementation: bounded in-memory output, explicit errors, per-call
 * tables, no setjmp or global state. See blast-LICENSE.txt for original terms.
 * Remaining code SPDX-License-Identifier: LGPL-2.1-or-later */
#include "private.h"
static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p+2) << 16; }
static int span(unsigned at, unsigned count, unsigned size) { return at<=size && count<=size-at; }
static int name(const uint8_t *p, unsigned size, unsigned *at, char *out) {
    if (!span(*at,2,size)) return 0;
    unsigned n=u16(p+*at); *at+=2;
    if (!n || n>=IS5_NAME || !span(*at,n,size)) return 0;
    for (unsigned i=0;i<n;i++) {
        unsigned c=p[*at+i];
        if (c<33 || c>126 || c=='/' || c=='\\' || c==':' || c=='"') return 0;
        out[i]=(char)c;
    }
    out[n]=0; *at+=n;
    return !(out[0]=='.');
}
int is5_directory(const uint8_t *p, uint32_t size, struct is5_archive *a) {
    static const uint8_t signature[]={42,171,121,216,0,1,0,0};
    if (!p || !a || size<82) return 0;
    for (unsigned i=0;i<8;i++) if(p[i]!=signature[i])return 0;
    a->count=u32(p+78);
    if (!a->count || a->count>IS5_MEMBERS) return 0;
    unsigned at=82;
    for (unsigned i=0;i<a->count;i++) {
        struct is5_member *m=a->member+i;
        if (!span(at,20,size)) return 0;
        m->offset=u32(p+at);m->packed=u32(p+at+4);m->expanded=u32(p+at+8);at+=20;
        if (!m->packed || !m->expanded || m->expanded>8u*1024*1024 ||
            !span(m->offset,m->packed,size) || !name(p,size,&at,m->original) ||
            !name(p,size,&at,m->runtime)) return 0;
        for (unsigned j=0;j<i;j++) {
            struct is5_member *b=a->member+j;
            if (m->offset<b->offset+b->packed && b->offset<m->offset+m->packed) return 0;
        }
    }
    for (unsigned i=0;i<a->count;i++) if(a->member[i].offset<at)return 0;
    return 1;
}
struct bits { const uint8_t *p; unsigned size, at, value, count, bad; };
static unsigned take(struct bits *b, unsigned n) {
    while (b->count<n) {
        if(b->at==b->size){b->bad=1;return 0;}
        b->value|=(unsigned)b->p[b->at++]<<b->count;b->count+=8;
    }
    unsigned v=b->value&((1u<<n)-1);b->value>>=n;b->count-=n;return v;
}
struct table { unsigned count[14], symbol[256]; };
static void table(struct table *t,const uint8_t *rep,unsigned n) {
    unsigned lens[256],used=0,off[14]={0};
    for(unsigned i=0;i<14;i++)t->count[i]=0;
    for(unsigned i=0;i<n;i++)for(unsigned j=0;j<1u+(rep[i]>>4);j++)lens[used++]=rep[i]&15;
    for(unsigned i=0;i<used;i++)t->count[lens[i]]++;
    for(unsigned i=1;i<13;i++)off[i+1]=off[i]+t->count[i];
    for(unsigned i=0;i<used;i++)t->symbol[off[lens[i]]++]=i;
}
static int symbol(struct bits *b,const struct table *t) {
    unsigned code=0,first=0,index=0;
    for(unsigned len=1;len<=13;len++) {
        code|=take(b,1)^1;
        if(b->bad)return -1;
        if(code<first+t->count[len])return (int)t->symbol[index+code-first];
        index+=t->count[len];first=(first+t->count[len])<<1;code<<=1;
    }
    return -1;
}
int is5_dcl(const uint8_t *p,uint32_t size,uint8_t *out,uint32_t capacity) {
    static const uint8_t literal[]={11,124,8,7,28,7,188,13,76,4,10,8,12,10,12,10,8,23,8,
        9,7,6,7,8,7,6,55,8,23,24,12,11,7,9,11,12,6,7,22,5,7,24,6,11,9,6,7,22,7,11,
        38,7,9,8,25,11,8,11,9,12,8,12,5,38,5,38,5,11,7,5,6,21,6,10,53,8,7,24,10,27,
        44,253,253,253,252,252,252,13,12,45,12,45,12,61,12,45,44,173};
    static const uint8_t length[]={2,35,36,53,38,23},distance[]={2,20,53,230,247,151,248};
    static const unsigned base[]={3,2,4,5,6,7,8,9,10,12,16,24,40,72,136,264};
    static const unsigned extra[]={0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8};
    if(!p || !out || size<2 || p[0]>1 || p[1]<4 || p[1]>6)return 0;
    struct bits b={p,size,2,0,0,0};struct table lit,len,dist;
    table(&lit,literal,sizeof(literal));table(&len,length,sizeof(length));table(&dist,distance,sizeof(distance));
    unsigned at=0;
    for(;;) {
        if(take(&b,1)) {
            int s=symbol(&b,&len);if(s<0)return 0;
            unsigned n=base[s]+take(&b,extra[s]);
            if(b.bad)return 0;
            if(n==519)return at==capacity && b.at==size;
            s=symbol(&b,&dist);if(s<0)return 0;
            unsigned bits=n==2?2:p[1],d=((unsigned)s<<bits)+take(&b,bits)+1;
            if(b.bad || d>at || n>capacity-at)return 0;
            while(n--){out[at]=out[at-d];at++;}
        } else {
            int c=p[0]?symbol(&b,&lit):(int)take(&b,8);
            if(b.bad || c<0 || at==capacity)return 0;
            out[at++]=(uint8_t)c;
        }
    }
}
int is5_ne(const uint8_t *p,uint32_t size) {
    if(!p || size<64 || u16(p)!=0x5a4d)return 0;
    unsigned h=u32(p+60);
    return h>=64 && span(h,64,size) && u16(p+h)==0x454e &&
        p[h+54]==2 && !(u16(p+h+12)&0x8000);
}
int is5_string(const uint8_t *p,uint32_t size,unsigned id,char *out,unsigned cap) {
    /* The launcher's localized strings live in _SETUP.DLL on the reference
     * CD. Reading a DLL's resources does not admit it as an executable. */
    if(!p || size<64 || u16(p)!=0x5a4d)return 0;
    unsigned h=u32(p+60);
    if(h<64 || !span(h,64,size) || u16(p+h)!=0x454e || p[h+54]!=2)return 0;
    unsigned at=h+u16(p+h+36);
    if(!span(at,2,size))return 0;
    unsigned shift=u16(p+at);at+=2;if(shift>16)return 0;
    while(span(at,8,size) && u16(p+at)) {
        unsigned type=u16(p+at),n=u16(p+at+2);at+=8;
        if(n>65535/12 || !span(at,n*12,size))return 0;
        for(unsigned i=0;i<n;i++) {
            const uint8_t *r=p+at+i*12;
            if(type!=0x8006 || u16(r+6)!=(0x8000u+id/16+1))continue;
            unsigned a=u16(r)<<shift,bytes=u16(r+2)<<shift;
            if(!span(a,bytes,size))return 0;
            unsigned end=a+bytes;
            for(unsigned j=0;j<=id%16;j++) {
                if(a==end)return 0;
                unsigned len=p[a++];if(!span(a,len,end))return 0;
                if(j==id%16){if(!len || len>=cap)return 0;for(unsigned k=0;k<len;k++)out[k]=(char)p[a+k];out[len]=0;return 1;}
                a+=len;
            }
        }
        at+=n*12;
    }
    return 0;
}
