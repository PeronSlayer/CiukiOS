/* SPDX-License-Identifier: MIT -- round-2 production service acceptance */
#include "fake.h"
#include "wire.h"
#include "users.h"
#include <ciuki/sha256.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void restart(struct fake *f, struct acct_service *s)
{
    acct_service_close(s); fake_powercut(f); struct acct_store st=fake_store(f);
    int e=acct_service_open(s,&st,1); if(e) fprintf(stderr,"restart error %d\n",e); assert(!e && s->available);
}
static void set_password(struct acct_service *s)
{
    struct acct_request q; struct acct_response r;
    fake_request(&q,ACCT_PASSWORD,s->db.generation,1000,NULL,"password1"); assert(!fake_run(s,1,&q,&r));
}
static void status(struct acct_service *s, int fd, uint64_t id, struct acct_response *r)
{
    struct acct_request q={.operation=ACCT_STATUS,.client_operation_id=id}; assert(!fake_run(s,fd,&q,r));
}
void round2_results(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); set_password(&s);
    struct acct_request q,rsv,copy; struct acct_response r;
    fake_request(&q,ACCT_PASSWORD,s.db.generation,1000,NULL,"password1"); strcpy(q.old_password,"password1");
    assert(!fake_reserve(&s,1,&q)); copy=q; assert(!fake_run(&s,1,&q,&r)); unsigned entropy=f.entropy_calls;
    assert(!fake_run(&s,1,&copy,&r) && f.entropy_calls==entropy);
    fake_request(&q,ACCT_PICTURE,s.db.generation,1000,NULL,NULL); strcpy(q.picture,"catalogue/ciuki.png");
    assert(!fake_reserve(&s,1,&q)); uint64_t id=q.client_operation_id; copy=q;
    status(&s,1,id,&r); assert(a32(r.body+8)==1);
    assert(!fake_run(&s,1,&q,&r)); uint64_t committed=s.db.generation; unsigned owners=f.owner_used;
    assert(!fake_run(&s,1,&copy,&r) && s.db.generation==committed && r.generation==committed);
    status(&s,1,id,&r); assert(a32(r.body+8)==3 && a64(r.body+40)==committed);
    rsv=(struct acct_request){.operation=ACCT_STATUS,.client_operation_id=id}; assert(fake_run(&s,2,&rsv,&r)==-EACCES);
    copy.picture[0]=0; assert(fake_run(&s,1,&copy,&r)==-EINVAL); /* changed content, same id */
    ++f.peers[1].epoch; ++f.peers[1].subject.endpoint_generation; f.peers[1].subject.session_id=17;
    restart(&f,&s); status(&s,1,id,&r); assert(a32(r.body+8)==3);
    assert(!fake_run(&s,1,&q,&r) && s.db.generation==committed); /* exact retry has no stale-generation reexecution */
    uint64_t earliest=id;
    for(unsigned i=0;i<2055;++i) {
        fake_request(&q,ACCT_PICTURE,s.db.generation,1000,NULL,NULL);
        if(i&1) strcpy(q.picture,"catalogue/ciuki.png");
        assert(!fake_run(&s,1,&q,&r));
        if(i%17==0) {
            fake_request(&q,ACCT_PICTURE,s.db.generation,1000,NULL,NULL); strcpy(q.picture,"../denied");
            assert(fake_run(&s,1,&q,&r)==-EINVAL);
        }
        if(i%257==0) {
            fake_request(&q,ACCT_PASSWORD,s.db.generation,1000,NULL,"password1"); strcpy(q.old_password,"password1");
            assert(!fake_run(&s,1,&q,&r));
        }
    }
    assert(f.owner_used==owners && s.results->count<768 && s.results->floor>=earliest);
    status(&s,2,earliest,&r); assert(a32(r.body+8)==5 && a64(r.body+32)==earliest && !a32(r.body+20));
    copy.client_operation_id=earliest; assert(fake_run(&s,1,&copy,&r)==-EAGAIN);
    restart(&f,&s); status(&s,1,earliest,&r); assert(a32(r.body+8)==5);
    uint64_t high=s.results->high;
    fake_request(&q,ACCT_PICTURE,s.db.generation,1000,NULL,NULL); assert(!fake_run(&s,1,&q,&r) && q.client_operation_id>high);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS durable caller-owned RESERVE/STATUS, stale exact retry, reconnect, digest/uid denial, EXPIRED and >2048 metadata/password updates without OWNER growth");
}
static void stage_reserve(struct fake *f, struct acct_service *s, uint64_t g, struct acct_owner_request *q)
{
    unsigned count; assert(!acct_owner_requests(&s->store,&s->db,q,&count)); q->transaction=g;
    q->expected_accounts_generation=s->db.generation;
    if(f->reserve_create) {
        struct acct_owner_binding *b=&q->bindings[q->count++]; memset(b,0,sizeof(*b));
        b->uid=s->store.high_uid-1; b->action=1; b->volume=s->store.volume;
        snprintf(b->root,64,"/Windows/Profiles/u%08x",b->uid);
    }
    uint8_t db[ACCT_FILE_MAX], manifest[ACCT_MANIFEST_MAX],wire[ACCT_OWNER_BODY_MAX]; size_t n,m;
    assert(!acct_encode(&s->db,0,db,sizeof(db),&n)); sha256(db,n,q->target_digest);
    assert(!s->store.owner.stage(f,q,manifest,sizeof(manifest),&n)); assert(!acct_manifest_validate(q,manifest,n));
    char path[128]; snprintf(path,sizeof(path),"/System/Security/Staging/g%016llx/manifest",(unsigned long long)g);
    fake_put(f,path,manifest,n); sha256(manifest,n,q->manifest_digest);
    assert(!acct_owner_encode(q,7,wire,sizeof(wire),&m)); memcpy(q->digest,wire+128,32);
}
static void delete_manifest(struct fake *f, struct acct_service *s, uint64_t g)
{
    char path[128]; snprintf(path,sizeof(path),"/System/Security/Staging/g%016llx/manifest",(unsigned long long)g);
    assert(!s->store.io.remove(f,path)); assert(!s->store.io.barrier(f));
}
void round2_owner(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
    struct acct_owner_request first={0},q; struct acct_owner_reply r;
    uint64_t key=f.key_high,locator=f.locator_high; f.reserve_create=1;
    for(unsigned i=0;i<2055;++i) {
        uint64_t g=s.store.high_generation+1;
        assert(!acct_store_reserve_generation(&s.store,s.db.sid,g,s.store.high_uid+1));
        stage_reserve(&f,&s,g,&q);
        assert(!acct_owner_call(&s.store,7,&q,&r) && r.state==5);
        unsigned used=f.owner_used;
        assert(!acct_owner_call(&s.store,7,&q,&r) && f.owner_used==used);
        assert(!acct_owner_call(&s.store,3,&q,&r) && r.state==3 && f.owner_used==used);
        if(!i) first=q;
        delete_manifest(&f,&s,g);
        if(f.owner_used>=768) assert(!acct_owner_checkpoint(&s.store,&s.db,0));
    }
    assert(f.owner_used<768 && f.owner_floor>first.transaction && f.key_high==key+2055 && f.locator_high==locator+2055);
    first.expected_generation=12345; /* floor wins even over changed old content */
    uint8_t wire[ACCT_OWNER_BODY_MAX]; size_t bytes; assert(!acct_owner_encode(&first,4,wire,sizeof(wire),&bytes)); memcpy(first.digest,wire+128,32);
    assert(!acct_owner_call(&s.store,4,&first,&r) && r.state==4 && !r.count);
    assert(!acct_owner_call(&s.store,7,&first,&r) && r.state==4); assert(acct_owner_call(&s.store,2,&first,&r)==-EIO);
    restart(&f,&s); assert(f.owner_used<768 && f.profile_count==3);
    f.reserve_create=0;
    set_password(&s); /* still admits ordinary management after receipt turnover */
    uint64_t g=s.store.high_generation+1; assert(!acct_store_reserve_generation(&s.store,s.db.sid,g,s.db.next_uid));
    stage_reserve(&f,&s,g,&q); f.replace_root=1; assert(acct_owner_call(&s.store,7,&q,&r)==-EIO); f.replace_root=0;
    q.bindings[0].locator=1; assert(acct_owner_encode(&q,7,wire,sizeof(wire),&bytes)==-EINVAL);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS >2048 RESERVED-to-ABORTED batches count once, proactive checkpoint/compaction, permanent floors and no locator input");
}
void round2_profile(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); set_password(&s);
    struct acct_request q; struct acct_response r;
    for(unsigned i=0;i<8;++i) {
        char name[32]; snprintf(name,32,"User%u",i);
        fake_request(&q,ACCT_CREATE,s.db.generation,0,name,"password1"); assert(!fake_run(&s,1,&q,&r));
    }
    for(unsigned i=0;i<f.profile_count;++i) if(f.profiles[i].uid==1007) strcpy(f.profiles[i].root,"/Windows/Profiles/u00000003");
    fake_request(&q,ACCT_PROFILE,s.db.generation,1007,NULL,NULL); assert(!fake_run(&s,1,&q,&r));
    assert(r.body_bytes==272 && a32(r.body+8)==1007 && !strcmp((char *)r.body+32,"/Windows/Profiles/u00000003") &&
        !strcmp((char *)r.body+96,"/Windows/Profiles/u00000003/Documents") && !strcmp((char *)r.body+192,"/Programs/terminal"));
    assert(fake_run(&s,2,&q,&r)==-EACCES && fake_run(&s,3,&q,&r)==-EACCES);
    f.peers[4].subject.role=1; f.peers[4].subject.uid=0; assert(!fake_run(&s,4,&q,&r));
    f.peers[2].subject.uid=1007; assert(!fake_run(&s,2,&q,&r));
    --q.generation; assert(fake_run(&s,2,&q,&r)==-EAGAIN); ++q.generation;
    f.wrong_owner=1; assert(fake_run(&s,2,&q,&r)==-EIO); f.wrong_owner=0;
    for(unsigned i=0;i<f.profile_count;++i) if(f.profiles[i].uid==1007) f.profiles[i].action=2;
    assert(fake_run(&s,2,&q,&r)==-EACCES);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS authorized 272-byte PROFILE, retained mismatched numeric root, home/shell, generation and retired/corrupt binding denial");
}
void round2_autologon(void)
{
    for(unsigned kind=0;kind<5;++kind) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
        if(kind==1) { f.logon_generation=2; f.peers[0].subject.role_generation=2; }
        if(kind==2) f.account_generation=2;
        if(kind==3) f.peers[0].subject.safe_mode=1;
        if(kind==4) set_password(&s);
        struct acct_request q; struct acct_response r; fake_request(&q,ACCT_AUTOLOGON,s.db.generation,1000,NULL,NULL);
        int e=fake_run(&s,0,&q,&r);
        if(!kind) { assert(!e && a32(f.last_outcome+4)==5 && r.body_bytes==16); }
        else assert(e==-EACCES);
        f.now+=1000; fake_request(&q,ACCT_AUTOLOGON,s.db.generation,1000,NULL,NULL); assert(fake_run(&s,0,&q,&r)==-EACCES);
        acct_service_close(&s); fake_destroy(&f);
    }
    puts("PASS LOGON-only one-shot ready autologon, consumed state/password retirement, Safe Mode and either role restart suppress it");
}
static void enrollment_boot(struct fake *f, struct acct_service *s, const void *source, size_t n)
{
    struct acct_bootstrap b={.state=1,.origin=3,.volume=1234,.next_uid=1001,.generation=1};
    uint8_t bytes[160]; sha256(source,n,b.seed_digest); assert(!acct_boot_encode(&b,bytes,160)); fake_put(f,"bootstrap.state",bytes,160);
    struct acct_store store=fake_store(f); assert(!acct_service_open(s,&store,1) && !s->available);
    assert(!acct_enrollment_source(s,source,n));
}
void round2_enrollment(void)
{
    for(unsigned source_kind=0;source_kind<3;++source_kind) {
        unsigned imported=source_kind==1;
        struct fake f; fake_init(&f); struct acct_service s; uint8_t bytes[1400]; size_t n=0;
        if(imported) {
            struct users_db old={.count=2,.next_id=9};
            old.records[0].id=3; strcpy(old.records[0].name,"OldOne"); old.records[1].id=8; strcpy(old.records[1].name,"OldTwo");
            users_derive("abc",old.records[0].salt,10000,old.records[0].verifier); users_derive("",old.records[1].salt,10000,old.records[1].verifier);
            n=users_encode(&old,bytes,sizeof(bytes)); assert(n);
        }
        if(source_kind==2) { struct users_db empty={.next_id=1}; n=users_encode(&empty,bytes,sizeof(bytes)); assert(n); }
        enrollment_boot(&f,&s,bytes,n);
        struct acct_request q={.operation=ACCT_ENROLL_PLAN,.service=1,.wire_operation=7,.plan_count=imported?2:1,.first_old_id=imported?8:0};
        q.plan[0].old_id=imported?3:0; strcpy(q.plan[0].name,imported?"OldOne":"Alice");
        if(imported) { q.plan[1].old_id=8; strcpy(q.plan[1].name,"OldTwo"); }
        struct acct_response r; assert(!acct_submit(&s,0,&q,sizeof(q),&r)); assert(acct_work(&s,&r)==1 && !r.error && r.count==(imported?4u:3u));
        uint64_t generation=s.enrollment.generation; assert(!acct_submit(&s,0,&q,sizeof(q),&r)); assert(acct_work(&s,&r)==1 && !r.error && r.generation==generation);
        if(!imported) {
            uint32_t retired=s.enrollment.draft.records[2].uid; q.generation=generation; strcpy(q.plan[0].name,"Renamed");
            assert(!acct_submit(&s,0,&q,sizeof(q),&r)); assert(acct_work(&s,&r)==1 && !r.error);
            assert(s.enrollment.draft.records[2].uid>retired && s.enrollment.generation>generation); generation=s.enrollment.generation;
        }
        acct_service_close(&s); fake_powercut(&f); struct acct_store st=fake_store(&f); assert(!acct_service_open(&s,&st,1) && s.enrollment.generation==generation);
        if(imported) { f.enrollment_count=s.enrollment.map_count; for(unsigned i=0;i<f.enrollment_count;++i) {
            f.enrollment[i].legacy_id=s.enrollment.map[i].old_id; f.enrollment[i].uid=s.enrollment.map[i].uid; memcpy(f.enrollment[i].root,s.enrollment.map[i].root,64); } }
        unsigned count=s.enrollment.draft.count;
        for(unsigned i=0;i<count;++i) if(i!=1) {
            uint32_t uid=s.enrollment.draft.records[i].uid;
            fake_request(&q,ACCT_ENROLL_VERIFY,s.enrollment.generation,uid,NULL,"newpass12");
            if(imported && s.enrollment.draft.records[i].legacy_id==3) strcpy(q.old_password,"abc");
            if(i==0) {
                struct acct_request bad=q; strcpy(bad.password,"short"); assert(fake_run(&s,0,&bad,&r)==-EINVAL); f.now+=1000;
            }
            assert(!fake_run(&s,0,&q,&r) && r.body_bytes==16 && a32(r.body+4)==(i==count-1?3u:2u));
            assert(a32(f.last_outcome+68)==(i==count-1?3u:2u));
            assert(!a32(f.last_outcome+96)); /* neither enrollment disposition exports credentials */
        }
        assert(s.available && s.db.count==count && f.profile_count==count);
        fake_request(&q,ACCT_AUTH,s.db.generation,s.db.records[2].uid,NULL,"newpass12"); assert(!fake_run(&s,0,&q,&r) && a32(f.last_outcome+68)==1);
        restart(&f,&s); assert(!s.enrollment.generation && s.available);
        acct_service_close(&s); fake_destroy(&f);
    }
    puts("PASS durable ENROLL_PLAN exact retry/restart, fresh and G32 short/blank enrollment, staged/final outcomes without sessions and fresh LOGON requirement");
}

