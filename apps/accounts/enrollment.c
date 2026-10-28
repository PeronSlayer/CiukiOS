/* SPDX-License-Identifier: MIT -- protected provisional enrollment catalogue */
#include "accounts.h"
#include "wire.h"
#include "../desktop/users.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <stdio.h>

#define ENROLL_BYTES (128u + 18u * 256u + 16u * 72u)
static int source(struct acct_service *s, uint8_t bytes[1400], size_t *n)
{
    uint8_t p[1416]; size_t m; uint64_t sequence=0;
    int e=acct_blob_read(&s->store,"enrollment.source",p,sizeof(p),&m,&sequence);
    if(!e && (m<16 || memcmp(p,"CIUKIEN0",8) || a32(p+8)!=m-16 || a32(p+12))) e=-EIO;
    if(!e) { *n=m-16; memcpy(bytes,p+16,*n); }
    acct_wipe(p,sizeof(p)); return e;
}
int acct_enrollment_source(struct acct_service *s, const void *v, size_t n)
{
    if(!s || !s->leased || s->available || n>1400 || (n && !v)) return -EPERM;
    struct acct_bootstrap b; int e=acct_boot_read(&s->store,&b); uint8_t sum[32]; sha256(v,n,sum);
    if(!e && (b.state!=1 || b.origin==ACCT_READY || memcmp(b.seed_digest,sum,32))) e=-EACCES;
    struct source_frame { struct users_db old; uint8_t p[1416], existing[1416]; };
    struct source_frame *f=acct_frame_alloc(&s->store.kernel,sizeof(*f)); if(!f) return -ENOMEM;
    if(!e && n && users_decode(&f->old,v,n)) e=-EIO;
    uint8_t *p=f->p; memcpy(p,"CIUKIEN0",8); w32(p+8,(uint32_t)n); if(n) memcpy(p+16,v,n);
    size_t m; uint64_t sequence=0;
    if(!e) { int found=acct_blob_read(&s->store,"enrollment.source",f->existing,sizeof(f->existing),&m,&sequence);
        if(!found) e=(m==n+16 && !memcmp(f->existing,p,m))?1:-EACCES; else if(found!=-ENOENT) e=found; }
    if(!e) e=acct_blob_write(&s->store,"enrollment.source",p,n+16,&sequence);
    if(e==1) e=0;
    acct_frame_free(&s->store.kernel,f,sizeof(*f)); return e;
}
static void encode_record(const struct acct_record *r, uint8_t *p)
{
    w32(p,r->uid); w32(p+4,r->flags); w32(p+8,r->groups); w32(p+12,r->kdf); w32(p+16,r->iterations); w32(p+20,r->legacy_id);
    memcpy(p+24,r->name,32); memcpy(p+56,r->salt,16); memcpy(p+72,r->verifier,32); memcpy(p+104,r->picture,96);
}
static int save(struct acct_service *s, int committed)
{
    struct acct_enrollment *en=&s->enrollment; size_t n; uint64_t sequence=0;
    struct save_frame { uint8_t p[ENROLL_BYTES], old[ENROLL_BYTES]; };
    struct save_frame *f=acct_frame_alloc(&s->store.kernel,sizeof(*f)); if(!f) return -ENOMEM;
    uint8_t *p=f->p;
    int e=acct_blob_read(&s->store,"enrollment.catalogue",f->old,sizeof(f->old),&n,&sequence); if(e==-ENOENT) e=0;
    memcpy(p,"CIUKIEN1",8); w64(p+8,en->generation); memcpy(p+16,en->plan_digest,32); w32(p+48,en->staged);
    w32(p+52,en->map_count); w32(p+56,en->draft.count); w32(p+60,en->draft.origin); w32(p+64,en->draft.next_uid);
    w32(p+68,en->draft.flags); memcpy(p+72,en->draft.sid,12); w32(p+84,(uint32_t)committed); w64(p+88,en->draft.generation);
    for(unsigned i=0;i<en->draft.count;++i) encode_record(&en->draft.records[i],p+128+256*i);
    for(unsigned i=0;i<en->map_count;++i) {
        uint8_t *b=p+128+18*256+72*i; w32(b,en->map[i].old_id); w32(b+4,en->map[i].uid); memcpy(b+8,en->map[i].root,64);
    }
    if(!e) e=acct_blob_write(&s->store,"enrollment.catalogue",p,ENROLL_BYTES,&sequence);
    acct_frame_free(&s->store.kernel,f,sizeof(*f)); return e;
}
int acct_enrollment_recover(struct acct_service *s)
{
    size_t n; uint64_t sequence=0;
    uint8_t *p=acct_frame_alloc(&s->store.kernel,ENROLL_BYTES); if(!p) return -ENOMEM;
    int e=acct_blob_read(&s->store,"enrollment.catalogue",p,ENROLL_BYTES,&n,&sequence);
    if(e) { acct_frame_free(&s->store.kernel,p,ENROLL_BYTES); return e==-ENOENT?0:e; }
    struct acct_enrollment *en=&s->enrollment;
    if(n!=ENROLL_BYTES || memcmp(p,"CIUKIEN1",8) || !a64(p+8) || a32(p+52)>16 || a32(p+56)<3 || a32(p+56)>18 ||
        a32(p+84)>1 || !azero(p+96,32)) { acct_frame_free(&s->store.kernel,p,ENROLL_BYTES); return -EIO; }
    en->generation=a64(p+8); memcpy(en->plan_digest,p+16,32); en->staged=a32(p+48); en->map_count=a32(p+52);
    en->draft.count=a32(p+56); en->draft.origin=a32(p+60); en->draft.next_uid=a32(p+64); en->draft.flags=a32(p+68);
    memcpy(en->draft.sid,p+72,12); en->draft.generation=a64(p+88);
    for(unsigned i=0;i<en->draft.count;++i) {
        const uint8_t *b=p+128+256*i; struct acct_record *r=&en->draft.records[i];
        r->uid=a32(b); r->flags=a32(b+4); r->groups=a32(b+8); r->kdf=a32(b+12); r->iterations=a32(b+16); r->legacy_id=a32(b+20);
        memcpy(r->name,b+24,32); memcpy(r->salt,b+56,16); memcpy(r->verifier,b+72,32); memcpy(r->picture,b+104,96);
        if(!apadded(r->name,32) || !acct_name_valid(r->name) || !azero(b+200,56) || (r->flags&ACCT_ENABLED)) e=-EIO;
    }
    for(unsigned i=0;i<en->map_count;++i) {
        const uint8_t *b=p+128+18*256+72*i; en->map[i].old_id=a32(b); en->map[i].uid=a32(b+4); memcpy(en->map[i].root,b+8,64);
        if(!acct_profile_root_valid(en->map[i].root) || (i && en->map[i-1].old_id>=en->map[i].old_id)) e=-EIO;
    }
    if(!e && s->available) {
        /* A completed database is authoritative after the batch's durable DONE,
         * even if interruption preceded the catalogue completion write. */
        if(s->db.origin!=en->draft.origin || memcmp(s->db.sid,en->draft.sid,12)) e=-EIO;
        else en->generation=0;
    } else if(!e) {
        e=acct_boot_identity(&s->store,&en->draft,0);
        if(!e) e=acct_store_load_watermarks(&s->store,en->draft.sid);
        if(!e && (s->store.high_generation<en->generation || s->store.high_uid<en->draft.next_uid)) e=-EIO;
    }
    acct_frame_free(&s->store.kernel,p,ENROLL_BYTES); return e;
}
static void public_plan(const struct acct_enrollment *en, struct acct_response *out)
{
    out->generation=en->generation; out->count=en->draft.count;
    for(unsigned i=0;i<out->count;++i) {
        out->records[i].uid=en->draft.records[i].uid; out->records[i].groups=en->draft.records[i].groups;
        out->records[i].flags=en->draft.records[i].groups==ACCT_ADMIN?2:0; memcpy(out->records[i].name,en->draft.records[i].name,32);
    }
}
struct plan_frame { uint8_t raw[1400]; struct acct_enrollment en; struct acct_import_options options; struct acct_import_map maps[16]; };
static int plan_inner(struct acct_service *s, struct acct_job *job, struct acct_response *out, struct plan_frame *f)
{
    struct acct_request *q=&job->request; uint8_t *raw=f->raw; size_t n=0;
    if(s->available) return -EPERM;
    if(!q->plan_count || q->plan_count>16) return -EINVAL;
    for(unsigned i=0;i<q->plan_count;++i) if(!apadded(q->plan[i].name,32) || !acct_name_valid(q->plan[i].name) ||
        (i && q->plan[i-1].old_id>=q->plan[i].old_id)) return -EINVAL;
    int e=s->store.kernel.enrollment_authorized(s->store.kernel.ctx,&job->subject); if(e) return e;
    e=source(s,raw,&n); if(e) return e;
    uint8_t canonical[32+40*16]={0},digest[32]; w32(canonical,1); w32(canonical+4,q->plan_count); w32(canonical+8,q->first_old_id);
    for(unsigned i=0;i<q->plan_count;++i) { w32(canonical+32+40*i,q->plan[i].old_id); memcpy(canonical+36+40*i,q->plan[i].name,32); }
    sha256(canonical,32+40*q->plan_count,digest);
    if(s->enrollment.generation && !memcmp(digest,s->enrollment.plan_digest,32)) { public_plan(&s->enrollment,out); acct_wipe(raw,sizeof(f->raw)); return 0; }
    if(q->generation!=s->enrollment.generation) { acct_wipe(raw,sizeof(f->raw)); return -EAGAIN; }
    struct acct_enrollment *en=&f->en;
    int have_records=0;
    if(n) { struct users_db old={0}; e=users_decode(&old,raw,n); if(!e) have_records=old.count!=0; acct_wipe(&old,sizeof(old)); }
    if(e) { acct_wipe(raw,sizeof(f->raw)); return -EIO; }
    if(have_records) {
        struct acct_import_options *options=&f->options; struct acct_import_map *maps=f->maps; unsigned count=0;
        options->first_legacy_id=q->first_old_id;
        for(unsigned i=0;i<q->plan_count;++i) memcpy(options->replacement_names[i],q->plan[i].name,32);
        e=acct_import_g32(raw,n,options,&en->draft,maps,&count);
        if(!e && count!=q->plan_count) e=-EINVAL;
        for(unsigned i=0;!e && i<count;++i) {
            if(maps[i].legacy_id!=q->plan[i].old_id) { e=-EINVAL; break; }
            en->map[i].old_id=maps[i].legacy_id; en->map[i].uid=maps[i].uid; memcpy(en->map[i].root,maps[i].root,64);
        }
        en->map_count=count; acct_wipe(options,sizeof(*options)); acct_wipe(maps,16*sizeof(*maps));
    } else {
        if(q->plan_count!=1 || q->first_old_id || q->plan[0].old_id || !acct_name_valid(q->plan[0].name) || acct_reserved_name(q->plan[0].name)) e=-EINVAL;
        en->draft.count=3; en->draft.origin=ACCT_SETUP; en->draft.next_uid=1001;
        en->draft.records[0]=(struct acct_record){.uid=500,.flags=ACCT_BUILTIN,.groups=ACCT_ADMIN,.name="CiukiAdministrator"};
        en->draft.records[1]=(struct acct_record){.uid=501,.flags=ACCT_BUILTIN,.groups=ACCT_GUEST,.name="Guest"};
        en->draft.records[2]=(struct acct_record){.uid=1000,.groups=ACCT_ADMIN}; memcpy(en->draft.records[2].name,q->plan[0].name,32);
    }
    acct_wipe(raw,sizeof(f->raw));
    if(!e) {
        struct acct_bootstrap boot; e=acct_boot_read(&s->store,&boot);
        if(!e) { en->draft.origin=boot.origin; en->draft.flags=boot.origin==ACCT_UPGRADE?ACCT_MIGRATED:0; }
    }
    if(!e) e=acct_boot_identity(&s->store,&en->draft,1);
    if(!e && s->store.high_generation) {
        uint32_t base=s->store.high_uid;
        if(base>ACCT_LAST_UID-(en->draft.count-3)) e=-ENOSPC;
        else {
            for(unsigned i=2;i<en->draft.count;++i) en->draft.records[i].uid=base+i-2;
            for(unsigned i=0;i<en->map_count;++i) en->map[i].uid=base+(en->map[i].uid-1000);
            en->draft.next_uid=base+en->draft.count-2;
        }
    }
    if(!e && s->store.high_generation==UINT64_MAX) e=-EOVERFLOW;
    if(!e) { en->generation=s->store.high_generation+1; en->draft.generation=en->generation;
        e=acct_store_reserve_generation(&s->store,en->draft.sid,en->generation,en->draft.next_uid); }
    if(!e) {
        memcpy(en->plan_digest,digest,32); s->enrollment=*en; acct_attempts_clear(s); e=save(s,0);
        if(!e) public_plan(&s->enrollment,out); else s->faulted=1;
    }
    return e;
}
static int plan(struct acct_service *s, struct acct_job *job, struct acct_response *out)
{
    struct plan_frame *f=acct_frame_alloc(&s->store.kernel,sizeof(*f)); if(!f) return -ENOMEM;
    int e=plan_inner(s,job,out,f);
    acct_frame_free(&s->store.kernel,f,sizeof(*f)); return e;
}
struct execute_frame { uint8_t raw[1400]; struct users_db original; struct acct_record verifier, old; struct acct_db next, final; struct acct_owner_request owner; };
static int execute_inner(struct acct_service *s, struct acct_job *job, struct acct_response *out, struct execute_frame *f)
{
    if(job->request.operation==ACCT_ENROLL_PLAN) return plan(s,job,out);
    int e=s->store.kernel.enrollment_authorized(s->store.kernel.ctx,&job->subject); if(e) return e;
    struct acct_request *q=&job->request; struct acct_enrollment *en=&s->enrollment;
    struct acct_db *d=en->generation?&en->draft:&s->db; unsigned index=0;
    while(index<d->count && d->records[index].uid!=q->uid) ++index;
    if(index==d->count || index==1 || !acct_password_valid(q->password,0)) return -EINVAL;
    struct acct_record *r=&d->records[index];
    if(en->generation && r->legacy_id) {
        /* A resumed/repeated stage proves the original protected G32 verifier,
         * never the replacement verifier from an earlier successful stage. */
        uint8_t *raw=f->raw; size_t n; struct users_db *original=&f->original; struct acct_record *verifier=&f->verifier;
        e=source(s,raw,&n); if(!e) e=users_decode(original,raw,n);
        unsigned i=0; while(i<original->count && original->records[i].id!=r->legacy_id) ++i;
        if(!e && i==original->count) e=-EIO;
        if(!e) { verifier->flags=ACCT_PASSWORD_SET; memcpy(verifier->salt,original->records[i].salt,16); memcpy(verifier->verifier,original->records[i].verifier,32);
            if(!acct_verify(verifier,q->old_password)) e=-EACCES; }
        acct_wipe(raw,sizeof(f->raw)); acct_wipe(original,sizeof(*original)); acct_wipe(verifier,sizeof(*verifier));
        if(e) return e;
    } else if(r->legacy_id && (r->flags&ACCT_CHANGE_REQUIRED)) {
        if(!acct_verify(r,q->old_password)) return -EACCES;
    } else if(q->old_password[0] || (!en->generation && !(r->flags&ACCT_CHANGE_REQUIRED))) return -EACCES;
    if(!en->generation) {
        struct acct_db *next=&f->next; *next=s->db; next->generation=s->store.high_generation+1;
        e=acct_set_password(s,&next->records[index],q->password);
        if(!e) { s->store.subject=&job->subject; e=acct_store_local_commit(&s->store,&s->db,next); s->store.subject=NULL;
            if(e) { s->available=0; s->faulted=1; } }
        if(!e) s->db=*next;
        acct_wipe(next,sizeof(*next));
        return e?e:acct_auth_result(s,job,out,0,3,NULL);
    }
    struct acct_record *old=&f->old; *old=*r; e=acct_set_password(s,r,q->password);
    if(!e) { en->staged|=1u<<index; e=save(s,0); }
    if(e) { s->faulted=1; *r=*old; acct_wipe(old,sizeof(*old)); return e; }
    acct_wipe(old,sizeof(*old));
    unsigned required=((1u<<d->count)-1)&~2u;
    if((en->staged&required)!=required) return acct_auth_result(s,job,out,0,2,NULL);
    struct acct_db *final=&f->final; *final=*d; final->generation=s->store.high_generation+1;
    for(unsigned i=2;i<final->count;++i) final->records[i].flags|=ACCT_ENABLED;
    struct acct_owner_request *owner=&f->owner; memset(owner,0,sizeof(*owner)); owner->volume=s->store.volume; owner->transaction=final->generation; owner->count=final->count; memcpy(owner->sid,final->sid,12);
    for(unsigned i=0;i<final->count;++i) {
        struct acct_owner_binding *b=&owner->bindings[i]; b->uid=final->records[i].uid; b->action=1; b->volume=s->store.volume;
        snprintf(b->root,64,"/Windows/Profiles/u%08x",b->uid);
        for(unsigned j=0;j<en->map_count;++j) if(en->map[j].uid==b->uid) memcpy(b->root,en->map[j].root,64);
    }
    e=acct_store_commit(&s->store,NULL,final,owner,1);
    if(e) { s->available=0; s->faulted=1; }
    if(!e) { s->db=*final; s->available=1; e=acct_results_open(s); }
    if(!e) e=save(s,1);
    if(!e) { en->generation=0; e=acct_auth_result(s,job,out,0,3,NULL); }
    acct_wipe(final,sizeof(*final)); return e;
}
/* --- sec-user-04: AUTH ENROLL_SOURCE (contract amendment "G32 enrollment
 * source"). The trusted enrollment UI must submit every protected G32
 * record exactly (A2-1); this read-only LOGON route lists them: old ids in
 * ascending order and names, a replacement-label flag for reserved, invalid
 * or duplicate names, and, once a plan is staged, its generation, the uid
 * each record received and the staged mask. Never salts or verifiers. An
 * installation that is not waiting for enrollment answers ENOENT. --- */
