/* Private disk format shared by the VMM and Control Panel, never a syscall.
 * Physical ports are identified by I/O base, not a boot-local lease number.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_GAME_PORTS_SETTINGS_H
#define CIUKI_GAME_PORTS_SETTINGS_H
#include <stdint.h>
#include <stddef.h>
#define GAME_PORTS_NONE 0u
#define GAME_PORTS_NULL 1u
#define GAME_LPT_NONE 0u
#define GAME_LPT_PRINTER 1u
#define GAME_LPT_RAW 2u
#define GAME_LPT_FILE 3u
#define GAME_PORTS_TEXT_MAX 384u
struct game_ports_settings {
    uint32_t version;
    uint32_t sequence;
    uint32_t com[4];
    uint32_t lpt_mode;
    uint32_t lpt_base;
    char path[128];
};
_Static_assert(sizeof(struct game_ports_settings)==160,"private game ports format");
static inline void game_ports_defaults(struct game_ports_settings *p)
{
    *p=(struct game_ports_settings){.version=1,.com={1,1,1,1},.lpt_base=0x378};
}
static inline int game_ports_valid(const struct game_ports_settings *p)
{
    if (p->version!=1 || p->lpt_mode>GAME_LPT_FILE ||
        (p->lpt_base && (p->lpt_base<0x100 || p->lpt_base>0xfffc))) return 0;
    for (unsigned i=0;i<4;i++) if (p->com[i]>1 && (p->com[i]<0x100 || p->com[i]>0xfff8)) return 0;
    size_t n=0;
    for (;n<sizeof(p->path) && p->path[n];n++)
        if ((unsigned char)p->path[n]<32 || (unsigned char)p->path[n]>126 || p->path[n]=='\\' || p->path[n]==':') return 0;
    if (n==sizeof(p->path)) return 0;
    for (size_t i=n+1;i<sizeof(p->path);i++) if (p->path[i]) return 0;
    /* Only user documents, with no empty, dot or parent components. */
    if (n) {
        static const char prefix[]="/Documents/";
        for (unsigned i=0;i<sizeof(prefix)-1;i++) if (p->path[i]!=prefix[i]) return 0;
        size_t begin=sizeof(prefix)-1;
        for (size_t i=begin;i<=n;i++) if (!p->path[i] || p->path[i]=='/') {
            size_t bytes=i-begin;
            if (!bytes || (bytes==1 && p->path[begin]=='.') ||
                (bytes==2 && p->path[begin]=='.' && p->path[begin+1]=='.')) return 0;
            begin=i+1;
        }
    }
    return p->lpt_mode==GAME_LPT_FILE ? n!=0 : p->lpt_mode==GAME_LPT_NONE || p->lpt_base!=0;
}
static inline uint32_t game_ports_hash(const char *s,size_t n)
{
    uint32_t h=2166136261u;
    for (size_t i=0;i<n;i++) h=(h^(unsigned char)s[i])*16777619u;
    return h;
}
static inline int game_ports_digit(char c)
{ return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1; }
static inline size_t game_ports_encode(const struct game_ports_settings *p,char out[GAME_PORTS_TEXT_MAX])
{
    static const char prefix[]="[GamePorts]\nversion=1\nrecord=";
    static const char hex[]="0123456789abcdef";
    size_t n=0;
    for (unsigned i=0;i<sizeof(prefix)-1;i++) out[n++]=prefix[i];
    const uint32_t words[8]={p->version,p->sequence,p->com[0],p->com[1],p->com[2],p->com[3],p->lpt_mode,p->lpt_base};
    /* Explicit little-endian words, followed by the zero-padded path. */
    for (unsigned i=0;i<sizeof(*p);i++) {
        uint8_t b=i<32 ? (uint8_t)(words[i/4]>>((i%4)*8)) : (uint8_t)p->path[i-32];
        out[n++]=hex[b>>4];out[n++]=hex[b&15];
    }
    out[n++]='\n'; uint32_t hash=game_ports_hash(out,n);
    static const char check[]="checksum=";
    for (unsigned i=0;i<sizeof(check)-1;i++) out[n++]=check[i];
    for (unsigned i=0;i<8;i++) out[n++]=hex[(hash>>(28-i*4))&15];
    out[n++]='\n'; return n;
}
static inline int game_ports_decode(const char *text,size_t n,struct game_ports_settings *p)
{
    static const char prefix[]="[GamePorts]\nversion=1\nrecord=";
    size_t body=sizeof(prefix)-1+sizeof(*p)*2+1;
    if (n!=body+18 || text[body-1]!='\n' || text[n-1]!='\n') return 0;
    for (unsigned i=0;i<sizeof(prefix)-1;i++) if (text[i]!=prefix[i]) return 0;
    static const char check[]="checksum=";
    for (unsigned i=0;i<sizeof(check)-1;i++) if (text[body+i]!=check[i]) return 0;
    uint32_t hash=0;
    for (unsigned i=0;i<8;i++) { int d=game_ports_digit(text[body+9+i]); if (d<0) return 0; hash=(hash<<4)|(unsigned)d; }
    if (hash!=game_ports_hash(text,body)) return 0;
    *p=(struct game_ports_settings){0};
    uint32_t words[8]={0};
    for (unsigned i=0;i<sizeof(*p);i++) {
        int a=game_ports_digit(text[sizeof(prefix)-1+i*2]),b=game_ports_digit(text[sizeof(prefix)+i*2]);
        if (a<0 || b<0) return 0;
        uint8_t value=(uint8_t)(a*16+b);
        if (i<32) words[i/4]|=(uint32_t)value<<((i%4)*8);
        else p->path[i-32]=(char)value;
    }
    p->version=words[0];p->sequence=words[1];
    for (unsigned i=0;i<4;i++) p->com[i]=words[i+2];
    p->lpt_mode=words[6];p->lpt_base=words[7];
    if (!p->sequence || !game_ports_valid(p)) return 0;
    /* Reject noncanonical trailing path bytes as well as malformed strings. */
    int end=0;
    for (unsigned i=0;i<sizeof(p->path);i++) { if (end && p->path[i]) return 0; end|=!p->path[i]; }
    return 1;
}
#endif
