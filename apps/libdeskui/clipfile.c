/* SPDX-License-Identifier: MIT. Independently encoded from the CLP layout;
 * ReactOS clipbrd/fileutils.c is a research reference, no code is copied. */
#include "clipfile.h"
#include "clipboard.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static unsigned get16(const unsigned char *p) { return p[0]|((unsigned)p[1]<<8); }
static void put16(unsigned char *p, unsigned v) { p[0]=v; p[1]=v>>8; }
static int byte_format(unsigned f)
{ return (f>=1 && f<=17 && f!=2 && f!=3 && f!=9 && f!=14) || f==0x81 || (f>=0x200 && f<=0x2ff) || (f>=0xc000 && f<=0xffff); }
int du_clip_export(void **out, uint32_t *bytes)
{
    struct desk_clip_wire q; int e=du_clip_open(); if (e) return e;
    e=du_clip_query(&q);
    void *items[DESK_CLIP_FORMATS_MAX]={0}; uint32_t sizes[DESK_CLIP_FORMATS_MAX]={0},formats[DESK_CLIP_FORMATS_MAX]={0};
    char (*names)[79]=calloc(DESK_CLIP_FORMATS_MAX,79); unsigned count=0,total=0;
    if (!names) e=-ENOMEM;
    for (unsigned i=0;!e && i<q.count;i++) {
        unsigned f=q.formats[i];
        if (f==1 || f==7 || f==2) continue; /* retain text as Unicode, picture as DIB */
        if (!byte_format(f)) continue;
        if (count==DESK_CLIP_FORMATS_MAX) { e=-ENOMEM; break; }
        if (f>=0xc000) {
            struct desk_clip_wire w={.op=DESK_CLIP_NAME,.format=f},r; void *unused=NULL;
            e=du_clip_call(&w,NULL,&r,&unused); free(unused);
            if (e) break;
            if (r.count>78) { e=-EINVAL; break; }
            /* CLP 0xC350 names are ANSI. Refuse an unrepresentable name. */
            for (unsigned k=0;k<r.count;k++) if ((unsigned char)r.name[k]>=128) e=-EINVAL;
            if (e) break;
            memcpy(names[count],r.name,r.count);
        }
        e=du_clip_get(f,&items[count],&sizes[count]); if (e) break;
        if (sizes[count]>DESK_CLIP_BYTES_MAX-total) { e=-ENOMEM; break; }
        total+=sizes[count]; formats[count++]=f;
    }
    int closed=du_clip_close(); if (!e) e=closed;
    unsigned char *b=NULL;
    if (!e) {
        unsigned offset=4+89*count; b=calloc(1,offset+total); if (!b) e=-ENOMEM;
        else {
            put16(b,0xc350); put16(b+2,count);
            for (unsigned i=0;i<count;i++) {
                unsigned char *h=b+4+89*i; put16(h,formats[i]); desk_put32(h+2,sizes[i]); desk_put32(h+6,offset);
                memcpy(h+10,names[i],79); if (sizes[i]) memcpy(b+offset,items[i],sizes[i]); offset+=sizes[i];
            }
            *out=b; *bytes=offset;
        }
    }
    for (unsigned i=0;i<DESK_CLIP_FORMATS_MAX;i++) free(items[i]);
    free(names); return e;
}
int du_clip_import(const void *data, uint32_t bytes)
{
    const unsigned char *b=data;
    if (!b || bytes<4 || bytes>DESK_CLIP_BYTES_MAX+4+89*DESK_CLIP_FORMATS_MAX || get16(b)!=0xc350) return -EINVAL;
    unsigned count=get16(b+2),total=0;
    if (count>DESK_CLIP_FORMATS_MAX || bytes<4+89*count) return -EINVAL;
    unsigned formats[DESK_CLIP_FORMATS_MAX];
    for (unsigned i=0;i<count;i++) {
        const unsigned char *h=b+4+89*i; unsigned f=get16(h),n=desk_get32(h+2),offset=desk_get32(h+6);
        if (!byte_format(f) || offset<4+89*count || offset>bytes || n>bytes-offset || n>DESK_CLIP_BYTES_MAX-total) return -EINVAL;
        total+=n;
        if (f>=0xc000) {
            if (!memchr(h+10,0,79) || !h[10]) return -EINVAL;
            int registered=du_clip_register((const char *)h+10); if (registered<0) return registered; f=registered;
        }
        for (unsigned j=0;j<i;j++) {
            const unsigned char *p=b+4+89*j; unsigned start=desk_get32(p+6),size=desk_get32(p+2);
            if (formats[j]==f || (offset<start+size && start<offset+n)) return -EINVAL;
        }
        if ((f==1 || f==7) && (!n || b[offset+n-1])) return -EINVAL;
        if (f==13 && (n<2 || (n&1) || b[offset+n-1] || b[offset+n-2])) return -EINVAL;
        if (f==16 && n!=4) return -EINVAL;
        formats[i]=f;
    }
    int e=du_clip_open(); if (e) return e;
    e=du_clip_empty();
    for (unsigned i=0;!e && i<count;i++) {
        const unsigned char *h=b+4+89*i;
        e=du_clip_put(formats[i],b+desk_get32(h+6),desk_get32(h+2));
    }
    int closed=du_clip_close(); return e ? e : closed;
}
