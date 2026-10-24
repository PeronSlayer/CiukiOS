/* SPDX-License-Identifier: MIT. CF_HDROP + Preferred DropEffect. */
#include "model.h"
#include "../libdeskui/clipboard.h"
#include "../libdeskui/clipstore.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static int windows_path(const char *in, char *out)
{
    if (!in || in[0]!='/' || strlen(in)>=FM_PATH-4 || strchr(in,'\r') || strchr(in,'\n')) return -EINVAL;
    char drive='C';
    if (!strncmp(in,"/mnt/",5) && in[5] && (!in[6] || in[6]=='/')) {
        drive=in[5]; if (drive>='a' && drive<='z') drive-=32;
        if (drive<'A' || drive>'Z' || drive=='C') return -EINVAL;
        in+=6;
    }
    out[0]=drive; out[1]=':'; out[2]='\\'; unsigned n=3;
    if (*in=='/') in++;
    for (;*in;in++) out[n++]=*in=='/' ? '\\' : *in;
    out[n]=0; return 0;
}
static int native_path(char *s, char *out)
{
    if (!s[0] || s[1]!=':' || (s[2]!='\\' && s[2]!='/')) return -EINVAL;
    char drive=s[0]; if (drive>='a' && drive<='z') drive-=32;
    if (drive<'A' || drive>'Z') return -EINVAL;
    unsigned n=0; if (drive!='C') { memcpy(out,"/mnt/",5); out[5]=drive+32; n=6; }
    for (s+=2;*s;s++) { if (n+1>=FM_PATH || *s=='\r' || *s=='\n') return -EINVAL; out[n++]=*s=='\\' ? '/' : *s; }
    out[n]=0; return 0;
}
int fm_system_clipboard_save(struct fm_model *m)
{
    unsigned char *data=calloc(1,22); if (!data) return -ENOMEM;
    unsigned n=20; int e=0;
    for (unsigned i=0;i<m->clipboard_count;i++) {
        char path[FM_PATH]; void *wide=NULL; uint32_t bytes;
        e=windows_path(m->clipboard[i],path); if (e) break;
        e=clip_text_encode(path,strlen(path),&wide,&bytes); if (e) break;
        if (bytes>DESK_CLIP_ITEM_MAX-n-2) { free(wide); e=-ENOMEM; break; }
        void *next=realloc(data,n+bytes+2); if (!next) { free(wide); e=-ENOMEM; break; }
        data=next; memcpy(data+n,wide,bytes); n+=bytes; free(wide);
    }
    int format=0;
    if (!e) { format=du_clip_register(DESK_CLIP_PREFERRED_DROPEFFECT); if (format<0) e=format; }
    if (!e) e=du_clip_open();
    if (!e) {
        e=du_clip_empty();
        if (!e && m->clipboard_count) {
            desk_put32(data,20); desk_put32(data+16,1); data[n]=data[n+1]=0;
            e=du_clip_put(DESK_CF_HDROP,data,n+2);
            unsigned char effect[4]; desk_put32(effect,m->cut ? 2 : 5);
            if (!e) e=du_clip_put(format,effect,4);
        }
        struct desk_clip_wire close_request={.op=DESK_CLIP_CLOSE},reply; void *unused=NULL;
        int closed=du_clip_call(&close_request,NULL,&reply,&unused); free(unused);
        if (!e) e=closed;
        if (!e) m->clipboard_sequence=reply.sequence;
    }
    free(data); return e;
}
int fm_system_clipboard_load(struct fm_model *m)
{
    struct desk_clip_wire query; int e=du_clip_query(&query); if (e) return e;
    int present=0; for (unsigned i=0;i<query.count;i++) if (query.formats[i]==DESK_CF_HDROP) present=1;
    if (!present) { fm_clipboard_clear(m); m->clipboard_sequence=query.sequence; return 0; }
    int effect=du_clip_register(DESK_CLIP_PREFERRED_DROPEFFECT); if (effect<0) return effect;
    e=du_clip_open(); if (e) return e;
    struct fm_model *next=calloc(1,sizeof(*next));
    void *data=NULL,*move=NULL; uint32_t bytes=0,moves=0;
    if (!next) e=-ENOMEM;
    if (!e) e=du_clip_get(DESK_CF_HDROP,&data,&bytes);
    if (!e) {
        const unsigned char *b=data; unsigned offset=bytes>=20 ? desk_get32(b) : 0;
        unsigned step=bytes>=20 && desk_get32(b+16) ? 2 : 1;
        if (offset<20 || offset>bytes || bytes-offset<2*step || (step==2 && ((offset|bytes)&1))) e=-EINVAL;
        unsigned pos=offset; int terminated=0;
        while (!e && pos+step<=bytes) {
            if (!b[pos] && (step==1 || !b[pos+1])) { terminated=1; break; }
            unsigned start=pos;
            while (pos+step<=bytes && (b[pos] || (step==2 && b[pos+1]))) pos+=step;
            if (pos+step>bytes || next->clipboard_count==FM_ENTRIES) { e=-EINVAL; break; }
            char *name=NULL;
            if (step==2) e=clip_text_decode(b+start,pos-start+2,&name);
            else {
                /* ANSI HDROP uses Windows-1252. Convert through the same core. */
                struct clip_store *store=malloc(sizeof(*store)); if (!store) { e=-ENOMEM; break; } clip_init(store); struct desk_clip_wire w={.op=DESK_CLIP_OPEN},r; void *out=NULL;
                clip_request(store,1,&w,NULL,&r,&out); w.op=DESK_CLIP_EMPTY; clip_request(store,1,&w,NULL,&r,&out);
                w=(struct desk_clip_wire){.op=DESK_CLIP_PUT,.format=1,.bytes=pos-start+1}; e=clip_request(store,1,&w,b+start,&r,&out);
                if (!e) { w=(struct desk_clip_wire){.op=DESK_CLIP_GET,.format=13}; e=clip_request(store,1,&w,NULL,&r,&out); if (!e) e=clip_text_decode(out,r.bytes,&name); }
                free(out); clip_destroy(store); free(store);
            }
            char path[FM_PATH]; if (!e) e=native_path(name,path); free(name);
            if (e) break;
            char **paths=realloc(next->clipboard,(next->clipboard_count+1)*sizeof(*paths)); if (!paths) { e=-ENOMEM; break; }
            next->clipboard=paths; paths[next->clipboard_count]=malloc(strlen(path)+1); if (!paths[next->clipboard_count]) { e=-ENOMEM; break; }
            strcpy(paths[next->clipboard_count++],path); pos+=step;
        }
        if (!e && (!terminated || !next->clipboard_count)) e=-EINVAL;
    }
    if (!e && !du_clip_get(effect,&move,&moves) && moves==4) next->cut=!!(desk_get32(move)&2);
    struct desk_clip_wire r; if (!e && !(e=du_clip_query(&r))) next->clipboard_sequence=r.sequence;
    int closed=du_clip_close(); if (!e) e=closed;
    if (!e) { fm_clipboard_clear(m); m->clipboard=next->clipboard; m->clipboard_count=next->clipboard_count; m->cut=next->cut; m->clipboard_sequence=next->clipboard_sequence; next->clipboard=NULL; next->clipboard_count=0; }
    if (next) { fm_clipboard_clear(next); free(next); }
    free(data); free(move); return e;
}
int fm_system_clipboard_consume(struct fm_model *m)
{
    int e=du_clip_open(); if (e) return e;
    struct desk_clip_wire query; e=du_clip_query(&query);
    if (!e && m->cut && query.sequence==m->clipboard_sequence) {
        struct desk_clip_wire request={.op=DESK_CLIP_EMPTY,.token=m->clipboard_sequence},reply; void *unused=NULL;
        e=du_clip_call(&request,NULL,&reply,&unused); free(unused);
        if (e==-EAGAIN) e=0; /* a delayed owner rendered after the query */
    }
    int closed=du_clip_close(); if (!e) e=closed;
    fm_clipboard_clear(m); return e;
}
