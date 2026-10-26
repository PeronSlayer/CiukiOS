/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_OWNER_INTERNAL_H
#define CIUKI_OWNER_INTERNAL_H
#include <ciuki/abi.h>
#include "owner.h"
#include <ciuki/sha256.h>
static inline uint64_t ow64(const void *p) { return fs_rd32(p)|((uint64_t)fs_rd32((const uint8_t *)p+4)<<32); }
static inline void op64(void *p,uint64_t v) { fs_wr32(p,(uint32_t)v); fs_wr32((uint8_t *)p+4,(uint32_t)(v>>32)); }
static inline bool ozero(const void *p,size_t n) { const uint8_t *b=p; unsigned z=0; while(n--) z|=*b++; return !z; }
static inline bool opadded(const uint8_t *p,size_t n) { size_t i=0; while(i<n && p[i]) i++; return i<n && ozero(p+i,n-i); }
static inline bool osame(const char *a,const char *b) { return !memcmp(a,b,strlen(b)+1); }
#ifdef FS_HOST
extern unsigned owner_host_alloc_fail;
#endif
static inline void *oalloc(size_t n) {
#ifdef FS_HOST
    if(owner_host_alloc_fail && !--owner_host_alloc_fail) return 0;
#endif
    void *p=n<=2040?fs_alloc(n):fs_contig_alloc(n); if(p) memset(p,0,n); return p;
}
static inline void ofree(void *p,size_t n) { if(!p) return; if(n<=2040) fs_free(p); else fs_contig_free(p,n); }
struct owner_receipt {
    uint64_t transaction, generation;
    uint32_t state, count, request_bytes;
    /* Immutable exact wire request, then 32-byte result vector. */
    uint8_t body[];
};
static inline size_t receipt_size(const struct owner_receipt *r) { return sizeof(*r)+r->request_bytes+32*r->count; }
static inline uint8_t *receipt_results(struct owner_receipt *r) { return r->body+r->request_bytes; }
int owner_reply(const struct owner_request *,const struct owner_receipt *,unsigned state,void *,size_t,size_t *);
int owner_persist(struct owner_store *);
int owner_load(struct owner_store *,bool fresh);
int owner_save_records(struct owner_store *,const struct owner_request *,struct owner_receipt *);
int owner_pending_records(const struct owner_store *,const struct owner_request *,const struct owner_receipt *,struct owner_record *);
int owner_checkpoint_call(struct owner_store *,const struct owner_request *,unsigned,const void *);
int owner_checkpoint_recover(struct owner_store *);
int owner_intent(const struct owner_store *,const struct owner_request *);
size_t owner_store_bound(void);
void owner_security_unbind(struct owner_store *);
#endif
