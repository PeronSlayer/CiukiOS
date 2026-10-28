/* sec-own: OWNER binding and system-volume publication (owner_system.h).
 *
 * One system volume per boot. Its kernel table reads the ACCOUNT role's
 * protected evidence under /System/Security for the OWNER core
 * (owner_account_evidence) and the first-boot gate; nothing here writes an
 * ACCOUNT file. Lifecycle (user-security-contract.md 3.5/3.6, kernel half
 * "Owner metadata and consistency"):
 *  - machine.sid present: open the existing store (recovery included), bind
 *    the OWNER service, then the volume's policy and OWNER lease;
 *  - no SID and no store: UNPROVISIONED. The volume stays unmanaged (k03's
 *    interim rule) and the first RESERVE from ACCOUNT may create the store
 *    only after the fresh-provisioning gate below proves, from the
 *    destination's own durable evidence, an UNPROVISIONED installation with
 *    this exact SID intent, seed and volume and no published database;
 *  - anything else (SID without store, unreadable or contradictory store):
 *    FAILED. The account service then fails closed (every OWNER request is
 *    EIO); the volume stays unmanaged so the interim administrator desktop
 *    keeps its boot (decision recorded in docs/validation/sec-own.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#include "owner_internal.h"
#include "owner_vfs.h"
#include "owner_channel.h"
#include "owner_system.h"
#include "security_policy.h"
#include "path.h"
#include "fat.h"
#include <ciuki/security.h>
#ifdef FS_HOST
#include <stdio.h>
#define owner_log(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)
#else
#include <ciuki/kernel.h>
#include <ciuki/storage.h>
#define owner_log(...) klog(__VA_ARGS__)
#endif

/* Account-service record sizes mirrored from apps/accounts (disk schemas of
 * user-security-contract.md 3.3/3.6, private to the two roles). */
#define ACCOUNT_DB_MAX (96u+18u*256u)
#define ACCOUNT_INTENT_MAX (192u+OWNER_REQUEST_MAX+ACCOUNT_DB_MAX)
#define ACCOUNT_ENROLL_BYTES (128u+18u*256u+16u*72u)
#define ACCOUNT_BLOB_MAX (ACCOUNT_ENROLL_BYTES+64u)
#define SNAPSHOT_MAX (160u+112u*OWNER_PROFILES+160u*OWNER_ROOTS+OWNER_RETAIN*(8u+OWNER_REQUEST_MAX+OWNER_REPLY_MAX))

static struct owner_system {
    struct vfs *vfs;
    unsigned drive;
    uint64_t volume, mount;
    struct vfs_table table;
    struct owner_store store;
    struct owner_vfs backend;
    struct owner_backend io;
    struct policy_volume policy;
    int state, error;
    bool table_ready, readonly, bound;
} sys;

int owner_system_state(void) { return sys.state; }
uint64_t owner_system_volume(void) { return sys.state==OWNER_SYSTEM_NONE?0:sys.volume; }

/* ---- protected evidence reads (kernel table, never written here) ---- */
static int sec_path(const char *name,char out[FS_PATH_BYTES])
{
    static const char base[]="/System/Security/";
    size_t n=strlen(name);
    if(!n || n+sizeof(base)+3>FS_PATH_BYTES) return -ENAMETOOLONG;
    out[0]=(char)('A'+sys.drive); out[1]=':';
    memcpy(out+2,base,sizeof(base)-1); memcpy(out+2+sizeof(base)-1,name,n+1);
    return 0;
}
/* Whole file, at most cap bytes: ENOENT when absent, EIO when larger. */
static int sec_read(const char *name,void *buffer,size_t cap,size_t *bytes)
{
    char path[FS_PATH_BYTES]; int e=sec_path(name,path); if(e) return e;
    *bytes=0;
    int fd=vfs_open(&sys.table,path,VFS_READ,VFS_DENY_NONE,0);
    if(fd<0) return fd==-FS_ENOTDIR?-ENOENT:fd;
    struct fat_entry entry; e=vfs_handle_stat(&sys.table,fd,&entry,0);
    if(!e && (entry.attr&FAT_ATTR_DIR)) e=-EISDIR;
    if(!e && entry.size>cap) e=-EIO;
    size_t got=0;
    while(!e && got<entry.size) {
        size_t n=0; e=vfs_read(&sys.table,fd,(uint8_t *)buffer+got,entry.size-got,&n);
        if(!e && !n) e=-EIO;
        got+=n;
    }
    int closed=vfs_close(&sys.table,fd); if(!e) e=closed;
    if(!e) *bytes=got;
    return e;
}
static int sec_absent(const char *name)
{
    char path[FS_PATH_BYTES]; int e=sec_path(name,path); if(e) return e;
    struct fat_entry entry; e=vfs_stat(&sys.table,path,&entry);
    return e==-ENOENT || e==-FS_ENOTDIR?0:e?e:-EEXIST;
}
/* ACCOUNT's forward-recovered snapshot blob (CIABLOB1, apps/accounts/
 * persist.c): the valid candidate with the highest sequence among name,
 * name.new and name.bak, read-only. Equal sequences must be identical. */
