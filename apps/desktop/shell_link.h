/* SPDX-License-Identifier: MIT
 * CiukiOS .lnk v3: magic[8], show, signed icon index, five u32 lengths,
 * u32 hotkey (upper 16 bits zero); counted UTF-8 label,target,args,cwd,icon. No NULs.
 * This is the f5-15 IShellLink writer hand-off, not MS-SHLLINK decoding. */
#ifndef CIUKI_SHELL_LINK_H
#define CIUKI_SHELL_LINK_H
#include "protocol.h"
#include <string.h>
/* Self-contained codec: model-only users do not need the desktop wire parser. */
static inline uint32_t desk_link_get32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static inline void desk_link_put32(uint8_t *p,uint32_t v)
{ for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static inline int desk_link_utf8(const uint8_t *p,unsigned n)
{
    for (unsigned i=0;i<n;) {
        uint32_t c=p[i++],min=0; unsigned extra=0;
        if (c<128) { if (c<32 || c==127) return 0; continue; }
        if (c>=0xc2 && c<0xe0) { c&=31; extra=1; min=128; }
        else if (c>=0xe0 && c<0xf0) { c&=15; extra=2; min=2048; }
        else if (c>=0xf0 && c<=0xf4) { c&=7; extra=3; min=65536; }
        else return 0;
        if (extra>n-i) return 0;
        while(extra--) { unsigned b=p[i++]; if ((b&192)!=128) return 0; c=(c<<6)|(b&63); }
        if(c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff) || (c>=0x80 && c<0xa0)) return 0;
    }
    return 1;
}
#define DESK_LINK_BYTES 4096u
#define DESK_LINKS 64u
#define DESK_UNINSTALL_MAX 32u
enum desk_program_kind { DESK_PROGRAM_WIN32, DESK_PROGRAM_GAME, DESK_PROGRAM_DOS, DESK_PROGRAM_NATIVE };
struct desk_launch_record {
    char label[64], target[260], arguments[1024], directory[260], icon[260];
    uint32_t show;
    int32_t icon_index;
    uint32_t hotkey;
};
struct desk_link_entry {
    struct desk_launch_record launch;
    char source[260]; /* re-read at activation; never launch stale cached bytes */
    unsigned desktop;
    uint64_t source_size;
    int64_t source_modified;
};
struct desk_uninstall_entry {
    char key[260], name[64], command[1284], version[64], publisher[128], location[260];
    uint32_t estimated_kb, no_remove, no_modify, system_component;
    unsigned kind, game, inventory_more;
    uint64_t disk_bytes;
    char licence[260];
    /* fx-exeicons: the icon Add/Remove Programs shows, a libdeskui file
     * icon key (POSIX path, "|index" for an explicit resource); empty: the
     * kind's default. */
    char icon[300];
};
static inline int desk_target_url(const char *s)
{
    if (!s || !((*s>='A' && *s<='Z') || (*s>='a' && *s<='z'))) return 0;
    for (unsigned i=1;s[i] && i<32;i++) {
        unsigned c=(unsigned char)s[i];
        if (c==':') return i>1 && s[i+1]; /* a drive letter is a path */
        if (!((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='+' || c=='-' || c=='.')) return 0;
    }
    return 0;
}
static inline int desk_link_hotkey_valid(uint32_t hotkey)
{
    return !hotkey || (!(hotkey&0xfffff000u) && (hotkey&255u));
}
static inline int desk_launch_valid(const struct desk_launch_record *r)
{
    const char *fields[]={r->label,r->target,r->arguments,r->directory,r->icon};
    const unsigned caps[]={sizeof(r->label),sizeof(r->target),sizeof(r->arguments),sizeof(r->directory),sizeof(r->icon)};
    for (unsigned k=0;k<5;k++) {
        unsigned n=0; while (n<caps[k] && fields[k][n]) n++;
        if (n==caps[k] || !desk_link_utf8((const uint8_t *)fields[k],n) || (k<2 && !n)) return 0;
    }
    if (!desk_target_url(r->target) && r->target[0]!='/' && !((r->target[0]>='A' && r->target[0]<='Z') || (r->target[0]>='a' && r->target[0]<='z'))) return 0;
    if (!desk_target_url(r->target) && r->target[0]!='/' && (r->target[1]!=':' || (r->target[2]!='\\' && r->target[2]!='/'))) return 0;
    return desk_link_hotkey_valid(r->hotkey) && (r->show==1 || r->show==3 || r->show==7);
}
static inline int desk_link_decode(const uint8_t *bytes, unsigned count, struct desk_launch_record *out)
{
    if (!bytes || !out || count<40 || count>DESK_LINK_BYTES || memcmp(bytes,"CIUKLNK3",8)) return 0;
    memset(out,0,sizeof(*out)); out->show=desk_link_get32(bytes+8); out->icon_index=(int32_t)desk_link_get32(bytes+12);
    out->hotkey=desk_link_get32(bytes+36);
    char *fields[]={out->label,out->target,out->arguments,out->directory,out->icon};
    const unsigned caps[]={sizeof(out->label),sizeof(out->target),sizeof(out->arguments),sizeof(out->directory),sizeof(out->icon)};
    unsigned offset=40;
    for (unsigned k=0;k<5;k++) {
        unsigned n=desk_link_get32(bytes+16+k*4);
        if (n>=caps[k] || n>count-offset || !desk_link_utf8(bytes+offset,n)) return 0;
        memcpy(fields[k],bytes+offset,n); offset+=n;
    }
    return offset==count && desk_launch_valid(out);
}
static inline unsigned desk_link_encode(const struct desk_launch_record *r, uint8_t *out, unsigned cap)
{
    if (!r || !out || cap<40 || !desk_launch_valid(r)) return 0;
    const char *fields[]={r->label,r->target,r->arguments,r->directory,r->icon};
    unsigned total=40;
    for (unsigned k=0;k<5;k++) total+=(unsigned)strlen(fields[k]);
    if (total>cap || total>DESK_LINK_BYTES) return 0;
    memset(out,0,40); memcpy(out,"CIUKLNK3",8); desk_link_put32(out+8,r->show); desk_link_put32(out+12,(uint32_t)r->icon_index);
    desk_link_put32(out+36,r->hotkey);
    unsigned offset=40;
    for (unsigned k=0;k<5;k++) { unsigned n=(unsigned)strlen(fields[k]); desk_link_put32(out+16+k*4,n); memcpy(out+offset,fields[k],n); offset+=n; }
    return total;
}
/* Preserve the quoted Windows argument tail exactly; never use /bin/sh.
 * Unquoted executables with spaces are rejected instead of trying ambiguous
 * progressively longer paths (which would permit executable substitution). */
static inline int desk_uninstall_command(const char *command, struct desk_launch_record *out)
{
    if (!command || !out) return 0;
    memset(out,0,sizeof(*out)); strcpy(out->label,"Uninstall"); out->show=1;
    const char *p=command; while (*p==' ') p++;
    int quoted=*p=='"'; if (quoted) p++;
    unsigned n=0;
    while (*p && (quoted ? *p!='"' : *p!=' ')) {
        if (n+1>=sizeof(out->target)) return 0;
        out->target[n++]=*p++;
    }
    if (n<4) return 0;
    const char *ext=out->target+n-4;
    if (ext[0]!='.' || !((ext[1]=='e' || ext[1]=='E') && (ext[2]=='x' || ext[2]=='X') && (ext[3]=='e' || ext[3]=='E')))
        return 0;
    if (quoted) { if (*p++!='"') return 0; if (*p && *p!=' ') return 0; }
    while (*p==' ') p++;
    unsigned len=0; while (len<sizeof(out->arguments) && p[len]) len++;
    if (len==sizeof(out->arguments)) return 0;
    memcpy(out->arguments,p,len+1);
    return desk_launch_valid(out);
}
#endif
