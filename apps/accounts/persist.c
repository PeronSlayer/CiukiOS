/* SPDX-License-Identifier: MIT -- protected, forward-recovered snapshot files */
#include "accounts.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>

static int remove_if_present(struct acct_store *s, const char *name)
{ int e=s->io.remove(s->io.ctx,name); return e==-ENOENT?0:e; }
static int put(struct acct_store *s, const char *name, const void *p, size_t n)
{
    int e=s->io.write(s->io.ctx,name,p,n);
    if(!e) e=s->io.flush(s->io.ctx,name);
    if(!e) e=s->io.barrier(s->io.ctx);
    return e;
}
static int names(const char *base, char paths[3][128])
{
    if(strlen(base)>120) return -EINVAL;
    snprintf(paths[0],128,"%s",base); snprintf(paths[1],128,"%s.new",base);
    snprintf(paths[2],128,"%s.bak",base); return 0;
}
int acct_blob_read(struct acct_store *s, const char *base, void *out, size_t cap, size_t *bytes, uint64_t *sequence)
{
    char paths[3][128]; int e=names(base,paths); if(e) return e;
    uint8_t *p=s->kernel.allocate(s->kernel.ctx,cap+64); if(!p) return -ENOMEM;
    uint64_t best=0; int selected=-1; size_t length=0; uint8_t best_hash[32]={0};
    for(unsigned i=0;i<3;++i) {
        size_t n=0; e=s->io.read(s->io.ctx,paths[i],p,cap+64,&n);
        if(e==-ENOENT) { e=0; continue; }
        if(e) break;
        uint8_t sum[32], saved[32];
        if(n<64 || memcmp(p,"CIABLOB1",8) || a32(p+8)!=n || a32(p+12) || !a64(p+16) || a64(p+24)) { e=-EIO; break; }
        memcpy(saved,p+32,32); memset(p+32,0,32); sha256(p,n,sum);
        if(!acct_equal_secret(sum,saved,32)) { e=-EIO; break; }
        uint64_t seq=a64(p+16);
        if(seq==best && memcmp(saved,best_hash,32)) { e=-EIO; break; }
        if(seq>best) { best=seq; selected=(int)i; length=n-64; memcpy(out,p+64,length); memcpy(best_hash,saved,32); }
    }
    if(!e && selected<0) e=-ENOENT;
    if(!e && selected!=0) {
        e=remove_if_present(s,paths[0]);
        if(!e) e=s->io.rename(s->io.ctx,paths[selected],paths[0]);
        if(!e) e=s->io.barrier(s->io.ctx);
    }
    if(!e) e=remove_if_present(s,paths[1]);
    if(!e) e=remove_if_present(s,paths[2]);
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) { *bytes=length; *sequence=best; }
    acct_wipe(p,cap+64); s->kernel.deallocate(s->kernel.ctx,p,cap+64); return e;
}
int acct_blob_write(struct acct_store *s, const char *base, const void *data, size_t n, uint64_t *sequence)
{
    if(!sequence || *sequence==UINT64_MAX || n>UINT32_MAX-64) return -EOVERFLOW;
    char paths[3][128]; int e=names(base,paths); if(e) return e;
    uint8_t *p=s->kernel.allocate(s->kernel.ctx,n+64); if(!p) return -ENOMEM;
    memset(p,0,64); memcpy(p,"CIABLOB1",8); w32(p+8,(uint32_t)n+64); w64(p+16,*sequence+1);
    memcpy(p+64,data,n); sha256(p,n+64,p+32);
    /* Previous checkpoint is retained until the new file and its name are
     * durable. No trimming/publication of a floor precedes this barrier. */
    e=put(s,paths[1],p,n+64);
    if(!e) { e=s->io.rename(s->io.ctx,paths[0],paths[2]); if(e==-ENOENT) e=0; }
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=s->io.rename(s->io.ctx,paths[1],paths[0]);
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=remove_if_present(s,paths[2]);
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) ++*sequence;
    acct_wipe(p,n+64); s->kernel.deallocate(s->kernel.ctx,p,n+64); return e;
}
