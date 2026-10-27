/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include "wire.h"
#include <ciuki/sha256.h>
#include <errno.h>

static int valid(const struct acct_bootstrap *b)
{
    if (!b || b->state<1 || b->state>5 || b->origin<1 || b->origin>3 || !b->volume ||
        !b->generation || b->next_uid<1001 || (b->flags&~7u) || azero(b->seed_digest,32)) return 0;
    if (b->state==1) return (b->origin==3 || b->next_uid==1001) && azero(b->sid,12) &&
        !b->transaction && !b->accounts_generation && !b->owner_generation && !b->flags;
    if (azero(b->sid,12) || !b->transaction) return 0;
    if (b->state==2 && (b->owner_generation || b->accounts_generation || (b->flags&3))) return 0;
    if (b->state>=3 && (!b->owner_generation || !(b->flags&2))) return 0;
    if (b->state==3 && b->accounts_generation) return 0;
    if (b->state>=4 && b->accounts_generation!=b->transaction) return 0;
    return !!(b->flags&1)==(b->state==5);
}
int acct_boot_encode(const struct acct_bootstrap *b, void *v, size_t n)
{
    if (!valid(b) || !v || n!=160) return -EINVAL;
    uint8_t *p=v; memset(p,0,n); memcpy(p,"CIUKIBST",8); w32(p+8,1); w32(p+12,160);
    w32(p+16,b->state); w32(p+20,b->origin); w64(p+24,b->volume); memcpy(p+32,b->seed_digest,32);
    for(unsigned i=0;i<3;++i) w32(p+64+4*i,b->sid[i]);
    w64(p+80,b->transaction); w64(p+88,b->accounts_generation); w64(p+96,b->owner_generation);
    w32(p+104,b->next_uid); w32(p+108,b->flags); w64(p+112,b->generation); sha256(p,n,p+120); return 0;
}
int acct_boot_decode(struct acct_bootstrap *b, const void *v, size_t n)
{
    const uint8_t *p=v;
    if (!b || !p || n!=160 || memcmp(p,"CIUKIBST",8) || a32(p+8)!=1 || a32(p+12)!=160 || a32(p+76) || !azero(p+152,8)) return -EIO;
    uint8_t copy[160], sum[32]; memcpy(copy,p,160); memset(copy+120,0,32); sha256(copy,160,sum);
    if (!acct_equal_secret(sum,p+120,32)) return -EIO;
    struct acct_bootstrap t={0}; t.state=a32(p+16); t.origin=a32(p+20); t.volume=a64(p+24);
    memcpy(t.seed_digest,p+32,32); for(unsigned i=0;i<3;++i) t.sid[i]=a32(p+64+4*i);
    t.transaction=a64(p+80); t.accounts_generation=a64(p+88); t.owner_generation=a64(p+96);
    t.next_uid=a32(p+104); t.flags=a32(p+108); t.generation=a64(p+112);
    if (!valid(&t)) return -EIO;
    *b=t; return 0;
}
static int rm(struct acct_store *s, const char *p)
{ int e=s->io.remove(s->io.ctx,p); return e==-ENOENT?0:e; }
static int write_file(struct acct_store *s, const char *p, const void *v, size_t n)
{
    int e=s->io.write(s->io.ctx,p,v,n);
    if (!e) e=s->io.flush(s->io.ctx,p);
    if (!e) e=s->io.barrier(s->io.ctx);
    return e;
}
/* Validate all candidates and the intent, including candidates not selected.
 * A flushed .new plus valid old state can finish the monotonic transition even
 * if interruption preceded the intent. A corrupt candidate closes the gate. */
