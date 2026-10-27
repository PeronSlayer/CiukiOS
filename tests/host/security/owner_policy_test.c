/* Production credentials/channel fixture; fake storage is the only OWNER
 * state boundary. SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_HOST_MAIN owner_unused_desktop_main
#ifdef OWNER_REAL_VFS_TEST
#define CIUKI_DESKTOP_REAL_VFS
#endif
#include "../proc/desktop_test.c"
#include "../../../src/kernel/fs/owner_internal.h"
#include "../../../src/kernel/fs/security_policy.h"
#include "../../../src/kernel/fs/owner_channel.h"
struct blob { uint8_t *data; size_t bytes; };
struct owner_fake {
    struct blob files[5], durable[5], manifest, checkpoint;
    struct owner_request request;
    unsigned events, fail, after, published, seals;
    int intent, bad_tree, bad_map, marker;
    uint64_t accounts;
    uint32_t uid_base;
    uint32_t old_ids[32];
    uint8_t account_digest[32];
};
static struct security_subject *account_peer;
static struct security_subject alice={.credential={.version=1,.size=64,.uid=1000,.group_count=1,.groups={545},.flags=1},.live=true};
static struct security_subject bob={.credential={.version=1,.size=64,.uid=1001,.group_count=1,.groups={545},.flags=1},.live=true};
static struct security_subject guest={.credential={.version=1,.size=64,.uid=501,.group_count=1,.groups={546},.flags=1},.live=true};
static struct security_subject admin={.credential={.version=1,.size=64,.uid=500,.group_count=2,.groups={544,545},.flags=1},.live=true};
static const uint32_t owner_sid[3]={17,31,99};
static void blob_put(struct blob *b,const void *v,size_t n)
{ free(b->data); b->data=malloc(n?n:1); CHECK(b->data); memcpy(b->data,v,n); b->bytes=n; }
static int fake_event(struct owner_fake *f,bool after)
{ if(!after) f->events++; return f->fail && f->events==f->fail && !!f->after==after?-EIO:0; }
static int owner_read(void *c,unsigned i,void *p,size_t cap,size_t *n)
{ struct owner_fake *f=c; if(!f->files[i].data) return -ENOENT; if(cap<f->files[i].bytes) return -ENOSPC; *n=f->files[i].bytes; memcpy(p,f->files[i].data,*n); return 0; }
static int owner_write(void *c,unsigned i,const void *p,size_t n)
{ struct owner_fake *f=c; int e=fake_event(f,false); if(e) return e; blob_put(&f->files[i],p,n); return fake_event(f,true); }
static int owner_flush(void *c,unsigned i)
{ struct owner_fake *f=c; int e=fake_event(f,false); if(e) return e; blob_put(&f->durable[i],f->files[i].data,f->files[i].bytes); return fake_event(f,true); }
static int owner_barrier(void *c)
{ struct owner_fake *f=c; int e=fake_event(f,false); return e?e:fake_event(f,true); }
static int stage_read(void *c,uint64_t g,bool cp,void *p,size_t cap,size_t *n)
{ struct owner_fake *f=c; CHECK(g); struct blob *b=cp?&f->checkpoint:&f->manifest; if(!b->data) return -ENOENT; if(cap<b->bytes) return -ENOSPC; *n=b->bytes; memcpy(p,b->data,*n); return 0; }
static int tree_read(void *c,uint64_t g,uint32_t uid,uint8_t digest[32])
{ struct owner_fake *f=c; (void)g; memset(digest,(int)(uid%251+1),32); if(f->bad_tree) digest[0]^=1; return 0; }
static int root_seal(void *c,const struct owner_request *q,struct owner_record *r,bool recheck)
{ struct owner_fake *f=c; f->seals++; for(unsigned i=0;i<q->count;i++) { uint8_t proof[32]; memset(proof,(int)(q->bindings[i].uid%251+1),32); if(recheck && memcmp(r[i].proof,proof,32)) return -EIO; if(!recheck) memcpy(r[i].proof,proof,32); } return 0; }
static int root_publish(void *c,const struct owner_request *q,struct owner_record *r)
{ struct owner_fake *f=c; (void)r; int e=fake_event(f,false); if(e) return e; f->published=q->count; return fake_event(f,true); }
static int root_release(void *c,const struct owner_request *q,bool committed)
{ (void)c; (void)q; (void)committed; return 0; }
static int account_intent(void *c,const struct owner_request *q)
{ struct owner_fake *f=c; CHECK(q->transaction==f->request.transaction); return f->intent; }
static int account_now(void *c,uint64_t *g,uint8_t digest[32])
{ struct owner_fake *f=c; *g=f->accounts; memcpy(digest,f->account_digest,32); return 0; }
static int checkpoint_marker(void *c,const struct owner_request *q,uint64_t high,uint32_t uid)
{ struct owner_fake *f=c; CHECK(high>=q->transaction && uid>=1000); return f->marker?0:-ENOENT; }
static int import_map(void *c,uint64_t g,uint32_t old,uint32_t uid,const char *prior,const char *dest)
{
    struct owner_fake *f=c; (void)g; CHECK(old && uid>=1000 && prior && dest);
    if(f->bad_map) return -EACCES;
    for(unsigned i=0;i<f->request.count;i++) if(f->request.bindings[i].uid==uid && f->old_ids[i]==old && !strcmp(f->request.bindings[i].path,dest)) return 0;
    return -EACCES;
}
static int root_occupied(void *c,const char *path) { (void)c; return !strcmp(path,"/Windows/Profiles/u0000ffff"); }
static int map_verify(void *c,const struct owner_record *r) { (void)c; return ozero(r->proof,32)?-EIO:0; }
static struct owner_backend fake_backend(struct owner_fake *f)
{ return (struct owner_backend){f,owner_read,owner_write,owner_flush,owner_barrier,stage_read,tree_read,root_seal,root_publish,root_release,account_intent,account_now,checkpoint_marker,import_map,root_occupied,map_verify}; }
static void fake_destroy(struct owner_fake *f)
{ for(unsigned i=0;i<5;i++) { free(f->files[i].data); free(f->durable[i].data); } free(f->manifest.data); free(f->checkpoint.data); memset(f,0,sizeof(*f)); }
static void fake_cut(struct owner_fake *f)
{ f->fail=0; f->events=0; for(unsigned i=0;i<5;i++) { free(f->files[i].data); f->files[i]=(struct blob){0}; if(f->durable[i].data) blob_put(&f->files[i],f->durable[i].data,f->durable[i].bytes); } }
static void fresh_store(struct owner_fake *f,struct owner_store *s)
{ memset(f,0,sizeof(*f)); memset(f->account_digest,7,32); struct owner_backend io=fake_backend(f); CHECK(!owner_open(s,&io,owner_sid,31,true)); }
static void make_batch(struct owner_fake *f,unsigned count,uint64_t g,bool imported)
{
    struct owner_request *q=&f->request; memset(q,0,sizeof(*q)); memcpy(q->sid,owner_sid,12);
    memset(f->old_ids,0,sizeof(f->old_ids));
    q->volume=31; q->transaction=g; q->count=count; q->accounts_generation=f->accounts;
    memset(q->accounts_digest,9,32); size_t n=64+256*count; uint8_t *manifest=calloc(1,n); CHECK(manifest);
    memcpy(manifest,"CIUKISTG",8); fs_wr32(manifest+8,1); fs_wr32(manifest+12,64); fs_wr32(manifest+16,count);
    memcpy(manifest+24,owner_sid,12); op64(manifest+40,31); op64(manifest+48,g);
    for(unsigned i=0;i<count;i++) {
        uint32_t uid=i==0?500:i==1?501:(f->uid_base?f->uid_base:1000)+i-2;
        struct owner_binding *b=&q->bindings[i]; *b=(struct owner_binding){.uid=uid,.action=OWNER_CREATE,.volume=31};
        ksnprintf(b->path,64,"/Windows/Profiles/u%08x",imported && i>=2?i-1:uid);
        uint8_t *p=manifest+64+256*i; fs_wr32(p,uid); fs_wr32(p+8,1); fs_wr32(p+12,imported && i>=2?2:1);
        memcpy(p+16,b->path,64);
        if(imported && i>=2) { f->old_ids[i]=i-1; fs_wr32(p+4,i-1); memcpy(p+80,b->path,64); }
        ksnprintf((char *)p+144,64,"/System/Security/Staging/g%016llx/p%08x",(unsigned long long)g,uid);
        tree_read(f,g,uid,p+208);
    }
    sha256(manifest,n,q->manifest_digest); blob_put(&f->manifest,manifest,n); free(manifest);
}
static int call(struct owner_store *s,struct owner_request *q,unsigned op,uint8_t reply[OWNER_REPLY_MAX])
{
    uint8_t body[OWNER_REQUEST_MAX]; size_t n=0,bytes=99;
    int e=owner_request_encode(q,op,body,sizeof(body),&n); CHECK(!e); memcpy(q->digest,body+128,32);
    e=owner_call(s,account_peer,body,n,reply,OWNER_REPLY_MAX,&bytes);
    if(e) CHECK(!bytes); else { CHECK(bytes==96+32*fs_rd32(reply+12)); CHECK(!memcmp(reply+64,q->digest,32)); }
    return e;
}
static void test_batches_and_cuts(unsigned count,bool imported)
{
    struct owner_fake f; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); uint8_t reply[OWNER_REPLY_MAX];
    fresh_store(&f,s); make_batch(&f,count,1,imported);
    CHECK(!call(s,&f.request,OWNER_RESERVE,reply) && fs_rd32(reply+8)==OWNER_RESERVED);
    uint64_t generation=ow64(reply+48), key=ow64(reply+104);
    CHECK(generation==1 && key==1 && s->locator_high==count);
    CHECK(owner_root_occupied(s,31,f.request.bindings[count-1].path)==1);
    CHECK(!call(s,&f.request,OWNER_PREPARE,reply)); CHECK(call(s,&f.request,OWNER_COMMIT,reply)==-EIO);
    f.intent=1; f.events=0; CHECK(!call(s,&f.request,OWNER_COMMIT,reply)); unsigned steps=f.events;
    CHECK(s->profile_count==count && s->generation==1 && !s->active);
    CHECK(!call(s,&f.request,OWNER_COMMIT,reply) && ow64(reply+104)==key);
    CHECK(call(s,&f.request,OWNER_ABORT,reply)==-EBUSY);
    struct owner_record r; CHECK(!owner_profile(s,1000,&r)); CHECK(r.binding.uid==1000);
    if(imported) CHECK(!strcmp(r.binding.path,"/Windows/Profiles/u00000001"));
    owner_close(s); fake_destroy(&f);
    /* Interrupt the entire transaction, including reservation/preparation, then
     * recover either all original bindings or a closed/aborted old state. */
    for(unsigned after=0;after<2;after++) for(unsigned cut=1;cut<=steps+12;cut++) {
        fresh_store(&f,s); make_batch(&f,count,1,imported); f.events=0; f.fail=cut; f.after=after;
        int e=call(s,&f.request,OWNER_RESERVE,reply);
        if(!e) e=call(s,&f.request,OWNER_PREPARE,reply);
        if(!e) { f.intent=1; e=call(s,&f.request,OWNER_COMMIT,reply); }
        bool acknowledged=!e; owner_close(s); fake_cut(&f); struct owner_backend io=fake_backend(&f);
        int opened=owner_open(s,&io,owner_sid,31,false);
        if(!opened) {
            CHECK(s->profile_count==0 || s->profile_count==count);
            if(acknowledged || f.intent) CHECK(s->profile_count==count);
            if(!s->profile_count && s->key_high) {
                CHECK(s->key_high==count && s->locator_high==count && s->generation_high==1);
                CHECK(!call(s,&f.request,OWNER_STATUS,reply) && fs_rd32(reply+8)==OWNER_ABORTED);
            }
        } else CHECK(!acknowledged && opened==-EIO);
        owner_close(s); fake_destroy(&f);
    }
    free(s); printf("H02 batch %u%s: all writes/flushes before+after power cuts, all-or-closed recovery PASS\n",count,imported?" (16 imports + builtins)":"");
}
static void test_invalid_and_receipts(void)
{
    struct owner_fake f; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); uint8_t reply[OWNER_REPLY_MAX];
    memset(&f,0,sizeof(f)); struct owner_backend absent=fake_backend(&f);
    CHECK(owner_open(s,&absent,owner_sid,31,false)==-EIO); owner_close(s);
    fresh_store(&f,s); make_batch(&f,3,1,false); unsigned before=f.events;
    uint8_t wire[OWNER_REQUEST_MAX]; size_t wire_bytes=0,reply_bytes=7;
    CHECK(!owner_request_encode(&f.request,OWNER_RESERVE,wire,sizeof(wire),&wire_bytes));
    CHECK(owner_call(s,0,wire,wire_bytes,reply,sizeof(reply),&reply_bytes)==-EACCES && !reply_bytes);
    CHECK(owner_call(s,&alice,wire,wire_bytes,reply,sizeof(reply),&reply_bytes)==-EACCES);
    CHECK(owner_call(s,&system_subject,wire,wire_bytes,reply,sizeof(reply),&reply_bytes)==-EACCES);
    const unsigned bad_wire[]={0,4,12,28,64,128,160+32,160+40,160+44,160+112};
    for(unsigned i=0;i<sizeof(bad_wire)/sizeof(*bad_wire);i++) {
        wire[bad_wire[i]]^=1; memset(reply,0xa5,sizeof(reply));
        CHECK(owner_call(s,account_peer,wire,wire_bytes,reply,sizeof(reply),&reply_bytes)<0 && !reply_bytes);
        for(unsigned j=0;j<sizeof(reply);j++) CHECK(reply[j]==0xa5);
        CHECK(f.events==before && !s->receipt_count); wire[bad_wire[i]]^=1;
    }
    CHECK(owner_call(s,account_peer,wire,wire_bytes,reply,96,&reply_bytes)==-ENOSPC && f.events==before);
    const unsigned bad_manifest[]={0,8,12,16,20,24,40,48,64,64+12,64+16,64+80,64+144,64+208,64+240,64+248};
    uint8_t original_digest[32]; memcpy(original_digest,f.request.manifest_digest,32);
    for(unsigned i=0;i<sizeof(bad_manifest)/sizeof(*bad_manifest);i++) {
        unsigned offset=bad_manifest[i]; f.manifest.data[offset]^=1;
        CHECK(call(s,&f.request,OWNER_RESERVE,reply)==-EIO && f.events==before);
        sha256(f.manifest.data,f.manifest.bytes,f.request.manifest_digest);
        CHECK(call(s,&f.request,OWNER_RESERVE,reply)<0 && f.events==before);
        f.manifest.data[offset]^=1; memcpy(f.request.manifest_digest,original_digest,32);
    }
    f.bad_tree=1; CHECK(call(s,&f.request,OWNER_RESERVE,reply)==-EIO && f.events==before); f.bad_tree=0;
    f.request.bindings[0].locator=42; CHECK(owner_request_encode(&f.request,7,reply,sizeof(reply),&(size_t){0})==-EINVAL); f.request.bindings[0].locator=0;
    fs_host_contig_fail=1; CHECK(call(s,&f.request,OWNER_RESERVE,reply)==-ENOMEM && f.events==before); fs_host_contig_fail=0;
    CHECK(!call(s,&f.request,OWNER_ABORT,reply) && fs_rd32(reply+8)==OWNER_UNKNOWN && !s->receipt_count);
    CHECK(!call(s,&f.request,OWNER_RESERVE,reply)); CHECK(!call(s,&f.request,OWNER_ABORT,reply));
    CHECK(s->key_high==3 && s->locator_high==3 && s->generation_high==1 && !s->profile_count && s->receipt_count==1);
    struct owner_request old=f.request; f.uid_base=1001; make_batch(&f,3,2,false); f.request.owner_generation=0;
    CHECK(!call(s,&f.request,OWNER_RESERVE,reply) && ow64(reply+104)==4 && ow64(reply+48)==2);
    CHECK(!call(s,&old,OWNER_STATUS,reply) && fs_rd32(reply+8)==OWNER_ABORTED);
    old.accounts_digest[0]^=1; CHECK(call(s,&old,OWNER_STATUS,reply)==-EIO);
    CHECK(!call(s,&f.request,OWNER_ABORT,reply));
    unsigned slot=(unsigned)(s->sequence&1); CHECK(f.durable[slot].bytes); f.durable[slot].data[f.durable[slot].bytes-1]^=1;
    owner_close(s); fake_cut(&f); struct owner_backend corrupt=fake_backend(&f);
    CHECK(owner_open(s,&corrupt,owner_sid,31,false)==-EIO); owner_close(s); fake_destroy(&f); free(s);
    puts("H02 immutable retry, zero locators, missing evidence, allocation failure, early ABORT and nonreuse PASS");
}
static void checkpoint_run(struct owner_fake *f,struct owner_store *s)
{
    size_t cap=owner_store_bound(), n=0; uint8_t *image=malloc(cap); CHECK(image); uint64_t floor;
    CHECK(!owner_checkpoint_image(s,f->accounts,f->account_digest,s->transaction_high,s->uid_high,image,cap,&n,&floor));
    blob_put(&f->checkpoint,image,n); free(image);
    struct owner_request q={.volume=31,.transaction=floor,.accounts_generation=f->accounts,.owner_generation=s->generation};
    memcpy(q.sid,owner_sid,12); memcpy(q.accounts_digest,f->account_digest,32); sha256(f->checkpoint.data,f->checkpoint.bytes,q.manifest_digest);
    uint8_t reply[OWNER_REPLY_MAX]; CHECK(!call(s,&q,OWNER_CHECKPOINT,reply)); CHECK(s->candidate && s->floor<floor);
    f->marker=1; CHECK(!call(s,&q,OWNER_COMPACT,reply)); CHECK(s->floor==floor && !s->candidate && s->receipt_count==256);
    CHECK(!call(s,&q,OWNER_COMPACT,reply));
}
static void test_compaction(void)
{
    struct owner_fake f; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); uint8_t reply[OWNER_REPLY_MAX];
    fresh_store(&f,s); struct owner_request oldest={0};
    for(unsigned i=1;i<=2305;i++) {
        if(s->receipt_count>=768) {
            make_batch(&f,1,i,false); unsigned before=f.events;
            CHECK(call(s,&f.request,OWNER_RESERVE,reply)==-EAGAIN && f.events==before);
            checkpoint_run(&f,s);
        }
        make_batch(&f,1,i,false); if(i==1) oldest=f.request;
        CHECK(!call(s,&f.request,OWNER_RESERVE,reply)); CHECK(!call(s,&f.request,OWNER_ABORT,reply));
    }
    CHECK(s->profile_count==0 && s->key_high==2305 && s->generation_high==2305);
    CHECK(!call(s,&oldest,OWNER_STATUS,reply) && fs_rd32(reply+8)==OWNER_CHECKPOINTED && !fs_rd32(reply+12) && !ow64(reply+48));
    oldest.accounts_digest[0]^=1; CHECK(!call(s,&oldest,OWNER_PREPARE,reply) && fs_rd32(reply+8)==OWNER_CHECKPOINTED);
    CHECK(call(s,&oldest,OWNER_COMMIT,reply)==-EIO);
    owner_close(s); fake_cut(&f); struct owner_backend io=fake_backend(&f); CHECK(!owner_open(s,&io,owner_sid,31,false));
    CHECK(s->generation_high==2305 && s->floor>0); owner_close(s); fake_destroy(&f); free(s);
    puts("H02 2305 RESERVED->ABORTED receipts: checkpoints, compaction, persisted floors and nonreplay PASS");
}
static void test_creator_roots(void)
{
    struct owner_fake f; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); fresh_store(&f,s);
    struct owner_record r; uint8_t proof[32]; memset(proof,55,32);
    CHECK(owner_root_reserve(s,&guest,"/GAME",proof,&r)==-EACCES);
    CHECK(!owner_root_reserve(s,&alice,"/GAME",proof,&r)); CHECK(owner_label(s,r.binding.locator,&r)==-EACCES);
    CHECK(!owner_root_finish(s,s->root_key,proof,false)); CHECK(!owner_label(s,s->roots[0].binding.locator,&r));
    CHECK(owner_root_change(s,&bob,r.binding.key,"/Moved",proof)==-EACCES);
    CHECK(!owner_root_change(s,&alice,r.binding.key,"/Moved",proof));
    CHECK(owner_root_occupied(s,31,"/game")==1 && owner_root_occupied(s,31,"/moved")==1);
    owner_close(s); fake_cut(&f); struct owner_backend io=fake_backend(&f);
    CHECK(!owner_open(s,&io,owner_sid,31,false));
    CHECK(owner_root_occupied(s,31,"/GAME")==1 && owner_root_occupied(s,31,"/Moved")==1);
    CHECK(!owner_root_finish(s,s->root_key,proof,true));
    CHECK(owner_root_occupied(s,31,"/GAME")==1 && !owner_root_occupied(s,31,"/Moved"));
    CHECK(!owner_root_change(s,&alice,r.binding.key,"/Moved",proof)); CHECK(!owner_root_finish(s,s->root_key,proof,false));
    CHECK(owner_root_occupied(s,31,"/Moved")==1 && !owner_root_occupied(s,31,"/GAME"));
    CHECK(!strcmp(s->roots[0].binding.path,"/Moved")); CHECK(!owner_root_change(s,&admin,r.binding.key,0,proof));
    CHECK(!owner_root_finish(s,s->root_key,proof,false)); CHECK(s->roots[0].retired);
    CHECK(owner_root_occupied(s,31,"/Moved")==1);
    for(unsigned i=1;i<256;i++) { char name[64]; ksnprintf(name,64,"/game%u",i); CHECK(!owner_root_reserve(s,&alice,name,proof,&r)); CHECK(!owner_root_finish(s,r.binding.key,proof,false)); }
    CHECK(owner_root_reserve(s,&alice,"/overflow",proof,&r)==-ENOSPC && s->root_count==256);
    owner_close(s); fake_cut(&f); CHECK(!owner_open(s,&io,owner_sid,31,false));
    CHECK(s->root_count==256 && s->roots[0].retired && s->key_high==256); owner_close(s); fake_destroy(&f); free(s);
    puts("H02 creator roots: owner/admin rename/delete, reserved-name restart/abort, Guest denial, 256 bound and durable retirement PASS");
}
static struct policy_label policy_path(struct policy_volume *v,const char *path,uint64_t locator)
{
    struct policy_label label; CHECK(!policy_root(v,1,&label)); const char *p=path;
    while(*p) {
        while(*p=='/') p++; if(!*p) break;
        char component[64]; unsigned n=0; while(*p && *p!='/') component[n++]=*p++; component[n]=0;
        CHECK(!policy_child(v,&label,component,label.object+1,*p?0:locator,&label));
    }
    return label;
}
static void test_matrix(void)
{
    struct owner_fake f; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); fresh_store(&f,s); make_batch(&f,3,1,false);
    uint8_t reply[OWNER_REPLY_MAX]; CHECK(!call(s,&f.request,OWNER_RESERVE,reply)); CHECK(!call(s,&f.request,OWNER_PREPARE,reply)); f.intent=1; CHECK(!call(s,&f.request,OWNER_COMMIT,reply));
    struct owner_record creator; uint8_t root_proof[32]; memset(root_proof,9,32);
    CHECK(!owner_root_reserve(s,&alice,"/GAME",root_proof,&creator)); CHECK(!owner_root_finish(s,creator.binding.key,root_proof,false));
    struct policy_volume v; CHECK(!policy_init(&v,31,1,true,false,false,s));
    const struct security_subject *subjects[]={&system_subject,&admin,&alice,&bob,&guest};
    /* Independent literal rights oracle: bits correspond to the five subjects
     * above. Do not infer expected rights from the implementation's labels. */
    static const struct { const char *path; unsigned read,write; } expected[]={
        {"/Windows",31,3},{"/Windows/Profiles",31,1},{"/Windows/Profiles/Default",31,3},
        {"/Windows/Profiles/All Users",31,3},{"/System",31,3},{"/System/Security",1,1},
        {"/System/Services",31,1},{"/System/Drivers",31,1},{"/System/Registry",1,1},
        {"/System/Settings",31,3},{"/System/Logs",1,1},{"/System/Settings/Hardware",31,1},
        {"/System/Settings/users.dat",1,1},{"/System/Settings/users.new",1,1},{"/System/Settings/users.lock",1,1},
        {"/Windows/USER.CRG",1,1},{"/System/PAGEFILE.SWP",1,1},{"/System/VMM.ELF",31,1},
        {"/System/BOOT.CFG",31,1},{"/Programs",31,3},{"/Programs/setup",31,1},
        {"/Program Files",31,3},{"/Documents",31,15},{"/Documents/Desktop",31,15},
        {"/Documents/Desktop/Quake III Arena Demo",31,3},
        {"/CIUKILDR",31,1},{"/BOOT.CFG",31,1}
    };
    CHECK(v.count==1+sizeof(expected)/sizeof(*expected));
    for(unsigned rule=0;rule<sizeof(expected)/sizeof(*expected);rule++) {
        struct policy_label label=policy_path(&v,expected[rule].path,0);
        for(unsigned who=0;who<5;who++) {
            CHECK((policy_check(&v,&label,subjects[who],POLICY_READ)==0)==!!(expected[rule].read&(1u<<who)));
            CHECK((policy_check(&v,&label,subjects[who],POLICY_WRITE)==0)==!!(expected[rule].write&(1u<<who)));
            CHECK((policy_check(&v,&label,subjects[who],POLICY_RENAME)==0)==(who==0));
        }
    }
    struct policy_label profile=policy_path(&v,"/Windows/Profiles/u000003e8",s->profiles[2].binding.locator);
    CHECK(!policy_check(&v,&profile,&alice,POLICY_WRITE)); CHECK(policy_check(&v,&profile,&bob,POLICY_READ)==-EACCES);
    CHECK(policy_check(&v,&profile,&guest,POLICY_READ)==-EACCES); CHECK(!policy_check(&v,&profile,&admin,POLICY_WRITE));
    struct policy_label guest_profile=policy_path(&v,"/Windows/Profiles/u000001f5",s->profiles[1].binding.locator);
    CHECK(!policy_check(&v,&guest_profile,&guest,POLICY_WRITE)); CHECK(policy_check(&v,&guest_profile,&alice,POLICY_READ)==-EACCES);
    struct policy_label game=policy_path(&v,"/GAME",creator.binding.locator);
    for(unsigned who=0;who<5;who++) {
        CHECK(!policy_check(&v,&game,subjects[who],POLICY_READ));
        CHECK((policy_check(&v,&game,subjects[who],POLICY_WRITE)==0)==!!(7u&(1u<<who)));
        CHECK((policy_check(&v,&game,subjects[who],POLICY_REMOVE)==0)==!!(7u&(1u<<who)));
    }
    struct policy_label registry; CHECK(!policy_child(&v,&profile,"Settings",11,0,&registry)); CHECK(!policy_child(&v,&registry,"Registry",12,0,&registry));
    for(unsigned i=1;i<5;i++) CHECK(policy_check(&v,&registry,subjects[i],POLICY_READ)==-EACCES);
    CHECK(!policy_check(&v,&registry,&system_subject,POLICY_WRITE));
    static const char *const hives[]={"SYSTEM.CRG","SYSTEM.CRG.0","SYSTEM.CRG.1","USER.CRG","USER.CRG.0","USER.CRG.1"};
    for(unsigned scope=0;scope<3;scope++) for(unsigned file=0;file<6;file++) {
        struct policy_label parent=scope==0?policy_path(&v,"/Windows",0):scope==1?profile:registry, hive;
        CHECK(!policy_child(&v,&parent,hives[file],100+file,0,&hive));
        for(unsigned who=0;who<5;who++) for(unsigned bit=1;bit<=POLICY_META;bit<<=1)
            CHECK((policy_check(&v,&hive,subjects[who],bit)==0)==(who==0));
    }
    struct policy_label shared=policy_path(&v,"/Documents/Desktop/notes.txt",0);
    struct policy_label launch=policy_path(&v,"/Documents/Desktop/Quake III Arena Demo/Install Quake III Arena Demo.lnk",0);
    for(unsigned who=0;who<5;who++) {
        CHECK((policy_check(&v,&shared,subjects[who],POLICY_WRITE|POLICY_REMOVE|POLICY_RENAME)==0)==(who<4));
        CHECK((policy_check(&v,&launch,subjects[who],POLICY_WRITE|POLICY_REMOVE|POLICY_RENAME)==0)==(who<2));
    }
    struct policy_label preferences;
    CHECK(!policy_child(&v,&profile,"Documents",20,0,&preferences));
    CHECK(!policy_child(&v,&preferences,"Settings",21,0,&preferences));
    CHECK(!policy_child(&v,&preferences,"Registry",22,0,&preferences));
    CHECK(!policy_check(&v,&preferences,&alice,POLICY_WRITE));
    CHECK(policy_child(&v,&profile,"foreign",23,s->profiles[0].binding.locator,&preferences)==-EIO);
    struct policy_label root; CHECK(!policy_root(&v,1,&root));
    CHECK(!policy_check(&v,&root,&alice,POLICY_ADD)); CHECK(policy_check(&v,&root,&guest,POLICY_ADD)==-EACCES);
    struct policy_label public=policy_path(&v,"/Systematic",0); CHECK(policy_check(&v,&public,&alice,POLICY_WRITE)==-EACCES);
    unsigned events=f.events; for(unsigned i=0;i<1000000;i++) CHECK(!policy_check(&v,&profile,&alice,POLICY_READ)); CHECK(f.events==events);
    CHECK(policy_check(&v,&profile,0,POLICY_READ)==-EACCES); bob.live=false; CHECK(policy_check(&v,&public,&bob,POLICY_READ)==-ESRCH); bob.live=true;
    struct policy_volume removable,readonly; CHECK(!policy_init(&removable,40,2,false,true,false,0)); CHECK(!policy_init(&readonly,41,3,false,false,true,0));
    struct policy_label media; CHECK(!policy_root(&removable,1,&media)); CHECK(!policy_check(&removable,&media,&alice,POLICY_WRITE)); CHECK(policy_check(&removable,&media,&guest,POLICY_WRITE)==-EACCES);
    CHECK(!policy_root(&readonly,1,&media)); for(unsigned i=0;i<5;i++) CHECK(policy_check(&readonly,&media,subjects[i],POLICY_WRITE)==-EROFS);
    printf("H02 matrix: all %u static trie nodes x SYSTEM/admin/owner/other/Guest; private hives, root ADD, removable/read-only; 1000000 warm checks, no I/O PASS\n",v.count);
    owner_close(s); fake_destroy(&f); free(s);
}
static void fake_clone(struct owner_fake *to,const struct owner_fake *from)
{
    *to=*from; to->events=to->fail=0;
    for(unsigned i=0;i<5;i++) { to->files[i]=(struct blob){0}; to->durable[i]=(struct blob){0}; if(from->files[i].data) blob_put(&to->files[i],from->files[i].data,from->files[i].bytes); if(from->durable[i].data) blob_put(&to->durable[i],from->durable[i].data,from->durable[i].bytes); }
    to->manifest=(struct blob){0}; to->checkpoint=(struct blob){0};
    if(from->manifest.data) blob_put(&to->manifest,from->manifest.data,from->manifest.bytes);
    if(from->checkpoint.data) blob_put(&to->checkpoint,from->checkpoint.data,from->checkpoint.bytes);
}
static void test_checkpoint_cuts(void)
{
    struct owner_fake baseline; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); fresh_store(&baseline,s); uint8_t reply[OWNER_REPLY_MAX];
    struct owner_record root; uint8_t proof[32]; memset(proof,77,32); CHECK(!owner_root_reserve(s,&alice,"/GAME",proof,&root)); CHECK(!owner_root_finish(s,root.binding.key,proof,false));
    for(unsigned i=1;i<=257;i++) { make_batch(&baseline,1,i,false); CHECK(!call(s,&baseline.request,OWNER_RESERVE,reply)); CHECK(!call(s,&baseline.request,OWNER_ABORT,reply)); }
    size_t cap=owner_store_bound(),n=0; uint8_t *image=malloc(cap); CHECK(image); uint64_t floor;
    CHECK(!owner_checkpoint_image(s,0,baseline.account_digest,s->transaction_high,s->uid_high,image,cap,&n,&floor));
    CHECK(fs_rd32(image+8)==2 && fs_rd32(image+140)==1); blob_put(&baseline.checkpoint,image,n); free(image); owner_close(s);
    struct owner_request q={.volume=31,.transaction=floor}; memcpy(q.sid,owner_sid,12); memcpy(q.accounts_digest,baseline.account_digest,32); sha256(baseline.checkpoint.data,baseline.checkpoint.bytes,q.manifest_digest);
    for(unsigned after=0;after<2;after++) for(unsigned cut=1;cut<=15;cut++) {
        struct owner_fake f; fake_clone(&f,&baseline); struct owner_backend io=fake_backend(&f); CHECK(!owner_open(s,&io,owner_sid,31,false));
        f.events=0; f.fail=cut; f.after=after;
        int e=call(s,&q,OWNER_CHECKPOINT,reply); if(!e) { f.marker=1; e=call(s,&q,OWNER_COMPACT,reply); }
        bool acknowledged=!e; owner_close(s); fake_cut(&f); io=fake_backend(&f); int reopened=owner_open(s,&io,owner_sid,31,false);
        if(!reopened) {
            CHECK(s->root_count==1 && s->roots[0].binding.key==root.binding.key && s->key_high==258);
            CHECK(s->floor==0 || s->floor==floor);
            CHECK(!call(s,&q,OWNER_CHECKPOINT,reply)); f.marker=1; CHECK(!call(s,&q,OWNER_COMPACT,reply)); CHECK(s->floor==floor && s->receipt_count==256);
        } else CHECK(!acknowledged && reopened==-EIO);
        owner_close(s); fake_destroy(&f);
    }
    fake_destroy(&baseline); free(s); puts("H02 checkpoint before/after every snapshot/marker/floor write+flush: retained creator map, recovery and trim PASS");
}
static void test_root_cuts(void)
{
    struct owner_fake baseline; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); fresh_store(&baseline,s);
    uint8_t proof[32]; memset(proof,81,32); struct owner_record record;
    for(unsigned i=0;i<255;i++) { char path[64]; ksnprintf(path,64,"/root%u",i); CHECK(!owner_root_reserve(s,&alice,path,proof,&record)); CHECK(!owner_root_finish(s,record.binding.key,proof,false)); }
    owner_close(s);
    for(unsigned after=0;after<2;after++) for(unsigned cut=1;cut<=12;cut++) {
        struct owner_fake f; fake_clone(&f,&baseline); struct owner_backend io=fake_backend(&f); CHECK(!owner_open(s,&io,owner_sid,31,false));
        f.fail=cut; f.after=after; f.events=0;
        int e=owner_root_reserve(s,&alice,"/last",proof,&record);
        if(!e) { f.published=1; e=owner_root_finish(s,record.binding.key,proof,false); }
        bool acknowledged=!e; owner_close(s); fake_cut(&f); io=fake_backend(&f); int reopened=owner_open(s,&io,owner_sid,31,false);
        if(!reopened) {
            CHECK(s->root_count==255 || s->root_count==256); CHECK(s->key_high==s->root_count && s->locator_high==s->root_count);
            if(s->root_key) {
                struct owner_record r; CHECK(owner_label(s,s->roots[255].binding.locator,&r)==-EACCES);
                CHECK(!owner_root_finish(s,s->root_key,proof,!f.published));
            }
            if(acknowledged) CHECK(s->root_count==256 && !s->roots[255].retired && !s->roots[255].pending);
        } else CHECK(!acknowledged && reopened==-EIO);
        owner_close(s); fake_destroy(&f);
    }
    fake_destroy(&baseline); free(s); puts("H02 root 256: every side-record write/flush cut, pending-object denial, durable recovery and nonreuse PASS");
}
static void test_profile_limit_and_retirement(void)
{
    struct owner_fake f; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); fresh_store(&f,s); make_batch(&f,32,1,false);
    uint8_t reply[OWNER_REPLY_MAX]; CHECK(!call(s,&f.request,OWNER_RESERVE,reply)); CHECK(!call(s,&f.request,OWNER_PREPARE,reply)); f.intent=1; CHECK(!call(s,&f.request,OWNER_COMMIT,reply));
    CHECK(s->profile_count==32); memcpy(f.account_digest,f.request.accounts_digest,32); f.accounts=1;
    make_batch(&f,3,2,false); struct owner_binding new_binding=f.request.bindings[2]; new_binding.uid=1030;
    ksnprintf(new_binding.path,64,"/Windows/Profiles/u%08x",new_binding.uid); f.request.count=1; f.request.bindings[0]=new_binding; f.request.owner_generation=s->generation;
    unsigned events=f.events; CHECK(call(s,&f.request,OWNER_RESERVE,reply)==-ENOSPC && f.events==events);
    make_batch(&f,32,2,false); f.request.owner_generation=s->generation;
    for(unsigned i=0;i<32;i++) {
        struct owner_binding *b=&f.request.bindings[i]; uint8_t *m=f.manifest.data+64+256*i;
        b->action=i==2?OWNER_RETIRE:OWNER_KEEP; b->key=s->profiles[i].binding.key; b->generation=s->profiles[i].binding.generation;
        fs_wr32(m+8,b->action); op64(m+240,b->key);
    }
    sha256(f.manifest.data,f.manifest.bytes,f.request.manifest_digest); f.intent=0;
    CHECK(!call(s,&f.request,OWNER_RESERVE,reply)); CHECK(!call(s,&f.request,OWNER_PREPARE,reply)); f.intent=1; CHECK(!call(s,&f.request,OWNER_COMMIT,reply));
    struct owner_record r; CHECK(owner_profile(s,1000,&r)==-EACCES && owner_root_occupied(s,31,"/Windows/Profiles/u000003e8")==1);
    CHECK(s->profile_count==32 && s->profiles[2].retired && s->profiles[2].binding.generation==2 && s->profiles[3].binding.generation==1);
    owner_close(s); fake_cut(&f); struct owner_backend io=fake_backend(&f); CHECK(!owner_open(s,&io,owner_sid,31,false)); CHECK(s->profiles[2].retired);
    owner_close(s); fake_destroy(&f); free(s); puts("H02 32-profile bound, full-vector KEEP/RETIRE, retained uid/root capacity and binding generations PASS");
}
static void test_allocation_failures(void)
{
    unsigned failures=0;
    for(unsigned op=OWNER_RESERVE;op>=OWNER_PREPARE;op=op==OWNER_RESERVE?OWNER_PREPARE:op==OWNER_PREPARE?OWNER_COMMIT:0) {
        for(unsigned fail=1;fail<16;fail++) {
            struct owner_fake f; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); fresh_store(&f,s); make_batch(&f,3,1,false);
            uint8_t reply[OWNER_REPLY_MAX];
            if(op!=OWNER_RESERVE) CHECK(!call(s,&f.request,OWNER_RESERVE,reply));
            if(op==OWNER_COMMIT) { CHECK(!call(s,&f.request,OWNER_PREPARE,reply)); f.intent=1; }
            owner_host_alloc_fail=fail; int e=call(s,&f.request,op,reply); owner_host_alloc_fail=0;
            if(e) { CHECK(e==-ENOMEM); failures++; if(!s->ready) CHECK(call(s,&f.request,OWNER_STATUS,reply)==-EIO); }
            owner_close(s); fake_cut(&f); struct owner_backend io=fake_backend(&f); CHECK(!owner_open(s,&io,owner_sid,31,false));
            CHECK(s->profile_count==0 || s->profile_count==3); if(f.intent) CHECK(s->profile_count==3);
            owner_close(s); fake_destroy(&f); free(s); if(!e) break;
        }
        if(op==OWNER_COMMIT) break;
    }
    CHECK(failures>=10); printf("H02 %u allocation failures across RESERVE/PREPARE/COMMIT: no volatile success, recover whole batch PASS\n",failures);
}
static void test_transport(void)
{
    struct owner_fake f; struct owner_store *s=calloc(1,sizeof(*s)); CHECK(s); fresh_store(&f,s); make_batch(&f,3,1,false);
    struct process *account=proc_find(security_subject_pid(account_peer)); CHECK(account); struct proc_thread *thread=0;
    for(unsigned i=0;i<CIUKI_THREAD_MAX;i++) if(proc_thread_slot(i) && proc_thread_slot(i)->process==account) thread=proc_thread_slot(i);
    CHECK(thread); desktop_select(thread);
    struct desktop_description *server=(struct desktop_description *)account_peer->peer.endpoint;
    struct channel *channel=server->u.endpoint.channel; int fd=-1;
    for(unsigned i=0;i<CIUKI_OPEN_MAX;i++) { struct desktop_description *d=desktop_fd(account,(int)i); if(d && d->kind==DESKTOP_CHANNEL && d->u.endpoint.channel==channel && !d->u.endpoint.side) fd=(int)i; }
    CHECK(fd>=0 && !channel_owner_pending()); CHECK(!owner_service_bind(s));
    uint8_t request[OWNER_REQUEST_MAX]; size_t bytes; CHECK(!owner_request_encode(&f.request,7,request,sizeof(request),&bytes));
    for(size_t offset=0;offset<bytes;) {
        unsigned chunk=bytes-offset>192?192:(unsigned)(bytes-offset); struct ciuki_message message={.length=64+chunk};
        uint32_t h[16]={0x31435343,1,64,3,7,1,1,0,channel->generation[0],chunk,0,0,0,(uint32_t)bytes,(uint32_t)offset,0};
        memcpy(message.data,h,64); memcpy(message.data+64,request+offset,chunk);
        CHECK(!ua_write(account->memory,CIUKI_IMAGE_BASE+PAGE_SIZE+0x100,&message,sizeof(message)));
        CHECK(channel_send(account,fd,CIUKI_IMAGE_BASE+PAGE_SIZE+0x100,DONTWAIT)==0); offset+=chunk;
    }
    CHECK(channel_owner_pending()); owner_service_poll(); CHECK(!channel_owner_pending() && s->active);
    uint8_t reply[OWNER_REPLY_MAX]; size_t got=0;
    while(channel->exchange) {
        CHECK(channel_recv(account,fd,CIUKI_IMAGE_BASE+PAGE_SIZE+0x100,DONTWAIT)==1);
        struct ciuki_message message; CHECK(!ua_read(account->memory,&message,CIUKI_IMAGE_BASE+PAGE_SIZE+0x100,sizeof(message)));
        uint32_t h[16]; memcpy(h,message.data,64); CHECK(h[3]==3 && h[4]==7 && h[5]==2 && !h[12] && h[14]==got);
        memcpy(reply+got,message.data+64,h[9]); got+=h[9];
    }
    CHECK(got==192 && fs_rd32(reply+8)==OWNER_RESERVED && fs_rd32(reply+12)==3);
    CHECK(!call(s,&f.request,OWNER_ABORT,reply));
    int server_fd=-1; for(unsigned i=0;i<CIUKI_OPEN_MAX;i++) if(proc_supervisor()->fds[i].object==account_peer->peer.endpoint) server_fd=(int)i;
    CHECK(server_fd>=0 && !desktop_close(account,fd)); channel_owner_reap(); CHECK(!proc_supervisor()->fds[server_fd].object);
    owner_close(s); fake_destroy(&f); free(s); g_current=&controller;
    puts("H02 OWNER production channel: fragmented request, retained admission, dispatch, framed correlated reply PASS");
}
static void subject_fixture(void)
{
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram); controller.state=T_RUNNING; g_current=&controller; proc_init();
    struct process *account; CHECK(!proc_prepare(proc_supervisor(),&account));
    CHECK(!security_role_prepare(proc_supervisor(),account,3,security_role_image(3)));
    struct ciuki_file image=fixture_file(); struct proc_strings *args=proc_strings_new(); CHECK(args);
    CHECK(!proc_strings_add(args,"accounts",9,false)); CHECK(!prepare_image(account,&image,args,0)); proc_strings_free(args); proc_publish(account,0);
    struct ciuki_security_link in={.version=1,.size=64,.service=3}, client_link, server_link; uint32_t result;
    CHECK(!channel_security_link(account,12,&in,&client_link,&result));
    CHECK(!channel_security_link(proc_supervisor(),13,&in,&server_link,&result));
    CHECK(!security_subject_acquire_endpoint(proc_supervisor(),server_link.endpoint_fd,server_link.endpoint_generation,&account_peer));
    CHECK(!security_subject_check(account_peer,(3u<<16)|7,0));
}
#ifdef OWNER_REAL_VFS_TEST
#include "owner_vfs_fixture.inc"
#endif
int main(int argc,char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout,0,_IONBF,0); subject_fixture();
#ifdef OWNER_REAL_VFS_TEST
    CHECK(argc==2); test_owner_real_vfs(argv[1]);
#else
    test_batches_and_cuts(3,false); test_batches_and_cuts(18,true); test_invalid_and_receipts(); test_compaction(); test_creator_roots(); test_matrix(); test_checkpoint_cuts(); test_root_cuts(); test_profile_limit_and_retirement(); test_allocation_failures(); test_transport();
#endif
    struct process *account=proc_find(security_subject_pid(account_peer)); CHECK(account);
    security_subject_put(account_peer); account_peer=0; stop_collect(account);
    for(unsigned i=0;i<CIUKI_OPEN_MAX;i++) if(proc_supervisor()->fds[i].object) CHECK(!desktop_close(proc_supervisor(),(int32_t)i));
    kfree(supervisor_task); supervisor_task=0; CHECK(!pages_used && !heap_blocks && !fs_host_contig_live); free(ram); ram=0;
    printf("H02 production OWNER: PASS checks=%u store=%zu label=%zu; zero live allocations\n",checks,sizeof(struct owner_store),sizeof(struct policy_label)); return 0;
}