static int blob_read(const char *base,uint8_t *payload,size_t cap,size_t *bytes)
{
    static const char *const suffix[3]={"",".new",".bak"};
    uint8_t *p=oalloc(cap+64); if(!p) return -ENOMEM;
    uint64_t best=0; uint8_t best_hash[32]={0}; int e=0, found=0;
    for(unsigned i=0;!e && i<3;i++) {
        char name[48]; size_t b=strlen(base), s=strlen(suffix[i]), n=0;
        if(b+s+1>sizeof(name)) { e=-ENAMETOOLONG; break; }
        memcpy(name,base,b); memcpy(name+b,suffix[i],s+1);
        int r=sec_read(name,p,cap+64,&n);
        if(r==-ENOENT) continue;
        if(r) { e=r; break; }
        uint8_t saved[32], sum[32];
        if(n<64 || memcmp(p,"CIABLOB1",8) || fs_rd32(p+8)!=n || fs_rd32(p+12) || !ow64(p+16) || ow64(p+24)) { e=-EIO; break; }
        memcpy(saved,p+32,32); memset(p+32,0,32); sha256(p,n,sum);
        if(memcmp(sum,saved,32)) { e=-EIO; break; }
        uint64_t sequence=ow64(p+16);
        if(sequence==best && memcmp(saved,best_hash,32)) { e=-EIO; break; }
        if(sequence>best) { best=sequence; memcpy(best_hash,saved,32); memcpy(payload,p+64,n-64); *bytes=n-64; found=1; }
    }
    ofree(p,cap+64);
    return e?e:found?0:-ENOENT;
}
/* accounts.db (CIUKISEC v2): published generation and whole-file digest. */
static int ev_current(void *ctx,uint64_t *generation,uint8_t digest[32])
{
    (void)ctx;
    uint8_t *p=oalloc(ACCOUNT_DB_MAX); if(!p) return -ENOMEM;
    size_t n=0; int e=sec_read("accounts.db",p,ACCOUNT_DB_MAX,&n);
    if(e==-ENOENT) { *generation=0; memset(digest,0,32); e=0; }
    else if(!e) {
        uint8_t saved[32], sum[32];
        if(n<96 || memcmp(p,"CIUKISEC",8) || fs_rd32(p+8)!=2 || fs_rd32(p+12)!=96 || fs_rd32(p+36)!=256 ||
           fs_rd32(p+28)>18 || n!=96+256*fs_rd32(p+28) || !ow64(p+16)) e=-EIO;
        if(!e) { memcpy(saved,p+64,32); memset(p+64,0,32); sha256(p,n,sum); memcpy(p+64,saved,32);
                 if(memcmp(sum,saved,32)) e=-EIO; }
        if(!e) { *generation=ow64(p+16); sha256(p,n,digest); }
    }
    ofree(p,ACCOUNT_DB_MAX); return e;
}
/* An ACCOUNT phase marker (CIUKIPHS, apps/accounts/store.c): 1 for this exact
 * key/digest, 0 absent or left by an older finished batch, -EIO otherwise. */
