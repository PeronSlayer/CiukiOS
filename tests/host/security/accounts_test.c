/* SPDX-License-Identifier: MIT */
#include "fake.h"
#include "wire.h"
#include "users.h"
#include <ciuki/sha256.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int zeros(const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n--) if (*b++) return 0;
    return 1;
}
static void rehash(unsigned char *b, size_t n)
{ memset(b + 64, 0, 32); sha256(b, n, b + 64); }
static void codec(const char *path)
{
    struct acct_db d, decoded;
    fake_seed(&d, 1);
    unsigned char b[ACCT_FILE_MAX + 1]; size_t n = 0;
    assert(!acct_encode(&d, 1, b, sizeof(b), &n) && n == 864);
    FILE *out = fopen(path, "wb"); assert(out);
    assert(fwrite(b, 1, n, out) == n && !fclose(out));
    assert(!acct_decode(&decoded, 1, b, n));
    assert(decoded.count == 3 && decoded.records[2].uid == 1000);
    assert(acct_decode(&decoded, 0, b, n) == -EIO);
    for (size_t i = 0; i < n; ++i) {
        b[i] ^= 1; memset(&decoded, 0x5a, sizeof(decoded));
        assert(acct_decode(&decoded, 1, b, n) == -EIO);
        assert(((unsigned char *)&decoded)[0] == 0x5a); b[i] ^= 1;
    }
    for (size_t i = 0; i < n; ++i) assert(acct_decode(&decoded, 1, b, i) == -EIO);
    assert(acct_decode(&decoded, 1, b, n + 1) == -EIO);
    static const unsigned offsets[] = {8, 12, 24, 28, 32, 36, 40, 52, 56, 60,
                                       96, 100, 104, 108, 112, 116, 121, 200, 296,
                                       352, 360, 608, 612, 616, 620, 624, 663};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        unsigned char saved = b[offsets[i]];
        b[offsets[i]] = offsets[i] == 24 ? 0 : 0xff; rehash(b, n);
        assert(acct_decode(&decoded, 1, b, n) == -EIO);
        b[offsets[i]] = saved; rehash(b, n);
    }
    struct acct_db bad = d;
    bad.records[2].iterations = 9999; assert(acct_validate(&bad, 1) == -EINVAL);
    bad = d; bad.next_uid = 1000; assert(acct_validate(&bad, 1) == -EINVAL);
    bad = d; strcpy(bad.records[2].name, "Guest"); assert(acct_validate(&bad, 1) == -EINVAL);
    bad = d; bad.records[2].uid = UINT32_MAX; assert(acct_validate(&bad, 1) == -EINVAL);
    bad = d; bad.sid[0] = 1; assert(acct_validate(&bad, 1) == -EINVAL);
    assert(acct_encode(&d, 1, b, n - 1, &n) == -ENOSPC);
    assert(acct_picture_syntax("catalogue/ciuki.png"));
    assert(!acct_picture_syntax("../secret") && !acct_picture_syntax("a/../b"));
    assert(!acct_picture_syntax("/System/foo") && !acct_picture_syntax("a//b"));
    puts("PASS codec: independent seed fixture, every truncated/corrupted byte, semantic corruption and unchanged failure output");
}
static void crypto(const char *hex)
{
    uint8_t salt[16], out[32], expected[32];
    assert(strlen(hex) == 64);
    for (unsigned i = 0; i < 16; ++i) salt[i] = (uint8_t)i;
    for (unsigned i = 0; i < 32; ++i) { unsigned x; assert(sscanf(hex + i * 2, "%2x", &x) == 1); expected[i] = (uint8_t)x; }
    assert(!acct_derive("password", salt, ACCT_ITERATIONS, out));
    assert(acct_equal_secret(out, expected, 32));
    for (unsigned i = 0; i < 32; ++i) { expected[i] ^= 1; assert(!acct_equal_secret(out, expected, 32)); expected[i] ^= 1; }
    assert(acct_derive("password", salt, 9999, out) == -EINVAL);
    assert(acct_derive("password", salt, UINT32_MAX, out) == -EINVAL);
    assert(acct_password_valid("12345678", 0) && acct_password_valid("        ", 0));
    assert(!acct_password_valid("1234567", 0) && !acct_password_valid("", 0));
    assert(!acct_password_valid("1234567\n", 0));
    char boundary[65]; memset(boundary, 'x', 63); boundary[63] = 0;
    assert(acct_password_valid(boundary, 0)); boundary[63] = 'x'; boundary[64] = 0;
    assert(!acct_password_valid(boundary, 0));
    assert(acct_reserved_name("sYsTeM") && acct_reserved_name("users"));
    assert(!acct_name_valid("a.b") && !acct_name_valid("a/b") && !acct_name_valid(""));
    assert(acct_name_valid("Alice_1-Test"));
    acct_wipe(boundary, sizeof(boundary)); assert(zeros(boundary, sizeof(boundary)));
    puts("PASS crypto: independent hashlib PBKDF2/10000 oracle, every verifier mismatch offset, boundaries and wiping");
}
static void migration(void)
{
    struct users_db old = {0}; old.count = 3; old.next_id = 20;
    const uint32_t ids[3] = {9, 2, 5};
    const char *const names[3] = {"SYSTEM", "Short", "Empty"};
    const char *const passwords[3] = {"password", "abc", ""};
    for (unsigned i = 0; i < 3; ++i) {
        struct users_record *r = &old.records[i]; r->id = ids[i]; strcpy(r->name, names[i]);
        memset(r->salt, (int)i, sizeof(r->salt)); users_derive(passwords[i], r->salt, USERS_ITERATIONS, r->verifier);
    }
    unsigned char b[1400]; size_t n = users_encode(&old, b, sizeof(b)); assert(n);
    struct acct_import_options options = {0}; options.first_legacy_id = 9;
    struct acct_db imported; struct acct_import_map maps[16]; unsigned count = 0;
    assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -EINVAL);
    strcpy(options.replacement_names[2], "FirstAdmin");
    assert(!acct_import_g32(b, n, &options, &imported, maps, &count) && count == 3);
    assert(imported.records[2].uid == 1000 && imported.records[2].legacy_id == 9 && imported.records[2].groups == ACCT_ADMIN);
    assert(imported.records[3].uid == 1001 && imported.records[3].legacy_id == 2 && imported.records[3].groups == ACCT_STANDARD);
    assert(imported.records[3].flags & ACCT_CHANGE_REQUIRED);
    assert(imported.records[4].legacy_id == 5 && !(imported.records[4].flags & ACCT_ENABLED));
    assert(maps[0].legacy_id == 2 && maps[0].uid == 1001 && !strcmp(maps[0].root, "/Windows/Profiles/u00000002"));
    assert(maps[2].uid == 1000 && !memcmp(imported.records[2].verifier, old.records[0].verifier, 32));
    assert(acct_validate(&imported, 0) == -EINVAL); /* no SID/enrollment, never publish draft */
    options.first_legacy_id = 1;
    assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -EINVAL);
    options.first_legacy_id = 9; strcpy(options.replacement_names[0], "FirstAdmin");
    assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -EEXIST);
    options.replacement_names[0][0] = 0;
    b[70] ^= 1; assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -EIO); b[70] ^= 1;
    assert(acct_import_g32(b, n - 1, &options, &imported, maps, &count) == -EIO);
    old.count = 0; n = users_encode(&old, b, sizeof(b));
    assert(acct_import_g32(b, n, &options, &imported, maps, &count) == -ENOENT);
    old.count = 16; old.next_id = 1001;
    for (unsigned i = 0; i < 16; ++i) {
        old.records[i] = old.records[0]; old.records[i].id = i == 15 ? 1000 : 1 + i;
        snprintf(old.records[i].name, 32, "Legacy%u", i);
    }
    n = users_encode(&old, b, sizeof(b)); assert(n == sizeof(b));
    memset(&options, 0, sizeof(options)); options.first_legacy_id = 2;
    assert(!acct_import_g32(b, n, &options, &imported, maps, &count) && count == 16 && imported.count == 18);
    assert(imported.records[2].legacy_id == 2 && imported.records[2].uid == 1000);
    assert(maps[15].legacy_id == 1000 && maps[15].uid != 1000 && !strcmp(maps[15].root, "/Windows/Profiles/u000003e8"));
    assert(imported.next_uid == 1016);
    puts("PASS G32 import: explicit administrator, sorted monotonic mapping, preserved verifiers/roots, reserved-name enrollment, short/blank and corrupt/empty sources");
}
static void migration_batch(void)
{
    struct users_db old={0}; old.count=16; old.next_id=1001;
    for(unsigned i=0;i<16;++i) {
        struct users_record *r=&old.records[i]; r->id=i==15?1000:i+1;
        snprintf(r->name,32,"Legacy%u",i); memset(r->salt,(int)i,16);
        users_derive("password1",r->salt,USERS_ITERATIONS,r->verifier);
    }
    uint8_t source[1400]; size_t bytes=users_encode(&old,source,sizeof(source)); assert(bytes==1400);
    struct acct_db draft; struct acct_import_map maps[16]; unsigned count;
    struct acct_import_options options={0}; options.first_legacy_id=2;
    assert(!acct_import_g32(source,bytes,&options,&draft,maps,&count) && count==16 && draft.count==18);
    draft.sid[0]=1; draft.sid[1]=2; draft.sid[2]=3;
    /* Explicit enrollment is simulated at the boundary, never by enabling an
     * unknown-length imported password. Existing verifier bytes remain intact. */
    draft.records[2].flags=ACCT_PASSWORD_SET|ACCT_ENABLED;
    struct acct_owner_request initial={0}; initial.volume=1234; initial.transaction=1; initial.count=18;
    memcpy(initial.sid,draft.sid,12);
    for(unsigned i=0;i<18;++i) {
        struct acct_owner_binding *b=&initial.bindings[i]; b->uid=draft.records[i].uid; b->volume=1234; b->action=1;
        if(i<2) snprintf(b->root,64,"/Windows/Profiles/u%08x",b->uid);
        else { unsigned j=0; while(j<count && maps[j].uid!=b->uid) ++j; assert(j<count); memcpy(b->root,maps[j].root,64); }
    }
    unsigned boundaries=0, good=0, closed=0;
    for(unsigned cut=0;cut<=boundaries;++cut) {
        struct fake f; fake_init(&f); struct acct_store st=fake_store(&f);
        f.enrollment_count=count; memcpy(f.enrollment,maps,sizeof(maps));
        f.move.uid=maps[15].uid; strcpy(f.move.from,maps[15].root); strcpy(f.move.to,"/Windows/Profiles/u000007d0");
        uint8_t locator_digest[32]; sha256(f.move.from,strlen(f.move.from),locator_digest); f.move.before=a64(locator_digest);
        sha256(f.move.to,strlen(f.move.to),locator_digest); f.move.after=a64(locator_digest);
        for(unsigned i=0;i<initial.count;++i) if(initial.bindings[i].uid==f.move.uid) strcpy(initial.bindings[i].root,f.move.to);
        struct acct_bootstrap b={0}; b.state=1; b.origin=3; b.volume=1234; b.next_uid=1016; b.generation=1;
        sha256(source,bytes,b.seed_digest); uint8_t boot[160]; assert(!acct_boot_encode(&b,boot,160));
        fake_put(&f,"bootstrap.state",boot,160); assert(!st.kernel.lease_acquire(st.kernel.ctx));
        f.event=0; f.cut=cut;
        int e=acct_store_commit(&st,NULL,&draft,&initial,1);
        if(!cut) { assert(!e); boundaries=f.event; }
        else assert(e==-EIO);
        st.kernel.lease_release(st.kernel.ctx); fake_powercut(&f);
        struct acct_service s; e=acct_service_open(&s,&st,1);
        if(e) {
            assert(e==-EIO); ++closed;
            struct acct_bootstrap stopped; assert(!acct_boot_read(&st,&stopped));
            assert(stopped.state==ACCT_UNPROVISIONED && fake_file_index(&f,"accounts.db.txn")<0);
        }
        else {
            if(!s.available) { ++closed; assert(!f.profile_count); acct_service_close(&s); fake_destroy(&f); continue; }
            ++good; assert(s.db.count==18 && f.profile_count==18);
            assert(f.owner_used==1 && f.owners[0].request.count==18);
            int moved=0; for(unsigned i=0;i<f.profile_count;++i) if(f.profiles[i].uid==f.move.uid) moved=!strcmp(f.profiles[i].root,f.move.to);
            assert(moved);
            for(unsigned i=0;i<18;++i) {
                assert(f.owners[0].reply.results[i].generation==7);
                if(i>=2) assert(!memcmp(s.db.records[i].verifier,draft.records[i].verifier,32));
            }
            assert(!acct_boot_read(&s.store,&b) && b.state==5);
            if(!cut) {
                struct acct_request enroll; struct acct_response response;
                fake_request(&enroll,ACCT_ENROLL_VERIFY,s.db.generation,1001,s.db.records[3].name,"newpass12");
                strcpy(enroll.old_password,"password1");
                assert(!fake_run(&s,0,&enroll,&response) && !response.authenticated_uid && f.outcomes==1);
                assert(!(s.db.records[3].flags&(ACCT_ENABLED|ACCT_CHANGE_REQUIRED)));
            }
            acct_service_close(&s);
        }
        fake_destroy(&f);
    }
    printf("PASS sixteen-profile migration plus both built-ins, mapped occupied uid-1000 root, one G/owner generation, all %u cuts (%u resumed, %u closed)\n",boundaries,good,closed);
}
static void restart(struct fake *f, struct acct_service *s)
{
    acct_service_close(s); fake_powercut(f); struct acct_store st=fake_store(f);
    assert(!acct_service_open(s,&st,1) && s->available);
}
static void password(struct acct_service *s, uint32_t uid, const char *old, const char *next)
{
    struct acct_request q; struct acct_response r;
    fake_request(&q,ACCT_PASSWORD,s->db.generation,uid,NULL,next);
    if(old) strcpy(q.old_password,old);
    assert(!fake_run(s,1,&q,&r));
}
static void create_user(struct acct_service *s, const char *name, unsigned enabled, unsigned groups)
{
    struct acct_request q; struct acct_response r;
    fake_request(&q,ACCT_CREATE,s->db.generation,0,name,"password1"); q.flags=enabled; q.groups=groups;
    assert(!fake_run(s,1,&q,&r));
}
static void service(void)
{
    struct fake f; fake_init(&f); struct acct_store st=fake_store(&f); struct acct_service s, other;
    assert(acct_service_open(&s,&st,0)==-ENOSYS);
    assert(acct_service_open(&s,&st,1)==-EIO); /* no initial state is not fresh */
    fake_boot(&f,&s); assert(f.profile_count==3 && f.owner_used==1 && f.owners[0].request.count==3);
    assert(acct_service_open(&other,&st,1)==-EBUSY);
    struct acct_request q; struct acct_response r;
    fake_request(&q,ACCT_AUTH,s.db.generation,0,"CiukiUser","");
    assert(!fake_run(&s,0,&q,&r) && !r.authenticated_uid && f.outcomes==1);
    assert(a32(f.last_outcome+104)==1000 && a32(f.last_outcome+164)==112);
    fake_request(&q,ACCT_AUTH,s.db.generation,0,"CiukiUser","");
    assert(fake_run(&s,2,&q,&r)==-EPERM);
    fake_request(&q,ACCT_AUTH,s.db.generation,0,"unknown","badpass1");
    assert(fake_run(&s,0,&q,&r)==-EACCES && r.retry_after_ms==1000);
    fake_request(&q,ACCT_AUTH,s.db.generation,0,"CiukiUser","");
    assert(fake_run(&s,0,&q,&r)==-EAGAIN); f.now+=1000;
    password(&s,1000,"","newpass12"); create_user(&s,"Bob",1,ACCT_STANDARD);
    f.active_uid=1001;
    const unsigned ops[]={ACCT_DELETE,ACCT_ENABLE,ACCT_ROLE,ACCT_PASSWORD};
    for(unsigned i=0;i<4;++i) {
        fake_request(&q,ops[i],s.db.generation,1001,NULL,"password2"); q.groups=ACCT_ADMIN;
        assert(fake_run(&s,1,&q,&r)==-EBUSY);
    }
    f.active_uid=UINT32_MAX;
    fake_request(&q,ACCT_PASSWORD,s.db.generation,1001,NULL,"password2"); strcpy(q.old_password,"wrong");
    assert(fake_run(&s,2,&q,&r)==-EACCES); f.now+=1000;
    fake_request(&q,ACCT_PASSWORD,s.db.generation,1001,NULL,"password2"); strcpy(q.old_password,"password1");
    assert(!fake_run(&s,2,&q,&r));
    fake_request(&q,ACCT_ROLE,s.db.generation,1000,NULL,NULL); q.groups=ACCT_STANDARD;
    assert(fake_run(&s,1,&q,&r)==-EPERM);
    password(&s,500,NULL,"recovery1");
    fake_request(&q,ACCT_AUTH,s.db.generation,0,"CiukiAdministrator","recovery1");
    assert(fake_run(&s,0,&q,&r)==-EACCES); f.now+=1000; f.peers[0].subject.safe_mode=1;
    fake_request(&q,ACCT_AUTH,s.db.generation,0,"CiukiAdministrator","recovery1");
    assert(!fake_run(&s,0,&q,&r) && a32(f.last_outcome+104)==500 && a32(f.last_outcome+152)==3);
    fake_request(&q,ACCT_DELETE,s.db.generation,1001,NULL,NULL); assert(!fake_run(&s,1,&q,&r));
    create_user(&s,"Next",1,ACCT_STANDARD); assert(s.db.records[3].uid==1002);
    restart(&f,&s); fake_request(&q,ACCT_AUTH,s.db.generation,0,"CiukiUser","");
    assert(fake_run(&s,0,&q,&r)==-EACCES);
    acct_service_close(&s); assert(zeros(&s,sizeof(s))); fake_destroy(&f);
    puts("PASS sealed service, complete bootstrap batch, filtered outcomes, backoff, active/last-admin, Safe Mode, monotonic ids and restart");
}
static void retirement(void)
{
    /* Each retirement prerequisite includes Guest, normally enabled 500, and
     * ordinary users. Repeat after service restart and simulated reboot. */
    for(unsigned which=0;which<3;++which) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
        struct acct_request q; struct acct_response r; unsigned uid=which==0?501:which==1?500:1001;
        if(uid==1001) create_user(&s,"Bob",0,ACCT_ADMIN); else password(&s,uid,NULL,"password1");

        fake_request(&q,ACCT_ENABLE,s.db.generation,uid,NULL,NULL); q.flags=1;
        assert(fake_run(&s,1,&q,&r)<0);
        fake_request(&q,ACCT_RENAME,s.db.generation,1000,"Renamed",NULL); assert(fake_run(&s,1,&q,&r)==-EPERM);
        fake_request(&q,ACCT_ROLE,s.db.generation,1000,NULL,NULL); q.groups=ACCT_STANDARD;
        assert(fake_run(&s,1,&q,&r)==-EPERM);
        restart(&f,&s);
        struct acct_bootstrap boot; assert(!acct_boot_read(&s.store,&boot));
        assert(acct_autologon_eligible(&s.db,&boot,0,0,0));
        assert(!acct_autologon_eligible(&s.db,&boot,1,0,0) && !acct_autologon_eligible(&s.db,&boot,0,1,0));
        password(&s,1000,"","newpass12");
        fake_request(&q,ACCT_ENABLE,s.db.generation,uid,NULL,NULL); q.flags=1; assert(!fake_run(&s,1,&q,&r));
        fake_request(&q,ACCT_ENABLE,s.db.generation,uid,NULL,NULL); assert(!fake_run(&s,1,&q,&r));
        if(uid==1001) { fake_request(&q,ACCT_DELETE,s.db.generation,uid,NULL,NULL); assert(!fake_run(&s,1,&q,&r)); }
        restart(&f,&s); assert(!acct_boot_read(&s.store,&boot));
        assert((s.db.flags&4) && (boot.flags&4) && !acct_autologon_eligible(&s.db,&boot,0,0,0));
        fake_request(&q,ACCT_RENAME,s.db.generation,1000,"Renamed",NULL); assert(!fake_run(&s,1,&q,&r));
        restart(&f,&s); assert(s.db.flags&4);
        acct_service_close(&s); fake_destroy(&f);
    }
    /* Password-first replacement uses ordinary single-target updates. */
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
    create_user(&s,"Replacement",0,ACCT_ADMIN); struct acct_request q; struct acct_response r;
    fake_request(&q,ACCT_ENABLE,s.db.generation,1000,NULL,NULL); assert(fake_run(&s,1,&q,&r)==-EPERM);
    password(&s,1000,"","newpass12");
    fake_request(&q,ACCT_ENABLE,s.db.generation,1001,NULL,NULL); q.flags=1; assert(!fake_run(&s,1,&q,&r));
    f.active_uid=1000; fake_request(&q,ACCT_ENABLE,s.db.generation,1000,NULL,NULL); assert(fake_run(&s,1,&q,&r)==-EBUSY);
    f.active_uid=1001; fake_request(&q,ACCT_ENABLE,s.db.generation,1000,NULL,NULL); assert(!fake_run(&s,1,&q,&r));
    fake_request(&q,ACCT_ROLE,s.db.generation,1000,NULL,NULL); q.groups=ACCT_STANDARD; assert(!fake_run(&s,1,&q,&r));
    fake_request(&q,ACCT_RENAME,s.db.generation,1000,"FormerBlank",NULL); assert(!fake_run(&s,1,&q,&r));
    restart(&f,&s); assert((s.db.flags&4) && !s.db.auto_logon_uid);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS H03/H10 sole blank prerequisites, password-first replacement, active/last-admin and permanent retirement");
}