static void clone_persistent(struct fake *out, const struct fake *in)
{
    fake_init(out); out->owner_used=in->owner_used; out->owner_high=in->owner_high; out->key_high=in->key_high;
    out->locator_high=in->locator_high; out->owner_generation=in->owner_generation; out->owner_floor=in->owner_floor;
    out->checkpoint_candidate=in->checkpoint_candidate; out->checkpoint_floor_candidate=in->checkpoint_floor_candidate;
    out->profile_count=in->profile_count; memcpy(out->profiles,in->profiles,sizeof(out->profiles));
    out->enrollment_count=in->enrollment_count; memcpy(out->enrollment,in->enrollment,sizeof(out->enrollment)); out->move=in->move;
    memcpy(out->owners,in->owners,FAKE_OWNERS*sizeof(*in->owners));
    for(unsigned i=0;i<FAKE_FILES;++i) if(in->files[i].durable_name[0])
        fake_put(out,in->files[i].durable_name,in->files[i].durable_data,in->files[i].durable_size);
}
void round2_checkpoint_faults(void)
{
    struct fake baseline; fake_init(&baseline); struct acct_service s; fake_boot(&baseline,&s);
    for(unsigned i=1;i<768;++i) {
        uint64_t g=s.store.high_generation+1; assert(!acct_store_reserve_generation(&s.store,s.db.sid,g,s.db.next_uid));
        struct acct_owner_request q; struct acct_owner_reply r; stage_reserve(&baseline,&s,g,&q);
        assert(!acct_owner_call(&s.store,7,&q,&r)); assert(!acct_owner_call(&s.store,3,&q,&r)); delete_manifest(&baseline,&s,g);
    }
    acct_service_close(&s);
    unsigned boundaries=0;
    for(unsigned cut=0;cut<=boundaries;++cut) {
        struct fake f; clone_persistent(&f,&baseline); struct acct_store st=fake_store(&f); assert(!acct_service_open(&s,&st,1));
        f.event=0; f.cut=cut; int e=acct_owner_checkpoint(&s.store,&s.db,0);
        if(!cut) { assert(!e); boundaries=f.event; } else assert(e==-EIO);
        acct_service_close(&s); fake_powercut(&f); assert(!acct_service_open(&s,&st,1));
        /* Regression 3-2: mutate before retrying compaction. A lost OWNER
         * CHECKPOINT reply must not strand a candidate with the old DB hash. */
        set_password(&s);
        assert(!acct_owner_checkpoint(&s.store,&s.db,0)); assert(f.owner_used==256 && f.owner_floor);
        struct acct_request create; struct acct_response created;
        fake_request(&create,ACCT_CREATE,s.db.generation,0,"AfterCheckpoint","password1");
        assert(!fake_run(&s,1,&create,&created) && s.available);
        acct_service_close(&s); fake_destroy(&f);
    }
    fake_destroy(&baseline);
    printf("PASS OWNER checkpoint every %u snapshot/ACCOUNT-marker/floor/trim cut recovers and later password management succeeds\n",boundaries);
    fake_init(&baseline); fake_boot(&baseline,&s); set_password(&s);
    /* Fill ACCOUNT just before proactive checkpoint, including ABORTED results. */
    for(unsigned i=0;i<767;++i) {
        struct acct_request q; struct acct_response r;
        fake_request(&q,ACCT_PICTURE,s.db.generation,1000,NULL,NULL); assert(!fake_run(&s,1,&q,&r));
    }
    acct_service_close(&s); boundaries=0;
    for(unsigned cut=0;cut<=boundaries;++cut) {
        struct fake f; clone_persistent(&f,&baseline); struct acct_store st=fake_store(&f); assert(!acct_service_open(&s,&st,1));
        f.event=0; f.cut=cut; struct acct_request q; struct acct_response r;
        fake_request(&q,ACCT_PICTURE,s.db.generation,1000,NULL,NULL); int e=fake_run(&s,1,&q,&r);
        if(!cut) { assert(!e); boundaries=f.event; } else assert(e==-EIO);
        acct_service_close(&s); fake_powercut(&f); assert(!acct_service_open(&s,&st,1));
        uint64_t high=s.results->high; fake_request(&q,ACCT_PICTURE,s.db.generation,1000,NULL,NULL);
        assert(!fake_run(&s,1,&q,&r) && q.client_operation_id>high); acct_service_close(&s); fake_destroy(&f);
    }
    fake_destroy(&baseline);
    printf("PASS ACCOUNT result checkpoint every %u watermark/candidate/publication/local-intent cut, nonreuse and later management\n",boundaries);
}
void round2_cancellation(void)
{
    struct fake before; fake_init(&before); struct acct_store before_store=fake_store(&before);
    before.peers[1].queued_request=55;
    struct acct_security_link cancel={.version=1,.size=64,.service=2,.endpoint_fd=1,.endpoint_generation=2,.request_id_lo=55};
    assert(before_store.kernel.service_link(&before,14,&cancel,NULL)==1 && !before.peers[1].queued_request);
    before.peers[1].active_request=55; assert(!before_store.kernel.service_link(&before,14,&cancel,NULL)); before.peers[1].active_request=0;
    fake_destroy(&before);
    for(unsigned late=0;late<2;++late) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); set_password(&s);
        struct acct_request q; struct acct_response r; fake_request(&q,ACCT_CREATE,s.db.generation,0,"LostReply","password1");
        assert(!fake_reserve(&s,1,&q)); uint64_t id=q.client_operation_id;
        f.cancel_on_prepare=!late; f.revoke_after_commit=late;
        assert(fake_run(&s,1,&q,&r)==-ESRCH);
        f.cancel_on_prepare=f.revoke_after_commit=0; ++f.peers[1].epoch; f.peers[1].alive=1; ++f.peers[1].subject.endpoint_generation;
        restart(&f,&s); status(&s,1,id,&r); assert(a32(r.body+8)==(late?3u:4u));
        int recorded=(int32_t)a32(r.body+16);
        uint64_t gen=s.db.generation; unsigned count=s.db.count,owners=f.owner_used;
        fake_request(&q,ACCT_CREATE,2,0,"LostReply","password1"); q.client_operation_id=id; q.wire_operation=3;
        assert(fake_run(&s,1,&q,&r)==recorded);
        assert(s.db.generation==gen && s.db.count==count && f.owner_used==owners);
        acct_service_close(&s); fake_destroy(&f);
    }
    puts("PASS cancel before/after COMMIT_REQUIRED, durable caller STATUS after lost reply/reconnect and exact retry never reallocates");
}
void round2_logon_relay(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
    struct acct_logon l={.kernel=s.store.kernel};
    f.peers[4].subject=(struct acct_subject){.role=2,.service=5,.group_count=1,.endpoint_generation=41,.service_generation=1,.role_generation=1};
    f.peers[5].subject=f.peers[4].subject; f.peers[5].subject.endpoint_generation=51;
    f.peers[6].subject=(struct acct_subject){.role=3,.service=1,.group_count=1,.endpoint_generation=61,.service_generation=1,.role_generation=1};
    struct acct_subject ui,ui2,account;
    assert(!l.kernel.retain_peer(&f,4,&ui)); assert(!l.kernel.retain_peer(&f,5,&ui2)); assert(!l.kernel.retain_peer(&f,6,&account));
    struct acct_begin b={.action=2,.selector=1,.uid=1000}; uint8_t begin[64],relay[272],challenge[64],submit[208]={0};
    assert(!acct_begin_encode(&b,0,begin,64)); assert(!acct_logon_begin(&l,&ui,begin,64,relay));
    struct acct_challenge c; assert(!acct_challenge_decode(&c,1,relay+64,64)); c.uid=1000; c.accounts_generation=s.db.generation;
    assert(!acct_challenge_encode(&c,0,challenge,64)); assert(!acct_logon_challenged(&l,&account,challenge,64));
    w32(submit,1); w64(submit+8,c.attempt); w32(submit+20,c.console); w32(submit+36,1000); w64(submit+40,s.db.generation);
    assert(acct_logon_submit(&l,&ui2,submit,208,relay)==-EAGAIN); assert(!acct_logon_submit(&l,&ui,submit,208,relay));
    f.attempt_endpoint_generation=41; struct acct_response r; struct acct_request q={0}; q.service=1;
    assert(acct_body_decode(1,5,(uint8_t[128]){0},128,0,&q)==-EINVAL); /* malformed proposal is never admitted */
    /* Use the actual ACCOUNT server to resolve/consume the relayed request. */
    struct acct_request challenge_q={.operation=ACCT_CHALLENGE,.service=1,.begin=b}; challenge_q.challenge=c;
    challenge_q.challenge.uid=0; challenge_q.challenge.accounts_generation=0;
    assert(!acct_submit(&s,0,&challenge_q,sizeof(challenge_q),&r)); assert(acct_work(&s,&r)==1 && !r.error);
    assert(!acct_submit_body(&s,0,1,2,relay,272,s.db.generation,0,&r)); assert(acct_work(&s,&r)==1 && !r.error);
    account.service=5; struct acct_auth_outcome outcome; uint8_t progress[16];
    assert(!acct_logon_outcome(&l,&account,s.db.generation,f.last_outcome,304,&outcome,progress));
    assert(outcome.disposition==1 && a32(progress+4)==1);
    assert(acct_logon_outcome(&l,&account,s.db.generation,f.last_outcome,304,&outcome,progress)==-EAGAIN);
    /* A late failure of a spent attempt cannot consume another UI's attempt. */
    assert(!acct_logon_begin(&l,&ui2,begin,64,relay)); uint64_t second=a64(relay+64+8);
    assert(acct_logon_outcome(&l,&account,s.db.generation,f.last_outcome,304,&outcome,progress)==-EAGAIN);
    int retained=0; for(unsigned i=0;i<8;++i) retained|=l.attempts[i].occupied && l.attempts[i].challenge.attempt==second; assert(retained);
    acct_logon_clear(&l); l.kernel.release_peer(&f,&ui); l.kernel.release_peer(&f,&ui2); l.kernel.release_peer(&f,&account);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS production LOGON BEGIN/selector/challenge/SUBMIT relay/outcome correlation, two UI links, replay and late failure isolation");
}
void round2_manifest(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
    struct acct_owner_request q; unsigned count; assert(!acct_owner_requests(&s.store,&s.db,&q,&count));
    q.transaction=s.store.high_generation+1; q.expected_accounts_generation=s.db.generation;
    uint8_t manifest[ACCT_MANIFEST_MAX]; size_t n; assert(!s.store.owner.stage(&f,&q,manifest,sizeof(manifest),&n));
    assert(!acct_manifest_validate(&q,manifest,n));
    uint8_t management[312]={0}; struct acct_request decoded;
    w32(management,1); w32(management+36,1000); w64(management+40,s.db.generation);
    assert(!acct_body_decode(2,8,management,312,s.db.generation,&decoded));
    assert(acct_body_decode(2,8,management,304,s.db.generation,&decoded)==-EINVAL);
    w32(management+24,9); memcpy(management+48,"CiukiUser",9);
    assert(acct_body_decode(2,8,management,312,s.db.generation,&decoded)==-EINVAL);
    uint8_t status_body[32]={0}; w32(status_body,1); w64(status_body+8,1);
    assert(!acct_body_decode(2,7,status_body,32,0,&decoded)); status_body[31]=1;
    assert(acct_body_decode(2,7,status_body,32,0,&decoded)==-EINVAL);
    uint8_t reserve_body[48]={0}; w32(reserve_body,1); w32(reserve_body+4,2); reserve_body[8]=1;
    assert(!acct_body_decode(2,9,reserve_body,48,0,&decoded)); reserve_body[47]=1;
    assert(acct_body_decode(2,9,reserve_body,48,0,&decoded)==-EINVAL);
    unsigned offsets[]={0,8,12,16,20,24,36,40,48,56,64,68,72,76,80,144,208,304,312};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i) {
        unsigned at=offsets[i]; uint8_t saved=manifest[at]; manifest[at]^=128;
        assert(acct_manifest_validate(&q,manifest,n)<0); manifest[at]=saved;
    }
    for(size_t i=0;i<n;++i) assert(acct_manifest_validate(&q,manifest,i)<0);
    /* Independently enumerated tree hash must agree even if the supplied
     * manifest is otherwise valid and its outer digest would match. */
    f.corrupt_manifest=1; struct acct_db next=s.db; next.generation=q.transaction;
    unsigned reserved=f.reserve_calls;
    assert(acct_store_commit(&s.store,&s.db,&next,NULL,0)==-EIO && f.reserve_calls==reserved);
    f.corrupt_manifest=0;
    uint8_t tree[49]={0},hash[32]; w32(tree,1); w32(tree+5,1); assert(!acct_tree_digest(tree,49,hash));
    tree[5]=2; assert(acct_tree_digest(tree,49,hash)==-EINVAL);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS shared CIUKISTG full header/key/map/path checks, every truncation, independent tree verification and unsupported tree refusal");
}
void round2_wire_oracle(const char *path)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
    FILE *file=fopen(path,"wb"); assert(file);
    struct acct_begin b={.action=2,.selector=1,.uid=1000}; uint8_t bytes[64]; assert(!acct_begin_encode(&b,0,bytes,64));
    assert(fwrite(bytes,1,64,file)==64);
    struct acct_request q; struct acct_response r; fake_request(&q,ACCT_AUTH,1,1000,NULL,""); q.service=1;
    assert(!fake_challenge(&s,0,&q)); assert(!acct_challenge_encode(&q.challenge,0,bytes,64)); assert(fwrite(bytes,1,64,file)==64);
    assert(!fake_run(&s,0,&q,&r)); assert(fwrite(f.last_outcome,1,304,file)==304);
    fake_request(&q,ACCT_AUTH,1,1000,NULL,"wrongpass"); assert(fake_run(&s,0,&q,&r)==-EACCES);
    assert(fwrite(f.last_outcome,1,304,file)==304);
    fake_request(&q,ACCT_PASSWORD,1,1000,NULL,"password1"); q.wire_operation=2;
    uint8_t digest[32]; assert(!acct_management_digest(&q,digest)); assert(fwrite(digest,1,32,file)==32 && !fclose(file));
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS complete BEGIN/challenge/success/failure and secret-bearing management digest independent wire oracle");
}