int acct_boot_read(struct acct_store *s, struct acct_bootstrap *out)
{
    static const char *const names[]={"bootstrap.state","bootstrap.state.new","bootstrap.state.bak"};
    struct acct_bootstrap states[3]; int have[3]={0}; int best=-1; uint8_t data[161], hashes[3][32]; size_t n;
    for(unsigned i=0;i<3;++i) {
        int e=s->io.read(s->io.ctx,names[i],data,sizeof(data),&n);
        if(e==-ENOENT) continue;
        if(e || acct_boot_decode(&states[i],data,n) || states[i].volume!=s->volume) return -EIO;
        have[i]=1; sha256(data,n,hashes[i]);
        if(best<0 || states[i].generation>states[best].generation) best=(int)i;
    }
    if(best<0) return -ENOENT;
    const struct acct_bootstrap *b=&states[best];
    for(unsigned i=0;i<3;++i) if(have[i]) {
        const struct acct_bootstrap *a=&states[i];
        if(a->origin!=b->origin || memcmp(a->seed_digest,b->seed_digest,32) || a->state>b->state ||
            (a->flags&~b->flags) || a->next_uid>b->next_uid ||
            a->transaction>b->transaction || a->accounts_generation>b->accounts_generation ||
            a->owner_generation>b->owner_generation ||
            (a->state>1 && memcmp(a->sid,b->sid,12)) ||
            (a->generation==b->generation && memcmp(hashes[i],hashes[best],32))) return -EIO;
    }
    uint8_t txn[97]; int e=s->io.read(s->io.ctx,"bootstrap.state.txn",txn,sizeof(txn),&n);
    if(!e) {
        if(n!=96) return -EIO;
        uint8_t sum[32]; sha256(txn,64,sum);
        if(!acct_equal_secret(sum,txn+64,32) ||
            (memcmp(txn,hashes[best],32) && memcmp(txn+32,hashes[best],32))) return -EIO;
    } else if(e!=-ENOENT) return e;
    if(best!=0) {
        e=rm(s,"bootstrap.state");
        if(!e) e=s->io.rename(s->io.ctx,names[best],"bootstrap.state");
        if(!e) e=s->io.barrier(s->io.ctx);
        if(e) return e;
    }
    e=rm(s,"bootstrap.state.new"); if(!e) e=rm(s,"bootstrap.state.bak");
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=rm(s,"bootstrap.state.txn"); if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) *out=*b;
    return e;
}
int acct_boot_write(struct acct_store *s, struct acct_bootstrap *b)
{
    struct acct_bootstrap old; int e=acct_boot_read(s,&old);
    if(e) return e; /* ACCOUNT never invents initial evidence. */
    if(old.generation==UINT64_MAX) return -EOVERFLOW;
    b->generation=old.generation+1;
    if(b->state<old.state || (old.flags&~b->flags) || b->volume!=old.volume || b->origin!=old.origin ||
        b->next_uid<old.next_uid || b->transaction<old.transaction ||
        b->accounts_generation<old.accounts_generation || b->owner_generation<old.owner_generation ||
        memcmp(b->seed_digest,old.seed_digest,32) ||
        (old.state>1 && memcmp(old.sid,b->sid,12))) return -EIO;
    uint8_t data[160], prior[160], txn[96];
    e=acct_boot_encode(b,data,160); if(e) return e;
    e=acct_boot_encode(&old,prior,160); if(e) return e;
    sha256(prior,160,txn); sha256(data,160,txn+32); sha256(txn,64,txn+64);
    e=write_file(s,"bootstrap.state.new",data,160);
    if(!e) e=write_file(s,"bootstrap.state.txn",txn,96);
    if(!e) e=s->io.rename(s->io.ctx,"bootstrap.state","bootstrap.state.bak");
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=s->io.rename(s->io.ctx,"bootstrap.state.new","bootstrap.state");
    if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=rm(s,"bootstrap.state.bak"); if(!e) e=s->io.barrier(s->io.ctx);
    if(!e) e=rm(s,"bootstrap.state.txn"); if(!e) e=s->io.barrier(s->io.ctx);
    return e;
}
int acct_autologon_eligible(const struct acct_db *d, const struct acct_bootstrap *b, int safe, int off, int locked)
{
    return d && valid(b) && !safe && !off && !locked && !acct_validate(d,0) && b->state==ACCT_CONSUMED &&
        !(b->flags&ACCT_AUTO_RETIRED) && !(d->flags&ACCT_AUTO_RETIRED) && d->auto_logon_uid==1000 &&
        b->volume && b->origin==d->origin && !memcmp(b->sid,d->sid,12) && b->accounts_generation==d->generation;
}