static void setup_seed(void)
{
    struct fake f; fake_init(&f); fake_provision(&f,0); struct acct_store st=fake_store(&f); struct acct_service s;
    assert(!acct_service_open(&s,&st,1)); struct acct_db seed; fake_seed(&seed,0); size_t n; uint8_t bytes[ACCT_FILE_MAX];
    assert(!acct_encode(&seed,1,bytes,sizeof(bytes),&n));
    unsigned entropy=f.entropy_calls;
    assert(acct_service_bootstrap(&s,bytes,n,1)==-EINVAL && f.entropy_calls==entropy);
    assert(!acct_service_bootstrap(&s,bytes,n,2)); assert(s.db.generation==1 && !s.db.auto_logon_uid);
    assert(!memcmp(seed.records[0].verifier,s.db.records[0].verifier,32));
    assert(!memcmp(seed.records[2].salt,s.db.records[2].salt,16));
    restart(&f,&s); assert(s.db.origin==2 && f.profile_count==3);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS exact Setup seed retains both independent verifiers, rejects ready origin, three owner bindings and consumed state");
}
static void bootstrap_codec(const char *path)
{
    struct acct_bootstrap b={0}, out; b.state=1; b.origin=1; b.volume=1234; b.next_uid=1001; b.generation=1;
    for(unsigned i=0;i<32;++i) b.seed_digest[i]=(uint8_t)(i+1);
    uint8_t bytes[161]; assert(!acct_boot_encode(&b,bytes,160)); FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(bytes,1,160,f)==160 && !fclose(f));
    assert(!acct_boot_decode(&out,bytes,160));
    for(unsigned i=0;i<160;++i) { bytes[i]^=1; assert(acct_boot_decode(&out,bytes,160)==-EIO); bytes[i]^=1; }
    for(unsigned i=0;i<160;++i) assert(acct_boot_decode(&out,bytes,i)==-EIO);
    b.state=2; assert(acct_boot_encode(&b,bytes,160)==-EINVAL);
    b.sid[0]=1; b.transaction=1; assert(!acct_boot_encode(&b,bytes,160));
    b.state=3; assert(acct_boot_encode(&b,bytes,160)==-EINVAL);
    b.flags=2; b.owner_generation=7; assert(!acct_boot_encode(&b,bytes,160));
    b.state=4; b.accounts_generation=1; assert(!acct_boot_encode(&b,bytes,160));
    b.state=5; assert(acct_boot_encode(&b,bytes,160)==-EINVAL); b.flags|=1; assert(!acct_boot_encode(&b,bytes,160));
    puts("PASS independent 160-byte bootstrap oracle, all corruption/truncation and state field combinations");
}
static void batch(const char *path)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
    struct acct_owner_request q=f.owners[0].request; struct acct_owner_reply r;
    uint8_t bytes[ACCT_OWNER_BODY_MAX], reply[ACCT_OWNER_REPLY_MAX]; size_t n,m; unsigned op; struct acct_owner_request out;
    assert(!acct_owner_encode(&q,1,bytes,sizeof(bytes),&n) && n==544);
    assert(a32(bytes)==1 && a32(bytes+4)==160 && a32(bytes+12)==3 && a64(bytes+40)==1);
    assert(a32(bytes+160)==500 && a32(bytes+288)==501 && a32(bytes+416)==1000);
    assert(!acct_owner_decode(&out,&op,bytes,n));
    for(unsigned i=0;i<n;++i) { bytes[i]^=128; assert(acct_owner_decode(&out,&op,bytes,n)<0); bytes[i]^=128; }
    assert(!acct_owner_call(&s.store,2,&q,&r) && r.count==3 && r.state==2);
    uint64_t generation=r.generation; assert(r.results[0].generation==generation && r.results[2].generation==generation);
    assert(!acct_owner_reply_encode(&r,reply,sizeof(reply),&m) && m==192);
    if(path) { FILE *fixture=fopen(path,"wb"); assert(fixture); assert(fwrite(bytes,1,n,fixture)==n && fwrite(reply,1,m,fixture)==m && !fclose(fixture)); }
    assert(acct_owner_call(&s.store,3,&q,&r)==-EBUSY);
    q.expected_generation=999; /* existing key with contradictory content fails EIO */
    assert(acct_owner_call(&s.store,1,&q,&r)==-EIO);
    q=f.owners[0].request; create_user(&s,"Disabled",0,ACCT_STANDARD);
    assert(!acct_owner_call(&s.store,4,&q,&r) && r.generation==generation); /* lifetime receipt after newer commit */
    struct acct_owner_request abandoned=f.owners[1].request; abandoned.transaction=77;
    assert(!acct_owner_encode(&abandoned,3,bytes,sizeof(bytes),&n)); memcpy(abandoned.digest,bytes+128,32);
    assert(!acct_owner_call(&s.store,3,&abandoned,&r) && r.state==0 && !r.count);
    assert(acct_owner_call(&s.store,2,&abandoned,&r)==-EIO);
    abandoned.bindings[0].locator=123;
    assert(acct_owner_encode(&abandoned,7,bytes,sizeof(bytes),&n)==-EINVAL);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS full-vector owner codecs, zero locators, reserved generations, digest/key matching and UNKNOWN abort without a receipt");
}