static int phase_marker(const char *name,const struct owner_request *q)
{
    uint8_t p[97], sum[32], label[16]={0}; size_t n=0;
    int e=sec_read(name,p,sizeof(p),&n);
    if(e==-ENOENT) return 0;
    if(e) return e;
    memcpy(label,name,strlen(name));
    sha256(p,64,sum);
    if(n!=96 || memcmp(p,"CIUKIPHS",8) || memcmp(p+48,label,16) || memcmp(sum,p+64,32)) return -EIO;
    if(ow64(p+8)<q->transaction) return 0;
    return ow64(p+8)==q->transaction && !memcmp(p+16,q->digest,32)?1:-EIO;
}
static int ev_intent(void *ctx,const struct owner_request *q)
{
    (void)ctx;
    int commit=phase_marker("acct.commit",q); if(commit<0) return commit;
    int aborted=phase_marker("acct.abort",q); if(aborted<0) return aborted;
    if(commit && aborted) return -EIO;
    if(!commit) return 0;
    /* COMMIT_REQUIRED needs the complete durable intent carrying this request. */
    uint8_t *p=oalloc(ACCOUNT_INTENT_MAX); if(!p) return -ENOMEM;
    size_t n=0; int e=sec_read("accounts.db.txn",p,ACCOUNT_INTENT_MAX,&n);
    if(e==-ENOENT) e=-EIO;
    if(!e && (n<192 || memcmp(p,"CIACTXN3",8) || fs_rd32(p+8)!=n || fs_rd32(p+12)!=160+128*q->count ||
              n!=192+fs_rd32(p+12)+fs_rd32(p+16) || ow64(p+192+40)!=q->transaction ||
              memcmp(p+192+128,q->digest,32))) e=-EIO;
    ofree(p,ACCOUNT_INTENT_MAX);
    return e?e:1;
}
/* ACCOUNT's durable checkpoint marker (owner.checkpoint): the exact
 * CHECKPOINT header for this floor plus the allocation watermarks. */
static int ev_checkpoint(void *ctx,const struct owner_request *q,uint64_t high_g,uint32_t high_uid)
{
    (void)ctx;
    uint8_t marker[192], expected[160]; size_t n=0, m=0;
    int e=blob_read("owner.checkpoint",marker,sizeof(marker),&n);
    if(e) return e;
    if(n!=192) return -EIO;
    e=owner_request_encode(q,OWNER_CHECKPOINT,expected,sizeof(expected),&m);
    if(e || m!=160 || memcmp(marker,expected,160) || ow64(marker+160)!=high_g ||
       fs_rd32(marker+168)!=high_uid || !ozero(marker+172,20)) return -EIO;
    return 0;
}
/* Protected enrollment catalogue (CIUKIEN1, apps/accounts/enrollment.c): the
 * retained G32 root of old_id maps to uid under this exact generation. */
static int ev_import(void *ctx,uint64_t g,uint32_t old,uint32_t uid,const char *prior,const char *destination)
{
    (void)ctx;
    uint8_t *p=oalloc(ACCOUNT_ENROLL_BYTES); if(!p) return -ENOMEM;
    size_t n=0; int e=blob_read("enrollment.catalogue",p,ACCOUNT_ENROLL_BYTES,&n);
    if(e==-ENOENT) e=-EACCES;
    if(!e && (n!=ACCOUNT_ENROLL_BYTES || memcmp(p,"CIUKIEN1",8) || ow64(p+8)!=g || fs_rd32(p+52)>16)) e=-EACCES;
    int matched=0;
    for(unsigned i=0;!e && i<fs_rd32(p+52);i++) {
        const uint8_t *map=p+128+18*256+72*i; char root[64];
        if(fs_rd32(map)!=old) continue;
        memcpy(root,map+8,64);
        if(!opadded((const uint8_t *)root,64) || fs_rd32(map+4)!=uid || !osame(root,destination) ||
           strlen(prior)>=64) e=-EACCES;
        matched=1; break;
    }
    ofree(p,ACCOUNT_ENROLL_BYTES);
    return e?e:matched?0:-EACCES;
}

