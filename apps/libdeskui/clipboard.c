/* SPDX-License-Identifier: MIT */
#include "clipboard.h"
#include "clipstore.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#define DU_PROCESS_LOCAL _Thread_local
#else
#define DU_PROCESS_LOCAL
#endif
static DU_PROCESS_LOCAL du_clip_transport transport;
static DU_PROCESS_LOCAL void *context;
static DU_PROCESS_LOCAL uint32_t serial,observed;
void du_clip_observe(uint32_t sequence) { observed=sequence; }
uint32_t du_clip_observed(void) { return observed; }
static DU_PROCESS_LOCAL char *text_cache;
static DU_PROCESS_LOCAL uint32_t text_sequence;
static DU_PROCESS_LOCAL int text_valid,watch_attempted,watch_active,query_valid;
static DU_PROCESS_LOCAL struct desk_clip_wire query_cache;
void du_clip_bind(du_clip_transport fn, void *ctx) { transport=fn; context=ctx; observed=0; text_valid=watch_attempted=watch_active=query_valid=0; free(text_cache); text_cache=NULL; }
int du_clip_call(struct desk_clip_wire *w, const void *data, struct desk_clip_wire *r, void **out)
{
    if (!transport) return -ENOSYS;
    uint32_t next=__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED);
    if (!next) next=__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED);
    w->serial=next; *out=NULL;
    int error=transport(context,w,data,r,out);
    if (!error) {
        du_clip_observe(r->sequence);
        if (w->op==DESK_CLIP_WATCH) { watch_attempted=1; watch_active=w->flags && r->bytes; }
    }
    return error;
}
static int simple(unsigned op)
{
    struct desk_clip_wire w={.op=op},r; void *out=NULL;
    int e=du_clip_call(&w,NULL,&r,&out); free(out); return e;
}
int du_clip_open(void) { return simple(DESK_CLIP_OPEN); }
int du_clip_close(void) { return simple(DESK_CLIP_CLOSE); }
int du_clip_empty(void) { return simple(DESK_CLIP_EMPTY); }
int du_clip_put(unsigned f, const void *data, uint32_t n)
{
    struct desk_clip_wire w={.op=DESK_CLIP_PUT,.format=f,.bytes=n},r; void *out=NULL;
    int e=du_clip_call(&w,data,&r,&out); free(out); return e;
}
int du_clip_get(unsigned f, void **data, uint32_t *n)
{
    struct desk_clip_wire w={.op=DESK_CLIP_GET,.format=f},r; *data=NULL;
    int e=du_clip_call(&w,NULL,&r,data); if (!e) *n=r.bytes; return e;
}
int du_clip_register(const char *name)
{
    size_t n=strlen(name); if (!n || n>DESK_CLIP_NAME_MAX) return -EINVAL;
    struct desk_clip_wire w={.op=DESK_CLIP_REGISTER,.count=n},r; void *out=NULL; memcpy(w.name,name,n);
    int e=du_clip_call(&w,NULL,&r,&out); free(out); return e ? e : (int)r.format;
}
int du_clip_query(struct desk_clip_wire *r)
{
    struct desk_clip_wire w={.op=DESK_CLIP_QUERY}; void *out=NULL;
    int e=du_clip_call(&w,NULL,r,&out); free(out);
    if (!e) { query_cache=*r; query_valid=1; }
    return e;
}
int du_clip_available(unsigned format)
{
    if (!transport) return -ENOSYS;
    if (!watch_attempted) {
        struct desk_clip_wire w={.op=DESK_CLIP_WATCH,.flags=1},r; void *out=NULL;
        int e=du_clip_call(&w,NULL,&r,&out); free(out); if (e) return e;
    }
    if (!query_valid || !watch_active || query_cache.sequence!=observed) {
        struct desk_clip_wire r; int e=du_clip_query(&r); if (e) return e;
    }
    for (unsigned i=0;i<query_cache.count;i++) if (query_cache.formats[i]==format) return 1;
    return 0;
}
int du_system_clipboard_text_set(const char *s, size_t n)
{
    if (!transport) return -ENOSYS;
    void *data=NULL; uint32_t bytes; int e=clip_text_encode(s,n,&data,&bytes);
    if (e) return e;
    e=du_clip_open();
    if (!e) { e=du_clip_empty(); if (!e) e=du_clip_put(DESK_CF_UNICODETEXT,data,bytes); int closed=du_clip_close(); if (!e) e=closed; }
    if (!e) {
        char *next=NULL;
        if (!clip_text_decode(data,bytes,&next)) { free(text_cache); text_cache=next; text_sequence=observed; text_valid=1; }
    }
    free(data); return e;
}
const char *du_system_clipboard_text_get(void)
{
    if (!transport) return NULL;
    struct desk_clip_wire q;
    if (du_clip_query(&q)) return "";
    if (text_valid && text_sequence==q.sequence) return text_cache ? text_cache : "";
    int available=0; for (unsigned i=0;i<q.count;i++) if (q.formats[i]==13) available=1;
    if (!available) { free(text_cache); text_cache=NULL; text_valid=1; text_sequence=q.sequence; return ""; }
    void *data=NULL; uint32_t n=0; char *next=NULL;
    int e=du_clip_open(); if (e) return "";
    e=du_clip_get(DESK_CF_UNICODETEXT,&data,&n);
    if (!e) e=clip_text_decode(data,n,&next);
    du_clip_close(); free(data);
    if (e) { free(next); return ""; }
    free(text_cache); text_cache=next; text_sequence=observed; text_valid=1; return text_cache;
}