struct source_frame { uint8_t raw[1400]; struct users_db old; };
static int source_inner(struct acct_service *s, struct acct_response *out, struct source_frame *f)
{
    if(s->available) return -ENOENT;
    size_t n=0; int e=source(s,f->raw,&n);
    if(e) return e;
    if(n && users_decode(&f->old,f->raw,n)) return -EIO;
    if(!n) f->old.count=0;
    if(f->old.count>16) return -EIO;
    for(unsigned i=1;i<f->old.count;++i) {
        struct users_record record=f->old.records[i]; unsigned j=i;
        while(j && f->old.records[j-1].id>record.id) { f->old.records[j]=f->old.records[j-1]; --j; }
        f->old.records[j]=record; acct_wipe(&record,sizeof(record));
    }
    const struct acct_enrollment *en=&s->enrollment;
    out->count=f->old.count; out->generation=en->generation;
    out->change_required=en->generation?1u:0u; out->authenticated_uid=en->generation?en->staged:0u;
    for(unsigned i=0;i<f->old.count;++i) {
        const struct users_record *r=&f->old.records[i]; struct acct_public *p=&out->records[i];
        if(i && f->old.records[i-1].id==r->id) return -EIO;
        p->uid=r->id; memcpy(p->name,r->name,31); p->name[31]=0;
        if(!p->name[0]) return -EIO;
        int replace=!acct_name_valid(p->name) || acct_reserved_name(p->name);
        for(unsigned j=0;j<i;++j) if(acct_name_equal(out->records[j].name,p->name)) replace=1;
        p->flags=replace?1u:0u;
        for(unsigned j=0;en->generation && j<en->map_count;++j) if(en->map[j].old_id==r->id) p->groups=en->map[j].uid;
    }
    return 0;
}
static int source_list(struct acct_service *s, struct acct_response *out)
{
    struct source_frame *f=acct_frame_alloc(&s->store.kernel,sizeof(*f)); if(!f) return -ENOMEM;
    int e=source_inner(s,out,f);
    if(e) { out->count=0; out->generation=0; out->change_required=0; out->authenticated_uid=0; }
    acct_frame_free(&s->store.kernel,f,sizeof(*f)); return e;
}
int acct_source_encode(const struct acct_response *r, void *v, size_t cap, size_t *n)
{
    if(!r || !v || !n || r->count>16 || cap<32u+48u*r->count) return -EINVAL;
    uint8_t *p=v; *n=32u+48u*r->count; memset(p,0,*n);
    /* acct_work fills a zero completion generation with the database's:
     * without a plan the header generation stays zero. */
    w32(p,1); w32(p+4,r->count); w32(p+8,r->change_required&1u);
    w32(p+12,(r->change_required&1u)?r->authenticated_uid:0); w64(p+16,(r->change_required&1u)?r->generation:0);
    for(unsigned i=0;i<r->count;++i) {
        uint8_t *b=p+32+48*i; w32(b,r->records[i].uid); w32(b+4,r->records[i].flags); w32(b+8,r->records[i].groups);
        memcpy(b+16,r->records[i].name,32);
    }
    return 0;
}
/* --- end sec-user-04 --- */
int acct_enrollment_execute(struct acct_service *s, struct acct_job *job, struct acct_response *out)
{
    if(job->request.operation==ACCT_ENROLL_SOURCE) return source_list(s,out); /* sec-user-04 */
    if(job->request.operation==ACCT_ENROLL_PLAN) return plan(s,job,out);
    struct execute_frame *f=acct_frame_alloc(&s->store.kernel,sizeof(*f)); if(!f) return -ENOMEM;
    int e=execute_inner(s,job,out,f);
    acct_frame_free(&s->store.kernel,f,sizeof(*f)); return e;
}