/* ---- first-boot gate (contract 3.6 first row) ---- */
static int boot_state(uint8_t state[160],uint32_t *origin)
{
    size_t n=0; int e=sec_read("bootstrap.state",state,160,&n);
    if(e) return e==-ENOENT?-EIO:e;
    uint8_t saved[32], sum[32];
    memcpy(saved,state+120,32); memset(state+120,0,32); sha256(state,160,sum); memcpy(state+120,saved,32);
    if(n!=160 || memcmp(state,"CIUKIBST",8) || fs_rd32(state+8)!=1 || fs_rd32(state+12)!=160 ||
       memcmp(sum,saved,32) || fs_rd32(state+16)!=1 || fs_rd32(state+20)<1 || fs_rd32(state+20)>3 ||
       ow64(state+24)!=sys.volume || ozero(state+32,32) || !ozero(state+64,16) || ow64(state+80) ||
       ow64(state+88) || ow64(state+96) || fs_rd32(state+108) || !ow64(state+112) || !ozero(state+152,8)) return -EIO;
    *origin=fs_rd32(state+20); return 0;
}
static int machine_sid(uint32_t sid[3])
{
    uint8_t p[65], sum[32]; size_t n=0;
    int e=sec_read("machine.sid",p,sizeof(p),&n); if(e) return e;
    sha256(p,32,sum);
    if(n!=64 || memcmp(p,"CIUKISID",8) || fs_rd32(p+20) || ow64(p+24)!=sys.volume || memcmp(sum,p+32,32)) return -EIO;
    for(unsigned i=0;i<3;i++) sid[i]=fs_rd32(p+8+4*i);
    return ozero(sid,12)?-EIO:0;
}
static int fresh_gate(const struct owner_request *q)
{
    if(q->volume!=sys.volume || q->accounts_generation || q->owner_generation) return -EIO;
    uint32_t sid[3]; int e=machine_sid(sid);
    if(e) return e==-ENOENT?-EIO:e;
    if(memcmp(sid,q->sid,12)) return -EIO;
    uint8_t state[161]; uint32_t origin=0;
    if((e=boot_state(state,&origin))) return e;
    static const char *const absent[]={"bootstrap.state.new","bootstrap.state.bak","bootstrap.state.txn",
        "accounts.db","accounts.db.new","accounts.db.bak","Owner/commit.0","Owner/commit.1"};
    for(unsigned i=0;i<sizeof(absent)/sizeof(*absent);i++) if((e=sec_absent(absent[i]))) return e==-EEXIST?-EIO:e;
    /* The installation's own SID intent (ACCOUNT bootstrap.sid) binds the
     * SID to this seed and volume; a copied SID file alone is not enough. */
    uint8_t p[129], sum[32]; size_t n=0;
    if((e=sec_read("bootstrap.sid",p,sizeof(p),&n))) return e==-ENOENT?-EIO:e;
    sha256(p,96,sum);
    if(n!=128 || memcmp(p,"CIUKISPR",8) || fs_rd32(p+8)!=origin || fs_rd32(p+12) || ow64(p+16)!=sys.volume ||
       memcmp(p+24,q->sid,12) || fs_rd32(p+36) || memcmp(p+40,state+32,32) || !ozero(p+72,24) ||
       memcmp(sum,p+96,32)) return -EIO;
    if(origin!=3) {
        uint8_t *seed=oalloc(ACCOUNT_DB_MAX); if(!seed) return -ENOMEM;
        e=sec_read("provision.seed",seed,ACCOUNT_DB_MAX,&n);
        if(!e) { sha256(seed,n,sum); if(memcmp(sum,state+32,32)) e=-EIO; }
        ofree(seed,ACCOUNT_DB_MAX);
        if(e) return e==-ENOENT?-EIO:e;
    }
    return 0;
}