static void failure_inputs(void)
{
    for(unsigned entropy=1;entropy<=2;++entropy) {
        struct fake f; fake_init(&f); fake_provision(&f,1); struct acct_service s; struct acct_store st=fake_store(&f);
        assert(!acct_service_open(&s,&st,1)); struct acct_db seed; fake_seed(&seed,1); uint8_t b[ACCT_FILE_MAX]; size_t n;
        assert(!acct_encode(&seed,1,b,sizeof(b),&n)); f.fail_entropy=entropy; unsigned before=f.owner_used;
        assert(acct_service_bootstrap(&s,b,n,1)==-EAGAIN && !s.available && f.owner_used==before);
        acct_service_close(&s); fake_destroy(&f);
    }
    for(unsigned mode=0;mode<5;++mode) {
        struct fake f; fake_init(&f); fake_provision(&f,1); struct acct_store st=fake_store(&f); struct acct_service s;
        const char *name=mode==0?"bootstrap.state":"provision.seed";
        int i=fake_file_index(&f,name); assert(i>=0);
        if(mode<2) f.files[i].size=1;
        else if(mode==2) f.files[i].data[8]^=1;
        else if(mode==3) { f.owner_generation=7; }
        else { uint8_t sid[64]={1}; fake_put(&f,"machine.sid",sid,64); }
        assert(acct_service_open(&s,&st,1)==-EIO && !f.leased);
        fake_destroy(&f);
    }
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); acct_service_close(&s);
    /* A consumed tombstone and residual valid seed never recreate accounts. */
    int i=fake_file_index(&f,"accounts.db"); assert(i>=0); f.files[i].name[0]=0;
    struct acct_store st=fake_store(&f); assert(acct_service_open(&s,&st,1)==-EIO && !f.leased);
    fake_destroy(&f);
    puts("PASS entropy failures, missing/corrupt/contradictory bootstrap evidence and no consumed seed replay");
}
static void capacity(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); password(&s,1000,"","password1");
    for(unsigned i=0;i<15;++i) { char name[32]; snprintf(name,32,"User%u",i); create_user(&s,name,0,ACCT_STANDARD); }
    assert(s.db.count==18 && f.profile_count==18);  struct acct_request q; struct acct_response r;
    fake_request(&q,ACCT_CREATE,s.db.generation,0,"Overflow","password1");
    assert(fake_run(&s,1,&q,&r)==-ENOSPC);
    fake_request(&q,ACCT_DELETE,s.db.generation,1015,NULL,NULL); assert(!fake_run(&s,1,&q,&r));
    create_user(&s,"Last",0,ACCT_STANDARD); assert(s.db.records[17].uid==1016);
    s.store.high_generation=UINT64_MAX;
    fake_request(&q,ACCT_PICTURE,s.db.generation,1000,NULL,NULL); assert(fake_run(&s,1,&q,&r)==-EOVERFLOW);
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS 18-account capacity, retained deleted roots and G exhaustion before writes");
}
static void disk_oom_corruption(void)
{
    unsigned allocs=0;
    for(unsigned run=0;run<=allocs;++run) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); password(&s,1000,"","password1");
        unsigned start=f.allocations; f.fail_alloc=run?start+run:0;
        struct acct_request q; struct acct_response r; fake_request(&q,ACCT_CREATE,s.db.generation,0,"Bob","password1");
        int e=fake_run(&s,1,&q,&r);
        if(!run) { assert(!e); allocs=f.allocations-start; } else assert(e==-ENOMEM);
        acct_service_close(&s); fake_destroy(&f);
    }
    for(unsigned which=0;which<3;++which) {
        struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); password(&s,1000,"","password1");
        strcpy(f.write_error_name,which==0?"accounts.db.txn":which==1?"accounts.db.new":"bootstrap.state.new"); f.write_error=ENOSPC;
        struct acct_request q; struct acct_response r; fake_request(&q,ACCT_CREATE,s.db.generation,0,"Bob","password1");
        assert(fake_run(&s,1,&q,&r)==-ENOSPC && !s.available);
        acct_service_close(&s); fake_powercut(&f); f.write_error=0; struct acct_store st=fake_store(&f);
        int e=acct_service_open(&s,&st,1); assert(!e || e==-EIO);
        if(!e) acct_service_close(&s); fake_destroy(&f);
    }
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); acct_service_close(&s);
    int i=fake_file_index(&f,"accounts.db"); f.files[i].data[100]^=1; struct acct_store st=fake_store(&f);
    assert(acct_service_open(&s,&st,1)==-EIO); fake_destroy(&f);
    puts("PASS all mutation allocations, partial disk-full writes and corrupt canonical store fail closed with wiping");
}
struct submit_args { struct acct_service *s; unsigned *accepted; pthread_mutex_t *lock; struct acct_request auth; };
static void *thread_submit(void *v)
{
    struct submit_args *a=v; struct acct_request q={0}; struct acct_response r;
    q=a->auth; q.service=1;
    int e=acct_submit(a->s,0,&q,sizeof(q),&r);
    if(!e) { pthread_mutex_lock(a->lock); ++*a->accepted; pthread_mutex_unlock(a->lock); }
    else assert(e==-EAGAIN);
    assert(zeros(q.password,64)); return NULL;
}
static void concurrency(void)
{
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s);
    struct acct_request auth; fake_request(&auth,ACCT_AUTH,s.db.generation,1000,NULL,"");
    assert(!fake_challenge(&s,0,&auth));
    pthread_t threads[16]; unsigned accepted=0; pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    struct submit_args args={&s,&accepted,&lock,auth};
    for(unsigned i=0;i<16;++i) assert(!pthread_create(&threads[i],NULL,thread_submit,&args));
    for(unsigned i=0;i<16;++i) assert(!pthread_join(threads[i],NULL));
    assert(accepted>=1 && accepted<=2); struct acct_response r;
    for(unsigned i=0;i<accepted;++i) assert(acct_work(&s,&r)==1);
    assert(f.outcomes==1); assert(!pthread_mutex_destroy(&lock));
    acct_service_close(&s); fake_destroy(&f);
    /* Cancellation before and after COMMIT_REQUIRED. */
    for(unsigned late=0;late<2;++late) {
        fake_init(&f); fake_boot(&f,&s); password(&s,1000,"","password1");
        f.cancel_on_prepare=!late; f.revoke_after_commit=late;
        struct acct_request q; fake_request(&q,ACCT_CREATE,s.db.generation,0,"Bob","password1");
        assert(fake_run(&s,1,&q,&r)==-ESRCH);
        unsigned owner_used=f.owner_used; uint64_t g=f.owners[owner_used-1].request.transaction;
        assert(f.owners[owner_used-1].reply.state==(late?2u:3u));
        restart(&f,&s); assert(s.store.high_generation>=g && s.store.high_uid==1002);
        assert(s.db.count==(late?4u:3u));
        f.peers[1].alive=1; ++f.peers[1].epoch; f.cancel_on_prepare=f.revoke_after_commit=0;
        create_user(&s,"Next",0,ACCT_STANDARD); assert(s.db.records[s.db.count-1].uid==1002 && s.db.generation>g);
        acct_service_close(&s); fake_destroy(&f);
    }
    puts("PASS 16 concurrent clients, one active plus one pending, single-use auth, cancellation before/after irrevocable point and nonreuse");
}
static void faults(void)
{
    for(unsigned kind=0;kind<4;++kind) {
        unsigned boundaries=0, resumed=0, closed=0;
        for(unsigned cut=0;cut<=boundaries;++cut) {
            struct fake f; fake_init(&f); struct acct_service s; struct acct_store st=fake_store(&f);
            if(kind==0) { fake_provision(&f,1); assert(!acct_service_open(&s,&st,1)); }
            else { fake_boot(&f,&s); if(kind>1) password(&s,1000,"","password1"); }
            struct acct_db old=s.db; uint32_t known_sid[3]={0};
            f.event=0; f.cut=cut; int e;
            if(kind==0) {
                struct acct_db seed; fake_seed(&seed,1); uint8_t bytes[ACCT_FILE_MAX]; size_t n;
                assert(!acct_encode(&seed,1,bytes,sizeof(bytes),&n)); e=acct_service_bootstrap(&s,bytes,n,1);
            } else {
                struct acct_request q; struct acct_response r;
                if(kind==1) { fake_request(&q,ACCT_PASSWORD,s.db.generation,1000,NULL,"newpass12"); }
                else { fake_request(&q,ACCT_CREATE,s.db.generation,0,"Bob","password1"); q.flags=kind==2?1:0; }
                if(kind==3) f.cancel_on_prepare=1;
                e=fake_run(&s,1,&q,&r);
            }
            if(!cut) { assert(e==(kind==3?-ESRCH:0)); boundaries=f.event; }
            else assert(e==-EIO || e==-ESRCH);
            int sid=fake_file_index(&f,"machine.sid");
            if(sid>=0 && f.files[sid].durable_name[0]) for(unsigned i=0;i<3;++i) known_sid[i]=a32(f.files[sid].durable_data+8+4*i);
            acct_service_close(&s); fake_powercut(&f); f.cancel_on_prepare=0;
            e=acct_service_open(&s,&st,1);
            if(e) {
                assert(e==-EIO && kind==0); ++closed; assert(!f.leased);
                struct acct_bootstrap stopped; assert(!acct_boot_read(&st,&stopped));
                assert(stopped.state==ACCT_UNPROVISIONED && fake_file_index(&f,"accounts.db.txn")<0 &&
                    fake_file_index(&f,"accounts.alloc.0")>=0);
            }
            else {
                ++resumed;
                if(s.available) {
                    assert(!acct_validate(&s.db,0));
                    if(known_sid[0]) assert(!memcmp(known_sid,s.db.sid,12));
                    if(kind==1 && s.db.generation!=old.generation) assert((s.db.flags&4) && !s.db.auto_logon_uid);
                    if(!cut && kind==1) assert((s.db.flags&4) && s.db.generation>old.generation);
                    if(kind>=2 && s.db.count==4) assert(s.db.records[3].uid==1001 && s.store.high_uid>=1002);
                    struct acct_bootstrap b; assert(!acct_boot_read(&s.store,&b) && b.state==5);
                } else assert(kind==0 && !f.profile_count);
                acct_service_close(&s);
            }
            fake_destroy(&f);
        }
        printf("PASS power cut kind %u: all %u boundaries (%u recovered, %u safely closed); no partial vector or acknowledged rollback\n",kind,boundaries,resumed,closed);
    }
}
static void protocol(void)
{
    struct acct_begin begin={.action=2,.selector=2,.name="CiukiUser"}, decoded_begin;
    uint8_t b[312]={0}; assert(!acct_begin_encode(&begin,0,b,64)); assert(!acct_begin_decode(&decoded_begin,0,b,64));
    b[63]=1; assert(acct_begin_decode(&decoded_begin,0,b,64)==-EINVAL); b[63]=0;
    struct fake f; fake_init(&f); struct acct_service s; fake_boot(&f,&s); struct acct_request q; struct acct_response r;
    fake_request(&q,ACCT_AUTH,s.db.generation,0,"cIuKiUsEr",""); q.service=1; assert(!fake_challenge(&s,0,&q));
    struct acct_request saved=q; assert(q.challenge.ui_generation==1 && f.peers[0].subject.endpoint_generation==1);
    assert(!fake_run(&s,0,&q,&r));
    struct acct_auth_outcome decoded; assert(!acct_outcome_decode(&decoded,0,f.last_outcome,304));
    assert(decoded.challenge.uid==1000 && decoded.disposition==1 && a32(decoded.credential+8)==1000 && r.body_bytes==16);
    assert(acct_outcome_decode(&decoded,-EACCES,NULL,0)==-EINVAL);
    saved.service=1; assert(!acct_submit(&s,0,&saved,sizeof(saved),&r)); assert(acct_work(&s,&r)==1 && r.error==-EAGAIN);
    f.peers[0].subject.role=ACCT_ROLE_UI;
    fake_request(&q,ACCT_AUTH,s.db.generation,1000,NULL,""); q.service=1;
    assert(acct_submit(&s,0,&q,sizeof(q),&r)==-EPERM); f.peers[0].subject.role=1;
    for(unsigned kind=0;kind<7;++kind) {
        f.now+=1000; fake_request(&q,ACCT_AUTH,s.db.generation,1000,NULL,kind==0?"wrongpass":""); q.service=1;
        assert(!fake_challenge(&s,0,&q));
        if(kind==1) ++q.uid;
        if(kind==2) ++q.challenge.ui_generation;
        if(kind==3) ++f.console_generation;
        if(kind==4) ++f.authentication_epoch;
        if(kind==5) ++f.account_generation;
        if(kind==6) f.now+=5000;
        assert(!acct_submit(&s,0,&q,sizeof(q),&r)); assert(acct_work(&s,&r)==1);
        if(kind==0) { assert(!r.error && (int32_t)a32(r.body+8)==-EACCES); assert(!acct_outcome_decode(&decoded,0,f.last_outcome,304));
            assert(decoded.disposition==4 && zeros(decoded.credential,64)); }
        else assert(r.error || (r.body_bytes==16 && (int32_t)a32(r.body+8)<0));
        if(kind==5) --f.account_generation;
    }
    f.now+=1000;
    struct acct_transport t={0}; uint8_t frame[256]={0}; w32(frame,0x31435343); w32(frame+4,1); w32(frame+8,64);
    w32(frame+12,2); w32(frame+16,6); w32(frame+20,1); w64(frame+24,1); w32(frame+32,2);
    assert(!acct_frame(&t,&s,1,frame,64,&r)); assert(acct_work(&s,&r)==1 && r.count==3);
    assert(acct_frame(&t,&s,1,frame,64,&r)==-EINVAL);
    w64(frame+24,2); w32(frame+52,312); w32(frame+36,192); assert(acct_frame(&t,&s,1,frame,256,&r)==1);
    assert(!acct_transport_cancel(&t,&s,1)); /* copied first fragment => unknown, never rollback proof */
    memset(frame+64,0,192); w32(frame+52,0); w32(frame+36,0); w64(frame+24,3);
    assert(!acct_frame(&t,&s,1,frame,64,&r)); assert(acct_work(&s,&r)==1);
    uint8_t event[32],progress[16]; struct acct_ui_event lock={.action=1,.session=1};
    assert(!acct_event_encode(5,0,&lock,event,32)); struct acct_subject subject;
    f.peers[4].subject.role=6; assert(!s.store.kernel.retain_peer(&f,4,&subject));
    assert(!acct_logon_session_lock(&s.store.kernel,&subject,event,32,progress)); unsigned epoch=f.authentication_epoch;
    assert(!acct_logon_session_lock(&s.store.kernel,&subject,event,32,progress) && f.authentication_epoch==epoch);
    s.store.kernel.release_peer(&f,&subject);
    for(unsigned role=0;role<=7;++role) if(role!=6) {
        f.peers[4].subject.role=role; assert(!s.store.kernel.retain_peer(&f,4,&subject));
        assert(acct_logon_session_lock(&s.store.kernel,&subject,event,32,progress)==-EACCES); s.store.kernel.release_peer(&f,&subject);
    }
    acct_service_close(&s); fake_destroy(&f);
    puts("PASS exact selectors, LOGON relay, correlated failure/success, role/epoch/replay/timeout refusal, transport and own-DESKTOP lock");
}
void round2_checkpoint_faults(void);
void round2_cancellation(void);
void round2_logon_relay(void);
void round2_manifest(void);
void round2_wire_oracle(const char *);
void round2_results(void);
void round2_owner(void);
void round2_profile(void);
void round2_autologon(void);
void round2_enrollment(void);
void round3_faulted_queue(void);
void round3_root_collision(void);
void round3_enrollment_outcome(void);
void round3_logon_restart(void);
void round3_distinct_handles(void);
void round3_intent_bounds(void);
int main(int argc, char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc>=2);
    if(!strcmp(argv[1],"codec")) { assert(argc==3); codec(argv[2]); }
    else if(!strcmp(argv[1],"crypto")) { assert(argc==3); crypto(argv[2]); }
    else if(!strcmp(argv[1],"migration")) migration();
    else if(!strcmp(argv[1],"migration-batch")) migration_batch();
    else if(!strcmp(argv[1],"bootstrap-codec")) { assert(argc==3); bootstrap_codec(argv[2]); }
    else if(!strcmp(argv[1],"service")) service();
    else if(!strcmp(argv[1],"retirement")) retirement();
    else if(!strcmp(argv[1],"setup-seed")) setup_seed();
    else if(!strcmp(argv[1],"batch")) batch(argc==3?argv[2]:NULL);
    else if(!strcmp(argv[1],"failures")) failure_inputs();
    else if(!strcmp(argv[1],"capacity")) capacity();
    else if(!strcmp(argv[1],"disk-oom")) disk_oom_corruption();
    else if(!strcmp(argv[1],"concurrency")) concurrency();
    else if(!strcmp(argv[1],"faults")) faults();
    else if(!strcmp(argv[1],"protocol")) protocol();
    else if(!strcmp(argv[1],"round2-checkpoint-faults")) round2_checkpoint_faults();
    else if(!strcmp(argv[1],"round2-cancellation")) round2_cancellation();
    else if(!strcmp(argv[1],"round2-logon")) round2_logon_relay();
    else if(!strcmp(argv[1],"round2-manifest")) round2_manifest();
    else if(!strcmp(argv[1],"round2-wire")) { assert(argc==3); round2_wire_oracle(argv[2]); }
    else if(!strcmp(argv[1],"round2-results")) round2_results();
    else if(!strcmp(argv[1],"round2-owner")) round2_owner();
    else if(!strcmp(argv[1],"round2-profile")) round2_profile();
    else if(!strcmp(argv[1],"round2-autologon")) round2_autologon();
    else if(!strcmp(argv[1],"round2-enrollment")) round2_enrollment();
    else if(!strcmp(argv[1],"round3-faulted-queue")) round3_faulted_queue();
    else if(!strcmp(argv[1],"round3-root-collision")) round3_root_collision();
    else if(!strcmp(argv[1],"round3-enrollment-outcome")) round3_enrollment_outcome();
    else if(!strcmp(argv[1],"round3-logon-restart")) round3_logon_restart();
    else if(!strcmp(argv[1],"round3-distinct-handles")) round3_distinct_handles();
    else if(!strcmp(argv[1],"round3-intent-bounds")) round3_intent_bounds();
    else assert(0);
    return 0;
}