void round3_faulted_queue(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); set_password(&s);
    struct acct_request auth,change; struct acct_response r;
    fake_request(&auth,ACCT_AUTH,s.db.generation,1000,NULL,"password1"); auth.service=1;
    assert(!fake_challenge(&s,0,&auth));
    fake_request(&change,ACCT_PASSWORD,s.db.generation,1000,NULL,"password2"); strcpy(change.old_password,"password1"); change.service=2;
    assert(!fake_reserve(&s,1,&change));
    assert(!acct_submit(&s,1,&change,sizeof(change),&r));
    assert(!acct_submit(&s,0,&auth,sizeof(auth),&r) && s.pending.occupied);
    f.write_error=ENOSPC; strcpy(f.write_error_name,"accounts.db.new");
    unsigned outcomes=f.outcomes;
    assert(acct_work(&s,&r)==1 && r.error==-ENOSPC && s.faulted && !s.available);
    for(unsigned i=0;i<8;++i) assert(!s.attempts[i].occupied);
    assert(acct_work(&s,&r)==1 && r.error==-EIO && !r.body_bytes && !r.authenticated_uid && f.outcomes==outcomes);
    fake_request(&auth,ACCT_AUTH,s.db.generation,1000,NULL,"password1");
    assert(fake_run(&s,0,&auth,&r)==-EIO && f.outcomes==outcomes);
    f.write_error=0; restart(&f,&s);
    fake_request(&auth,ACCT_AUTH,s.db.generation,1000,NULL,"password1"); assert(fake_run(&s,0,&auth,&r)==-EACCES);
    f.now+=1000; fake_request(&auth,ACCT_AUTH,s.db.generation,1000,NULL,"password2"); assert(!fake_run(&s,0,&auth,&r));
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS queued AUTH after ENOSPC is rejected, all attempts invalidated, only recovery admits the new password");
}
static void collision_boot(struct fake *f, struct acct_service *s, unsigned count)
{
    struct users_db old={.count=count,.next_id=1003};
    for(unsigned i=0;i<count;++i) {
        old.records[i].id=1001+i; snprintf(old.records[i].name,32,"Imported%u",i);
        users_derive("password1",old.records[i].salt,10000,old.records[i].verifier);
    }
    uint8_t raw[1400]; size_t n=users_encode(&old,raw,sizeof(raw)); assert(n);
    enrollment_boot(f,s,raw,n);
    struct acct_request plan={.operation=ACCT_ENROLL_PLAN,.service=1,.plan_count=count,.first_old_id=1001};
    for(unsigned i=0;i<count;++i) { plan.plan[i].old_id=1001+i; memcpy(plan.plan[i].name,old.records[i].name,32); }
    struct acct_response r; assert(!acct_submit(s,0,&plan,sizeof(plan),&r)); assert(acct_work(s,&r)==1 && !r.error);
    f->enrollment_count=s->enrollment.map_count;
    for(unsigned i=0;i<f->enrollment_count;++i) {
        f->enrollment[i].legacy_id=s->enrollment.map[i].old_id; f->enrollment[i].uid=s->enrollment.map[i].uid;
        memcpy(f->enrollment[i].root,s->enrollment.map[i].root,64);
    }
    for(unsigned i=0;i<count+2;++i) if(i!=1) {
        struct acct_request q; fake_request(&q,ACCT_ENROLL_VERIFY,s->enrollment.generation,i?998+i:500,NULL,"password1");
        if(i) strcpy(q.old_password,"password1");
        assert(!fake_run(s,0,&q,&r));
    }
    assert(s->available && s->store.high_uid==1000+count);
}
void round3_root_collision(void)
{
    struct fake baseline; fake_init(&baseline); struct acct_service s; collision_boot(&baseline,&s,1);
    acct_service_close(&s); unsigned boundaries=0;
    for(unsigned cut=0;cut<=boundaries;++cut) {
        struct fake f; clone_persistent(&f,&baseline); struct acct_store st=fake_store(&f); assert(!acct_service_open(&s,&st,1));
        struct acct_request q; struct acct_response r; fake_request(&q,ACCT_CREATE,s.db.generation,0,"Created","password1");
        assert(!fake_reserve(&s,1,&q)); q.service=2; f.event=0; f.cut=cut;
        int e=fake_run(&s,1,&q,&r);
        if(!cut) { assert(!e && s.available && !s.faulted && s.db.records[3].uid==1002); boundaries=f.event; }
        else assert(e==-EIO);
        restart(&f,&s);
        int committed=s.db.count==4;
        status(&s,1,q.client_operation_id,&r); assert(a32(r.body+8)==(committed?3u:4u));
        for(unsigned i=0;i<s.db.count;++i) assert(s.db.records[i].uid!=1001);
        fake_request(&q,ACCT_CREATE,s.db.generation,0,"Later","password1"); assert(!fake_run(&s,1,&q,&r));
        assert(s.db.records[s.db.count-1].uid>=1002 && s.available && !s.faulted);
        restart(&f,&s); assert(s.store.high_uid>=1003);
        acct_service_close(&s); fake_destroy(&f);
    }
    fake_destroy(&baseline);
    /* A retired imported root remains occupied after its account is deleted. */
    struct fake f; fake_init(&f); collision_boot(&f,&s,2);
    struct acct_request q; struct acct_response r; fake_request(&q,ACCT_DELETE,s.db.generation,1001,NULL,NULL);
    assert(!fake_run(&s,1,&q,&r)); restart(&f,&s);
    fake_request(&q,ACCT_CREATE,s.db.generation,0,"AfterRetire","password1");
    assert(!fake_run(&s,1,&q,&r) && s.db.records[3].uid==1003 && s.available && !s.faulted);
    acct_service_close(&s); fake_destroy(&f);
    printf("PASS imported/retired numeric roots skipped durably, all %u CREATE cuts retain nonreuse and correct client STATUS\n",boundaries);
}
static void logon_peers(struct fake *f, struct acct_service *s, struct acct_logon *l,
                        struct acct_subject *ui, struct acct_subject *account)
{
    l->kernel=s->store.kernel;
    f->peers[4].subject=(struct acct_subject){.role=2,.service=5,.group_count=1,.endpoint_generation=41,.service_generation=1,.role_generation=1};
    f->peers[5].subject=f->peers[4].subject; /* deliberately equal visible fields, different link */
    f->peers[6].subject=(struct acct_subject){.role=3,.service=1,.group_count=1,.endpoint_generation=61,.service_generation=1,.role_generation=1};
    assert(!l->kernel.retain_peer(f,4,ui)); assert(!l->kernel.retain_peer(f,6,account)); f->attempt_endpoint_generation=41;
}
static void relay_issue(struct fake *f, struct acct_service *s, struct acct_logon *l,
                        const struct acct_subject *ui, const struct acct_subject *account,
                        unsigned action, uint32_t uid, struct acct_challenge *c)
{
    (void)f; struct acct_begin b={.action=action,.selector=1,.uid=uid}; uint8_t begin[64],relay[128];
    assert(!acct_begin_encode(&b,0,begin,64)); assert(!acct_logon_begin(l,ui,begin,64,relay));
    struct acct_request q={.operation=ACCT_CHALLENGE,.service=1,.begin=b}; struct acct_response r;
    assert(!acct_challenge_decode(&q.challenge,1,relay+64,64));
    assert(!acct_submit(s,0,&q,sizeof(q),&r)); assert(acct_work(s,&r)==1 && !r.error);
    assert(!acct_logon_challenged(l,account,r.body,r.body_bytes)); assert(!acct_challenge_decode(c,0,r.body,r.body_bytes));
}
static void submit_bytes(const struct acct_challenge *c, const char *secret, const char *replacement, uint8_t p[208])
{
    memset(p,0,208); w32(p,1); w64(p+8,c->attempt); w32(p+16,c->session); w32(p+20,c->console);
    w32(p+28,(uint32_t)strlen(secret)); w32(p+36,c->uid); w64(p+40,c->accounts_generation); memcpy(p+80,secret,strlen(secret));
    if(replacement) { w32(p+32,(uint32_t)strlen(replacement)); memcpy(p+144,replacement,strlen(replacement)); }
}
void round3_enrollment_outcome(void)
{
    struct fake f; fake_init(&f); struct acct_service s; enrollment_boot(&f,&s,NULL,0);
    struct acct_request q={.operation=ACCT_ENROLL_PLAN,.service=1,.plan_count=1}; strcpy(q.plan[0].name,"Alice");
    struct acct_response r; assert(!acct_submit(&s,0,&q,sizeof(q),&r)); assert(acct_work(&s,&r)==1 && !r.error);
    fake_request(&q,ACCT_ENROLL_VERIFY,s.enrollment.generation,500,NULL,"password1"); assert(!fake_run(&s,0,&q,&r));
    struct acct_logon l={0}; struct acct_subject ui,account; logon_peers(&f,&s,&l,&ui,&account);
    struct acct_challenge c; relay_issue(&f,&s,&l,&ui,&account,4,1000,&c);
    uint8_t submit[208],relay[272]; submit_bytes(&c,"","password1",submit);
    assert(!acct_logon_submit(&l,&ui,submit,208,relay));
    assert(!acct_submit_body(&s,0,1,4,relay,272,c.accounts_generation,0,&r)); assert(acct_work(&s,&r)==1 && !r.error);
    assert(s.available && s.db.generation>c.accounts_generation && a32(f.last_outcome+68)==3);
    assert(azero(f.last_outcome+96,208));
    account.service=5; struct acct_auth_outcome o; uint8_t progress[16],bad[304];
    memcpy(bad,f.last_outcome,304); bad[96]=1; assert(acct_logon_outcome(&l,&account,s.db.generation,bad,304,&o,progress)==-EINVAL);
    memcpy(bad,f.last_outcome,304); w64(bad+8,c.attempt+1); assert(acct_logon_outcome(&l,&account,s.db.generation,bad,304,&o,progress)==-EAGAIN);
    struct acct_logon saved=l; /* negative probes consume only their matching attempt */
    l.kernel.subject_get(&f,&saved.attempts[0].ui);
    assert(acct_logon_outcome(&l,&account,c.accounts_generation,f.last_outcome,304,&o,progress)==-EAGAIN);
    l=saved; /* restore the separately retained test slot, balanced below */
    assert(!acct_logon_outcome(&l,&account,s.db.generation,f.last_outcome,304,&o,progress));
    assert(o.disposition==3 && o.resulting_generation==s.db.generation && azero(o.credential,sizeof(o.credential)) && azero(o.profile,sizeof(o.profile)));
    assert(acct_logon_outcome(&l,&account,s.db.generation,f.last_outcome,304,&o,progress)==-EAGAIN);
    acct_logon_clear(&l); l.kernel.release_peer(&f,&ui); l.kernel.release_peer(&f,&account);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS final production enrollment relays the original challenge at the resulting generation, rejects stale/forged/replayed outcomes and exports no credentials");
}
void round3_logon_restart(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
    struct acct_logon l={0}; struct acct_subject ui,account; logon_peers(&f,&s,&l,&ui,&account); struct acct_challenge c;
    for(unsigned i=0;i<7;++i) relay_issue(&f,&s,&l,&ui,&account,2,1000,&c);
    uint64_t high=s.high_attempt; acct_logon_clear(&l);
    ++f.logon_generation; f.peers[0].subject.role_generation=f.logon_generation; ++f.peers[0].epoch;
    l=(struct acct_logon){.kernel=s.store.kernel}; relay_issue(&f,&s,&l,&ui,&account,2,1000,&c);
    assert(c.attempt>high && c.attempt==((uint64_t)f.logon_generation<<32|1));
    l.next_attempt=UINT32_MAX-1; relay_issue(&f,&s,&l,&ui,&account,2,1000,&c);
    assert((uint32_t)c.attempt==UINT32_MAX);
    struct acct_begin b={.action=2,.selector=1,.uid=1000}; uint8_t begin[64],relay[128]; assert(!acct_begin_encode(&b,0,begin,64));
    assert(acct_logon_begin(&l,&ui,begin,64,relay)==-EAGAIN); /* no wrap */
    ++f.logon_generation; f.peers[0].subject.role_generation=f.logon_generation;
    relay_issue(&f,&s,&l,&ui,&account,2,1000,&c); assert((uint32_t)c.attempt==1);
    acct_logon_clear(&l); l.kernel.release_peer(&f,&ui); l.kernel.release_peer(&f,&account);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS LOGON respawn attempts exceed surviving ACCOUNT watermark, bounded counter cannot wrap and next role generation resumes");
}
void round3_distinct_handles(void)
{
    struct fake f; fake_init(&f); f.distinct_handles=1; struct acct_service s; fake_boot(&f,&s);
    struct acct_request q; struct acct_response r; fake_request(&q,ACCT_AUTH,s.db.generation,1000,NULL,"");
    assert(!fake_challenge(&s,0,&q)); uintptr_t original=s.attempts[0].logon.retained;
    struct acct_subject other; assert(!s.store.kernel.retain_peer(&f,0,&other)); assert(other.retained!=original);
    s.store.kernel.release_peer(&f,&other); assert(!fake_run(&s,0,&q,&r) && a32(f.last_outcome+68)==1);
    /* Identical public epochs cannot authorize a different kernel link. */
    fake_request(&q,ACCT_AUTH,s.db.generation,1000,NULL,""); assert(!fake_challenge(&s,0,&q)); q.service=1;
    f.peers[7].subject=f.peers[0].subject;
    assert(!acct_submit(&s,7,&q,sizeof(q),&r)); assert(acct_work(&s,&r)==1 && !r.error && a32(r.body+4)==4);
    assert((int32_t)a32(r.body+8)==-EAGAIN);
    struct acct_logon l={0}; struct acct_subject ui,account; logon_peers(&f,&s,&l,&ui,&account);
    /* The fake helper's small IDs must not precede the real LOGON stream. */
    struct acct_challenge c; relay_issue(&f,&s,&l,&ui,&account,2,1000,&c);
    uint8_t submit[208],relay[272]; submit_bytes(&c,"",NULL,submit);
    assert(!l.kernel.retain_peer(&f,5,&other)); assert(acct_logon_submit(&l,&other,submit,208,relay)==-EAGAIN);
    l.kernel.release_peer(&f,&other); assert(!l.kernel.retain_peer(&f,4,&other)); assert(other.retained!=ui.retained);
    assert(!acct_logon_submit(&l,&other,submit,208,relay)); l.kernel.release_peer(&f,&other);
    acct_logon_clear(&l); l.kernel.release_peer(&f,&ui); l.kernel.release_peer(&f,&account);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS separate retained handles for the same link authenticate/submit; different links with equal visible generations fail");
}
void round3_intent_bounds(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
    uint8_t intent[192+ACCT_OWNER_BODY_MAX+ACCT_FILE_MAX]={0};
    const size_t sizes[]={64+ACCT_FILE_MAX+1,sizeof(intent)};
    uint64_t generation=s.db.generation, high=s.store.high_generation;
    for(unsigned i=0;i<2;++i) {
        size_t n=sizes[i]; memset(intent,0,sizeof(intent)); memcpy(intent,"CIACTLC4",8); w32(intent+8,(uint32_t)n); w64(intent+16,generation);
        size_t db_bytes; assert(!acct_encode(&s.db,0,intent+64,ACCT_FILE_MAX,&db_bytes));
        uint64_t sequence=0; assert(!acct_blob_write(&s.store,"accounts.local",intent,n,&sequence));
        assert(acct_store_local_recover(&s.store)==-EIO && s.db.generation==generation && s.store.high_generation==high);
        assert(!s.store.io.remove(&f,"accounts.local")); assert(!s.store.io.barrier(&f));
    }
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS checksummed local intents over 64+ACCT_FILE_MAX are rejected before payload copy/decoding at both boundary and maximum blob size");
}
