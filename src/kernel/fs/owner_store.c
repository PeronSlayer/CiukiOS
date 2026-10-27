/* Alternating immutable snapshots with explicit durable commit markers.
 * A bad committed snapshot closes recovery, never falls back to older owners.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "owner_internal.h"
#define STORE_HEADER 224u
#define RECORD_BYTES 160u
#ifdef FS_HOST
unsigned owner_host_alloc_fail;
#endif
#define STORE_MAX (STORE_HEADER+288u+RECORD_BYTES*(OWNER_PROFILES*2+OWNER_ROOTS)+OWNER_RECEIPTS*(32u+OWNER_REQUEST_MAX+32u*32u))

size_t owner_store_bound(void) { return STORE_MAX; }
static void record_put(uint8_t *p,const struct owner_record *r)
{
    const struct owner_binding *b=&r->binding; memset(p,0,RECORD_BYTES);
    fs_wr32(p,b->uid); fs_wr32(p+4,b->action); op64(p+8,b->key); op64(p+16,b->generation);
    op64(p+24,b->volume); op64(p+32,b->locator); memcpy(p+40,b->path,64); memcpy(p+112,r->proof,32);
    fs_wr32(p+144,r->retired|(r->pending?2u:0u));
}
static int record_get(struct owner_store *s,struct owner_record *r,const uint8_t *p,bool profile)
{
    memset(r,0,sizeof(*r)); struct owner_binding *b=&r->binding;
    b->uid=fs_rd32(p); b->action=fs_rd32(p+4); b->key=ow64(p+8); b->generation=ow64(p+16);
    b->volume=ow64(p+24); b->locator=ow64(p+32); memcpy(b->path,p+40,64); memcpy(r->proof,p+112,32);
    r->retired=(fs_rd32(p+144)&1)!=0; r->pending=(fs_rd32(p+144)&2)!=0;
    if(!b->key || b->key>s->key_high || !b->locator || b->locator>s->locator_high ||
       !b->generation || b->generation>s->generation_high || b->volume!=s->volume ||
       (b->uid!=500 && b->uid!=501 && (b->uid<1000 || b->uid==UINT32_MAX) && (profile || b->uid!=0)) ||
       !opadded(p+40,64) || (profile && !owner_profile_path(b->path)) ||
       b->action<1 || b->action>3 || fs_rd32(p+144)>3 || !ozero(p+104,8) || !ozero(p+148,12)) return -EIO;
    return 0;
}
static size_t store_bytes(const struct owner_store *s)
{
    size_t n=STORE_HEADER+(s->checkpoint[0]?160:0)+(s->root_key?128:0)+RECORD_BYTES*(s->profile_count+s->root_count+(s->active?s->active->count:0));
    for(unsigned i=0;i<s->receipt_count;i++) n+=32+s->receipts[i]->request_bytes+32*s->receipts[i]->count;
    return n;
}
static int store_encode(const struct owner_store *s,uint8_t *p,size_t cap,size_t *bytes)
{
    size_t n=store_bytes(s); if(n>cap) return -ENOSPC;
    memset(p,0,n); memcpy(p,"CIUKIOWN",8); fs_wr32(p+8,1); fs_wr32(p+12,STORE_HEADER); op64(p+16,s->sequence);
    for(unsigned i=0;i<3;i++) fs_wr32(p+24+4*i,s->sid[i]);
    fs_wr32(p+36,s->profile_count); op64(p+40,s->volume); op64(p+48,s->generation); op64(p+56,s->generation_high);
    op64(p+64,s->key_high); op64(p+72,s->locator_high); op64(p+80,s->transaction_high); op64(p+88,s->floor);
    op64(p+96,s->account_high); fs_wr32(p+104,s->uid_high); fs_wr32(p+108,s->root_count); fs_wr32(p+112,s->receipt_count);
    fs_wr32(p+116,UINT32_MAX); fs_wr32(p+120,s->candidate?1:s->checkpoint[0]?2:0);
    op64(p+160,s->root_key); fs_wr32(p+168,s->root_operation);
    op64(p+176,s->committed_accounts_high); memcpy(p+192,s->committed_accounts_digest,32);
    size_t at=STORE_HEADER;
    if(s->checkpoint[0]) { memcpy(p+at,s->checkpoint,160); at+=160; }
    if(s->root_key) { memcpy(p+at,s->root_old,64); memcpy(p+at+64,s->root_new,64); at+=128; }
    for(unsigned i=0;i<s->profile_count;i++,at+=RECORD_BYTES) record_put(p+at,&s->profiles[i]);
    for(unsigned i=0;i<s->root_count;i++,at+=RECORD_BYTES) record_put(p+at,&s->roots[i]);
    if(s->active) for(unsigned i=0;i<s->active->count;i++,at+=RECORD_BYTES) record_put(p+at,&s->pending[i]);
    for(unsigned i=0;i<s->receipt_count;i++) {
        const struct owner_receipt *r=s->receipts[i];
        if(r==s->active) fs_wr32(p+116,i);
        op64(p+at,r->transaction); op64(p+at+8,r->generation); fs_wr32(p+at+16,r->state);
        fs_wr32(p+at+20,r->count); fs_wr32(p+at+24,r->request_bytes); at+=32;
        memcpy(p+at,r->body,r->request_bytes+32*r->count); at+=r->request_bytes+32*r->count;
    }
    sha256(p,n,p+128); *bytes=n; return 0;
}
static int store_decode(struct owner_store *s,uint8_t *p,size_t n)
{
    if(n<STORE_HEADER || memcmp(p,"CIUKIOWN",8) || fs_rd32(p+8)!=1 || fs_rd32(p+12)!=STORE_HEADER ||
       !ow64(p+16) || memcmp(p+24,s->sid,12) || ow64(p+40)!=s->volume ||
       fs_rd32(p+36)>32 || fs_rd32(p+108)>256 || fs_rd32(p+112)>1024 || fs_rd32(p+120)>2 ||
       fs_rd32(p+124) || fs_rd32(p+172) || fs_rd32(p+188)) return -EIO;
    uint8_t digest[32], saved[32]; memcpy(saved,p+128,32); memset(p+128,0,32); sha256(p,n,digest); memcpy(p+128,saved,32);
    if(memcmp(digest,saved,32)) return -EIO;
    s->sequence=ow64(p+16); s->generation=ow64(p+48); s->generation_high=ow64(p+56); s->key_high=ow64(p+64);
    s->locator_high=ow64(p+72); s->transaction_high=ow64(p+80); s->floor=ow64(p+88); s->account_high=ow64(p+96);
    s->uid_high=fs_rd32(p+104); s->profile_count=fs_rd32(p+36); s->root_count=fs_rd32(p+108); s->candidate=fs_rd32(p+120)==1;
    s->committed_accounts_high=ow64(p+176); memcpy(s->committed_accounts_digest,p+192,32);
    unsigned receipts=fs_rd32(p+112), active=fs_rd32(p+116); size_t at=STORE_HEADER;
    if(s->generation>s->generation_high || s->floor>s->transaction_high || s->account_high>s->transaction_high ||
       s->committed_accounts_high>s->transaction_high || (!!s->committed_accounts_high==ozero(s->committed_accounts_digest,32)) ||
       (active!=UINT32_MAX && active>=receipts)) return -EIO;
    if(fs_rd32(p+120)) { if(n-at<160) return -EIO; memcpy(s->checkpoint,p+at,160); at+=160; }
    s->root_key=ow64(p+160); s->root_operation=fs_rd32(p+168);
    if(s->root_key) {
        if(s->root_key>s->key_high || s->root_operation<1 || s->root_operation>3 || n-at<128 ||
           !opadded(p+at,64) || !opadded(p+at+64,64)) return -EIO;
        memcpy(s->root_old,p+at,64); memcpy(s->root_new,p+at+64,64); at+=128;
    } else if(s->root_operation) return -EIO;
    unsigned total=s->profile_count+s->root_count;
    if(n-at<(size_t)RECORD_BYTES*total) return -EIO;
    for(unsigned i=0;i<total;i++,at+=RECORD_BYTES) {
        struct owner_record *r=i<s->profile_count?&s->profiles[i]:&s->roots[i-s->profile_count];
        int e=record_get(s,r,p+at,i<s->profile_count); if(e) return e;
        for(unsigned j=0;j<i;j++) {
            const struct owner_record *old=j<s->profile_count?&s->profiles[j]:&s->roots[j-s->profile_count];
            if(old->binding.key==r->binding.key || old->binding.locator==r->binding.locator ||
               (i<s->profile_count && (old->binding.uid==r->binding.uid || osame(old->binding.path,r->binding.path)))) return -EIO;
        }
    }
    /* Active proofs precede the receipt vector. Their count is in the
     * active receipt, so the header carries the exact byte span at 184. */
    unsigned pending=fs_rd32(p+184);
    if(pending>32 || ((active==UINT32_MAX)!=(!pending)) || n-at<(size_t)pending*RECORD_BYTES) return -EIO;
    for(unsigned i=0;i<pending;i++,at+=RECORD_BYTES) { int e=record_get(s,&s->pending[i],p+at,true); if(e) return e; }
    uint64_t prior=s->floor;
    struct owner_request *q=oalloc(sizeof(*q)); if(!q) return -ENOMEM;
    int error=0;
    for(unsigned i=0;i<receipts;i++) {
        if(n-at<32) { error=-EIO; break; }
        unsigned state=fs_rd32(p+at+16), count=fs_rd32(p+at+20), bytes=fs_rd32(p+at+24);
        uint64_t g=ow64(p+at), generation=ow64(p+at+8);
        if(!count || count>32 || bytes!=160+128*count || fs_rd32(p+at+28) ||
           g<=prior || g>s->transaction_high || !generation || generation>s->generation_high ||
           (state!=1 && state!=2 && state!=3 && state!=5) ||
           ((state==1 || state==5)!=(i==active)) || n-at-32<bytes+32*count) { error=-EIO; break; }
        size_t allocation=sizeof(struct owner_receipt)+bytes+32*count;
        struct owner_receipt *r=oalloc(allocation); if(!r) { error=-ENOMEM; break; }
        r->transaction=g; r->generation=generation; r->state=state; r->count=count; r->request_bytes=bytes;
        at+=32; memcpy(r->body,p+at,bytes+32*count); at+=bytes+32*count;
        s->receipts[s->receipt_count++]=r; unsigned op;
        error=owner_request_decode(q,&op,r->body,bytes);
        if(!error && (q->volume!=s->volume || memcmp(q->sid,s->sid,12) || q->transaction!=g)) error=-EIO;
        if(error) break;
        for(unsigned j=0;j<count;j++) {
            const uint8_t *b=receipt_results(r)+32*j;
            if(fs_rd32(b)!=q->bindings[j].uid || fs_rd32(b+4)!=j || fs_rd32(b+24)!=q->bindings[j].action ||
               fs_rd32(b+28) || !ow64(b+8) || ow64(b+8)>s->key_high ||
               ow64(b+16)!=(q->bindings[j].action==OWNER_KEEP?q->bindings[j].generation:generation)) { error=-EIO; break; }
        }
        if(error) break;
        if(i==active) { if(count!=pending) { error=-EIO; break; } s->active=r; }
        prior=g;
    }
    ofree(q,sizeof(*q)); return error?error:at==n?0:-EIO;
}
int owner_persist(struct owner_store *s)
{
    if(s->sequence==UINT64_MAX) { s->ready=false; return -EOVERFLOW; }
    size_t n=store_bytes(s); uint8_t *p=oalloc(n); if(!p) { s->ready=false; return -ENOMEM; }
    s->sequence++;
    size_t bytes=0; int e=store_encode(s,p,n,&bytes);
    if(!e) { fs_wr32(p+184,s->active?s->active->count:0); memset(p+128,0,32); sha256(p,n,p+128); }
    unsigned slot=(unsigned)(s->sequence&1); uint8_t marker[48]={0}; op64(marker,s->sequence); op64(marker+8,n); if(!e) sha256(p,n,marker+16);
    if(!e) e=s->io.write(s->io.ctx,slot,p,n);
    if(!e) e=s->io.flush(s->io.ctx,slot);
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=s->io.write(s->io.ctx,slot+2,marker,sizeof(marker));
    if(!e) e=s->io.flush(s->io.ctx,slot+2);
    if(!e) e=s->io.barrier(s->io.ctx);
    ofree(p,n); if(e) s->ready=false; return e;
}
int owner_load(struct owner_store *s,bool fresh)
{
    uint8_t markers[2][48]; uint64_t sequence[2]={0};
    for(unsigned i=0;i<2;i++) {
        size_t n=0; int e=s->io.read(s->io.ctx,i+2,markers[i],48,&n);
        if(e==-ENOENT) continue;
        if(e) return e;
        if(n!=48 || !ow64(markers[i]) || ow64(markers[i]+8)>STORE_MAX || ow64(markers[i]+8)<STORE_HEADER) return -EIO;
        sequence[i]=ow64(markers[i]);
    }
    if(!sequence[0] && !sequence[1]) return fresh?owner_persist(s):-EIO;
    if(fresh) return -EEXIST;
    unsigned slot=sequence[1]>sequence[0]?1:0;
    size_t cap=(size_t)ow64(markers[slot]+8), n=0; uint8_t *p=oalloc(cap); if(!p) return -ENOMEM;
    int e=s->io.read(s->io.ctx,slot,p,cap,&n); uint8_t digest[32];
    if(!e && n>cap) e=-EIO;
    if(!e) { sha256(p,n,digest); if(n!=cap || memcmp(digest,markers[slot]+16,32)) e=-EIO; }
    if(!e) e=store_decode(s,p,n);
    if(!e && s->sequence!=sequence[slot]) e=-EIO;
    ofree(p,cap); return e;
}
void owner_close(struct owner_store *s)
{
    if(!s) return;
    extern void owner_service_unbind(struct owner_store *) __attribute__((weak));
    if(owner_service_unbind) owner_service_unbind(s);
    owner_security_unbind(s);
    for(unsigned i=0;i<s->receipt_count;i++) ofree(s->receipts[i],receipt_size(s->receipts[i]));
    memset(s,0,sizeof(*s));
}