/* ---- store, service and policy binding ---- */
static int bind_policy(void)
{
    int e=policy_init(&sys.policy,sys.volume,sys.mount,true,false,sys.readonly,&sys.store);
    if(e) return e;
    sys.policy.lease=&sys.backend;
    vfs_lock(sys.vfs,0);
    e=policy_bind_locked(sys.vfs,sys.drive,&sys.policy);
    vfs_unlock(sys.vfs);
    if(!e) sys.bound=true;
    return e;
}
static int make_directory(const char *name)
{
    char path[FS_PATH_BYTES]; int e=sec_path(name,path); if(e) return e;
    e=vfs_mkdir(&sys.table,path,FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM);
    return e==-EEXIST?0:e;
}
static int open_store(bool fresh,const uint32_t sid[3])
{
    struct owner_account_evidence evidence={&sys,ev_intent,ev_current,ev_checkpoint,ev_import};
    int e=owner_vfs_backend(&sys.backend,&sys.table,sys.volume,&evidence,&sys.io);
    if(!e && fresh) e=make_directory("Owner");
    if(!e) e=owner_open(&sys.store,&sys.io,sid,sys.volume,fresh);
    if(!e) e=owner_service_bind(&sys.store);
    if(!e) e=bind_policy();
    if(e) {
        owner_close(&sys.store);
        sys.state=OWNER_SYSTEM_FAILED; sys.error=e;
        owner_log("[owner] store unavailable volume=%016llx fresh=%u error=%d; volume stays unmanaged",
                  (unsigned long long)sys.volume,fresh?1u:0u,e);
        return e;
    }
    sys.state=OWNER_SYSTEM_OPEN; sys.error=0;
    owner_log("[owner] store open volume=%016llx fresh=%u profiles=%u roots=%u generation=%llu floor=%llu",
              (unsigned long long)sys.volume,fresh?1u:0u,sys.store.profile_count,sys.store.root_count,
              (unsigned long long)sys.store.generation,(unsigned long long)sys.store.floor);
    owner_log("[owner] system volume %c: policy bound (managed rules, OWNER lease)",'A'+sys.drive);
    return 0;
}
int owner_system_attach(struct vfs *v,unsigned drive,uint64_t volume,bool readonly)
{
    if(!v || drive>=26 || sys.vfs) return -EINVAL;
    memset(&sys,0,sizeof(sys));
    sys.vfs=v; sys.drive=drive; sys.volume=volume; sys.readonly=readonly;
    sys.mount=((uint64_t)v->generation[drive]<<8)|(drive+1u);
    owner_service_dispatcher(owner_system_dispatch);
    if(!volume) {
        owner_log("[owner] system volume %c: not managed (no partition/serial identity)",'A'+drive);
        return 0;
    }
    int e=vfs_table_init(v,&sys.table,drive);
    if(e) { sys.state=OWNER_SYSTEM_FAILED; sys.error=e; return e; }
    sys.table_ready=true;
    uint32_t sid[3]; e=machine_sid(sid);
    int store=sec_absent("Owner/commit.0"); if(!store) store=sec_absent("Owner/commit.1");
    if(!e && store==-EEXIST) return open_store(false,sid);
    if((!e || e==-ENOENT) && !store) {
        /* No committed store. Without a SID this is a fresh installation; with
         * one, only a first account batch cut before OWNER's first durable
         * record, while bootstrap.state is still UNPROVISIONED (contract 3.6:
         * reuse this SID). The RESERVE gate re-proves everything. */
        uint8_t state[161]; uint32_t origin=0;
        int unprovisioned=e==-ENOENT?0:boot_state(state,&origin);
        if(!unprovisioned) {
            sys.state=OWNER_SYSTEM_UNPROVISIONED;
            owner_log("[owner] system volume %c: unprovisioned volume=%016llx%s; unmanaged until the first account batch",
                      'A'+drive,(unsigned long long)volume,e?"":" (installation SID kept)");
            return 0;
        }
        e=unprovisioned;
    } else if(!e || e==-ENOENT) e=store==-EEXIST?-EIO:store;
    sys.state=OWNER_SYSTEM_FAILED; sys.error=e;
    owner_log("[owner] store unavailable volume=%016llx error=%d (installation identity); volume stays unmanaged",
              (unsigned long long)volume,e);
    return e;
}
void owner_system_detach(void)
{
    if(!sys.vfs) return;
    owner_service_dispatcher(0);
    if(sys.bound) { vfs_lock(sys.vfs,0); if(sys.vfs->policy[sys.drive]==&sys.policy) policy_unbind_locked(sys.vfs,sys.drive); vfs_unlock(sys.vfs); }
    if(sys.state==OWNER_SYSTEM_OPEN) owner_close(&sys.store);
    if(sys.table_ready) vfs_table_destroy(&sys.table);
    memset(&sys,0,sizeof(sys));
}
int owner_system_profile(uint32_t uid,struct ciuki_security_profile *out)
{
    if(!out || sys.state!=OWNER_SYSTEM_OPEN) return -ENOENT;
    struct owner_record r; int e=owner_profile(&sys.store,uid,&r); if(e) return e;
    size_t n=strlen(r.binding.path);
    if(!n || n>=64) return -EIO;
    memset(out,0,sizeof(*out));
    out->version=1; out->size=112; out->uid=uid;
    out->volume_id_lo=(uint32_t)r.binding.volume; out->volume_id_hi=(uint32_t)(r.binding.volume>>32);
    out->owner_generation_lo=(uint32_t)r.binding.generation; out->owner_generation_hi=(uint32_t)(r.binding.generation>>32);
    out->owner_key_lo=(uint32_t)r.binding.key; out->owner_key_hi=(uint32_t)(r.binding.key>>32);
    out->path_bytes=(uint32_t)n+1; memcpy(out->path,r.binding.path,n);
    return 0;
}

/* ---- OWNER service dispatch ---- */
/* Same admission as owner_call: the role-bound link of the sealed ACCOUNT
 * (or boot SETUP) service, SYSTEM credential. */
