/* Exact little-endian OWNER codec, independent of C structure layout.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "owner_internal.h"

bool owner_profile_path(const char path[64])
{
    const char prefix[]="/Windows/Profiles/u";
    if (memcmp(path,prefix,sizeof(prefix)-1)) return false;
    for(unsigned i=sizeof(prefix)-1;i<sizeof(prefix)+7;i++)
        if (!((path[i]>='0' && path[i]<='9') || (path[i]>='a' && path[i]<='f'))) return false;
    return ozero(path+sizeof(prefix)+7,64-(sizeof(prefix)+7));
}
static int request_valid(const struct owner_request *q,unsigned op)
{
    if (!q || op<1 || op>7 || !q->volume || !q->transaction || ozero(q->sid,12) ||
        q->count>32 || ((op==5 || op==6)?q->count!=0:q->count==0) ||
        (q->count && q->accounts_generation>=q->transaction) ||
        ozero(q->accounts_digest,32) || ozero(q->manifest_digest,32)) return -EINVAL;
    for(unsigned i=0;i<q->count;i++) {
        const struct owner_binding *b=&q->bindings[i];
        if ((i && q->bindings[i-1].uid>=b->uid) ||
            (b->uid!=500 && b->uid!=501 && (b->uid<1000 || b->uid==UINT32_MAX)) ||
            b->action<1 || b->action>3 || b->volume!=q->volume || b->locator ||
            !opadded((const uint8_t *)b->path,64) || !owner_profile_path(b->path) ||
            (b->action==OWNER_CREATE?(b->key || b->generation):(!b->key || !b->generation))) return -EINVAL;
        for(unsigned j=0;j<i;j++) if (osame(b->path,q->bindings[j].path)) return -EINVAL;
    }
    return 0;
}
int owner_request_encode(const struct owner_request *q,unsigned op,void *v,size_t cap,size_t *bytes)
{
    int e=request_valid(q,op); if(e || !v || !bytes) return e?e:-EINVAL;
    size_t n=160+128*q->count; if(cap<n) return -ENOSPC;
    uint8_t *p=v; memset(p,0,n); fs_wr32(p,1); fs_wr32(p+4,160); fs_wr32(p+12,q->count);
    for(unsigned i=0;i<3;i++) fs_wr32(p+16+4*i,q->sid[i]);
    op64(p+32,q->volume); op64(p+40,q->transaction); op64(p+48,q->accounts_generation); op64(p+56,q->owner_generation);
    memcpy(p+64,q->accounts_digest,32); memcpy(p+96,q->manifest_digest,32);
    for(unsigned i=0;i<q->count;i++) {
        const struct owner_binding *b=&q->bindings[i]; uint8_t *r=p+160+128*i;
        fs_wr32(r,b->uid); fs_wr32(r+4,b->action); op64(r+8,b->key); op64(r+16,b->generation);
        op64(r+24,b->volume); fs_wr32(r+40,(uint32_t)strlen(b->path)+1); fs_wr32(r+44,1); memcpy(r+48,b->path,64);
    }
    sha256(p,n,p+128); fs_wr32(p+8,op); *bytes=n; return 0;
}
int owner_request_decode(struct owner_request *q,unsigned *op,const void *v,size_t n)
{
    const uint8_t *p=v;
    if (!q || !op || !p || n<160 || fs_rd32(p)!=1 || fs_rd32(p+4)!=160 || fs_rd32(p+28) ||
        fs_rd32(p+12)>32 || n!=160+128*fs_rd32(p+12)) return -EINVAL;
    memset(q,0,sizeof(*q)); q->count=fs_rd32(p+12); *op=fs_rd32(p+8);
    for(unsigned i=0;i<3;i++) q->sid[i]=fs_rd32(p+16+4*i);
    q->volume=ow64(p+32); q->transaction=ow64(p+40); q->accounts_generation=ow64(p+48); q->owner_generation=ow64(p+56);
    memcpy(q->accounts_digest,p+64,32); memcpy(q->manifest_digest,p+96,32); memcpy(q->digest,p+128,32);
    for(unsigned i=0;i<q->count;i++) {
        const uint8_t *r=p+160+128*i; struct owner_binding *b=&q->bindings[i];
        if(!opadded(r+48,64) || fs_rd32(r+40)!=strlen((const char *)r+48)+1 || fs_rd32(r+44)!=1 || !ozero(r+112,16)) return -EINVAL;
        b->uid=fs_rd32(r); b->action=fs_rd32(r+4); b->key=ow64(r+8); b->generation=ow64(r+16);
        b->volume=ow64(r+24); b->locator=ow64(r+32); memcpy(b->path,r+48,64);
    }
    int e=request_valid(q,*op); if(e) return e;
    struct sha256_ctx hash; uint8_t zero[32]={0}, digest[32];
    sha256_init(&hash); sha256_update(&hash,p,8); sha256_update(&hash,zero,4);
    sha256_update(&hash,p+12,116); sha256_update(&hash,zero,32); sha256_update(&hash,p+160,n-160); sha256_final(&hash,digest);
    return memcmp(digest,q->digest,32)?-EIO:0;
}
int owner_reply(const struct owner_request *q,const struct owner_receipt *r,unsigned state,void *v,size_t cap,size_t *bytes)
{
    unsigned count=r?r->count:0; size_t n=96+32*count;
    if(cap<n) return -ENOSPC;
    uint8_t *p=v; memset(p,0,n); fs_wr32(p,1); fs_wr32(p+4,96); fs_wr32(p+8,state); fs_wr32(p+12,count);
    for(unsigned i=0;i<3;i++) fs_wr32(p+16+4*i,q->sid[i]);
    op64(p+32,q->volume); op64(p+40,q->transaction); op64(p+48,r?r->generation:0); memcpy(p+64,q->digest,32);
    if(r) memcpy(p+96,r->body+r->request_bytes,32*count);
    *bytes=n; return 0;
}
static void staging_path(char out[64],uint64_t g,uint32_t uid)
{
    const char hex[]="0123456789abcdef", prefix[]="/System/Security/Staging/g";
    memset(out,0,64); memcpy(out,prefix,sizeof(prefix)-1); unsigned at=sizeof(prefix)-1;
    for(unsigned i=0;i<16;i++) out[at++]=hex[(g>>(60-4*i))&15];
    out[at++]='/'; out[at++]='p'; for(unsigned i=0;i<8;i++) out[at++]=hex[(uid>>(28-4*i))&15];
}
int owner_manifest_validate(struct owner_store *s,const struct owner_request *q)
{
    uint8_t *p=oalloc(OWNER_MANIFEST_MAX); if(!p) return -ENOMEM;
    size_t n=0; int e=s->io.manifest(s->io.ctx,q->transaction,false,p,OWNER_MANIFEST_MAX,&n);
    uint8_t digest[32];
    if(!e && n>OWNER_MANIFEST_MAX) e=-EIO;
    if(!e) {
        sha256(p,n,digest);
        if(n!=64+256*q->count || memcmp(p,"CIUKISTG",8) || fs_rd32(p+8)!=1 || fs_rd32(p+12)!=64 ||
           fs_rd32(p+16)!=q->count || fs_rd32(p+20) || fs_rd32(p+36) || memcmp(p+24,q->sid,12) ||
           ow64(p+40)!=q->volume || ow64(p+48)!=q->transaction || ow64(p+56) || memcmp(digest,q->manifest_digest,32)) e=-EIO;
    }
    for(unsigned i=0;!e && i<q->count;i++) {
        const uint8_t *b=p+64+256*i; const struct owner_binding *r=&q->bindings[i];
        uint32_t old=fs_rd32(b+4), flags=fs_rd32(b+12); char stage[64]; staging_path(stage,q->transaction,r->uid);
        if(fs_rd32(b)!=r->uid || fs_rd32(b+8)!=r->action || (flags!=1 && flags!=2 && flags!=6) ||
           (!!old!=!!(flags&2)) || !opadded(b+16,64) || memcmp(b+16,r->path,64) || !opadded(b+80,64) ||
           !opadded(b+144,64) || memcmp(b+144,stage,64) || ozero(b+208,32) || ow64(b+240)!=r->key || ow64(b+248)) { e=-EIO; break; }
        if(old) {
            char prior[64]={0}; const char prefix[]="/Windows/Profiles/u", hex[]="0123456789abcdef";
            memcpy(prior,prefix,sizeof(prefix)-1); for(unsigned j=0;j<8;j++) prior[sizeof(prefix)-1+j]=hex[(old>>(28-4*j))&15];
            if(r->uid<1000 || memcmp(prior,b+80,64) || (!!memcmp(b+16,b+80,64)!=!!(flags&4))) { e=-EIO; break; }
            e=s->io.import_map(s->io.ctx,q->transaction,old,r->uid,prior,r->path);
        } else if(!ozero(b+80,64)) e=-EIO;
        if(!e) e=s->io.tree(s->io.ctx,q->transaction,r->uid,digest);
        if(!e && memcmp(digest,b+208,32)) e=-EIO;
    }
    ofree(p,OWNER_MANIFEST_MAX); return e;
}