static int authorize(const struct security_subject *subject,unsigned op)
{
    int e=security_subject_check(subject,(3u<<16)|op,0); if(e) return e;
    const struct ciuki_credential *c=security_subject_credential(subject);
    return !c || c->uid?-EPERM:0;
}
static bool store_enter(void)
{ bool expected=false; return __atomic_compare_exchange_n(&sys.store.busy,&expected,true,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED); }
static void store_leave(void) { __atomic_store_n(&sys.store.busy,false,__ATOMIC_RELEASE); }
static bool root_syntax(const char *path)
{
    if(path[0]!='/' || !path[1]) return false;
    for(const char *p=path+1;*p;p++) if((uint8_t)*p<32 || (uint8_t)*p>126 || *p=='\\' || *p==':' || (*p=='/' && p[1]=='/')) return false;
    return path[strlen(path)-1]!='/';
}
/* SNAPSHOT: the candidate checkpoint image (owner_checkpoint_image) is
 * written to the SYSTEM-only Owner/snapshot file; the reply names its size
 * and digest. Nothing is published: CHECKPOINT/COMPACT stay the protocol. */
static int snapshot(const uint8_t *q,uint8_t *r)
{
    uint8_t *image=oalloc(SNAPSHOT_MAX); if(!image) return -ENOMEM;
    size_t n=0; uint64_t floor=0;
    int e=owner_checkpoint_image(&sys.store,ow64(q+40),q+48,ow64(q+24),fs_rd32(q+32),image,SNAPSHOT_MAX,&n,&floor);
    char path[FS_PATH_BYTES];
    if(!e) e=sec_path("Owner/snapshot",path);
    if(!e) {
        int fd=vfs_open(&sys.table,path,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM);
        if(fd<0) e=fd;
        else {
            size_t wrote=0; e=vfs_write(&sys.table,fd,image,n,&wrote);
            if(!e && wrote!=n) e=-EIO;
            if(!e) e=vfs_commit(&sys.table,fd);
            int closed=vfs_close(&sys.table,fd); if(!e) e=closed;
        }
    }
    if(!e) { op64(r+32,floor); op64(r+72,n); sha256(image,n,r+80); }
    ofree(image,SNAPSHOT_MAX); return e;
}
static int query(const struct security_subject *subject,const uint8_t *q,size_t n,uint8_t *r,size_t cap,size_t *bytes)
{
    unsigned op=fs_rd32(q+8);
    int e=authorize(subject,op); if(e) return e;
    if(n!=OWNER_QUERY_BYTES || cap<OWNER_QUERY_BYTES || fs_rd32(q)!=1 || fs_rd32(q+4)!=OWNER_QUERY_BYTES ||
       fs_rd32(q+36) || !ozero(q+144,16) || (q[80] && !opadded(q+80,64)) || (!q[80] && !ozero(q+80,64))) return -EINVAL;
    uint64_t volume=ow64(q+16);
    memset(r,0,OWNER_QUERY_BYTES);
    fs_wr32(r,1); fs_wr32(r+4,OWNER_QUERY_BYTES); fs_wr32(r+8,op);
    if(sys.state==OWNER_SYSTEM_NONE) return op==OWNER_IDENTITY?-ENODEV:-EIO;
    if(op!=OWNER_IDENTITY && volume!=sys.volume) return -EACCES;
    bool open=sys.state==OWNER_SYSTEM_OPEN && sys.store.ready;
    /* IDENTITY names the volume in every state (2: store unavailable), so
     * ACCOUNT binds it and answers its clients at once with EIO. */
    fs_wr32(r+12,open?1:sys.state==OWNER_SYSTEM_UNPROVISIONED?0:2); op64(r+16,sys.volume);
    if(open) { op64(r+24,sys.store.generation); op64(r+32,sys.store.floor); fs_wr32(r+40,sys.store.receipt_count); }
    if(op!=OWNER_IDENTITY && sys.state!=OWNER_SYSTEM_UNPROVISIONED && !open) return -EIO;
    switch(op) {
    case OWNER_IDENTITY: case OWNER_GENERATION: case OWNER_CHECKPOINT_FLOOR: break;
    case OWNER_BINDING: {
        if(!open) return -ENOENT;
        struct owner_record record; e=owner_profile(&sys.store,fs_rd32(q+12),&record); if(e) return e;
        op64(r+48,record.binding.key); op64(r+56,record.binding.generation); fs_wr32(r+64,record.binding.uid);
        fs_wr32(r+68,1); memcpy(r+72,record.binding.path,64);
        break;
    }
    case OWNER_ROOT_OCCUPIED: {
        char path[64]; memcpy(path,q+80,64);
        if(!path[0] || !root_syntax(path)) return -EINVAL;
        if(open) e=owner_root_occupied(&sys.store,sys.volume,path);
        else {
            char full[FS_PATH_BYTES]; struct fat_entry entry; size_t k=strlen(path);
            full[0]=(char)('A'+sys.drive); full[1]=':'; memcpy(full+2,path,k+1);
            e=vfs_stat(&sys.table,full,&entry); e=e==-ENOENT || e==-FS_ENOTDIR?0:e?e:1;
        }
        if(e<0) return e;
        fs_wr32(r+44,(uint32_t)e);
        break;
    }
    case OWNER_SNAPSHOT:
        if(!open) return -EAGAIN;
        if(!store_enter()) return -EBUSY;
        e=snapshot(q,r); store_leave();
        if(e) return e;
        break;
    case OWNER_RECOVER_ORPHANS:
        /* An owner batch that ACCOUNT no longer carries is finished from
         * ACCOUNT's own durable intent: committed after COMMIT_REQUIRED,
         * otherwise aborted (owner_recover); watermarks never decrease. */
        if(open && sys.store.active && sys.store.active->transaction!=ow64(q+24)) {
            if(!store_enter()) return -EBUSY;
            e=owner_recover(&sys.store); store_leave();
            if(e) return e;
            op64(r+24,sys.store.generation); fs_wr32(r+40,sys.store.receipt_count);
        }
        break;
    default: return -EOPNOTSUPP;
    }
    *bytes=OWNER_QUERY_BYTES; return 0;
}
int owner_system_dispatch(const struct security_subject *subject,const void *wire,size_t n,void *reply,size_t cap,size_t *bytes)
{
    if(!bytes) return -EINVAL;
    *bytes=0;
    if(!wire || !reply || n<12) return -EINVAL;
    unsigned op=fs_rd32((const uint8_t *)wire+8);
    if(op>=OWNER_IDENTITY && op<=OWNER_CHECKPOINT_FLOOR) return query(subject,wire,n,reply,cap,bytes);
    if(sys.state==OWNER_SYSTEM_OPEN) return owner_call(&sys.store,subject,wire,n,reply,cap,bytes);
    if(sys.state!=OWNER_SYSTEM_UNPROVISIONED) return -EIO;
    int e=authorize(subject,op); if(e) return e;
    if(n>OWNER_REQUEST_MAX || n<160) return -EINVAL;
    struct owner_request *q=oalloc(sizeof(*q)); if(!q) return -ENOMEM;
    e=owner_request_decode(q,&op,wire,n);
    if(!e && q->volume!=sys.volume) e=-EACCES;
    bool call=false;
    if(!e) {
        if(op==OWNER_RESERVE) {
            e=fresh_gate(q);
            if(e) owner_log("[owner] first account batch refused volume=%016llx error=%d",(unsigned long long)sys.volume,e);
            else if(!(e=open_store(true,q->sid))) call=true;
        } else if(op==OWNER_STATUS || op==OWNER_ABORT) e=owner_reply(q,0,OWNER_UNKNOWN,reply,cap,bytes);
        else e=-EIO;
    }
    ofree(q,sizeof(*q));
    return call?owner_call(&sys.store,subject,wire,n,reply,cap,bytes):e;
}

#ifndef FS_HOST
void owner_system_boot(void)
{
    struct storage *s=storage_get();
    struct storage_volume *v=s && s->ready && !s->setup_mode ? storage_volume(s,2) : 0;
    uint64_t volume=0;
    /* contract identity (sdk/private/provision.h ciuki_provision_volume_id):
     * the FAT32 partition's first LBA << 32 | its BPB volume serial. */
    if(v && v->present && !v->error && !v->optical && v->partition && v->fat.type==32 &&
       v->part.start && v->part.start<=UINT32_MAX && v->fat.volume_serial)
        volume=v->part.start<<32|v->fat.volume_serial;
    if(!s || !s->ready || s->setup_mode) return;
    (void)owner_system_attach(&s->vfs,2,volume,v ? v->fat.readonly : true);
}
#endif
