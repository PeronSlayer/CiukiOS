/* Host transaction qualification over the production FAT/VFS/lease and real
 * libdisk, with a synthetic CD source of >200 files. No QEMU or image build.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../sdk/libsetup/internal.h"
#include "libsetup_model.inc"
#include <strings.h>
#include <errno.h>
#include <ciuki/devices.h>
#include "../../sdk/libreg/internal.h"
#include "../../apps/desktop/settings.h"
#include <ciuki/net_config.h>
#include <ciuki/tzrule.h>
#include "../../sdk/libsetup/internal.h"
/* sec-user-02: the Setup seed recipe and the account service's own codecs. */
#include "../../apps/accounts/accounts.h"
#include "../../sdk/private/provision.h"
#include <ciuki/sha256.h>
int st_seed_build(uint8_t out[CIUKI_PROVISION_SEED_BYTES], const char *name, const char *user_password,
                  const char *recovery_password, int (*entropy)(void *, void *, size_t), void *ctx);
int st_hopen(const char *);
int st_hread(int,void *,unsigned);
int st_hseek(int,uint64_t);
int st_hclose(int);
int st_hinfo(const char *,uint64_t *,int *);
static size_t heap_live,heap_peak;
void *st_test_malloc(size_t n) {
    size_t *p=malloc(n+sizeof(max_align_t)); if(!p)return NULL;
    *p=n; heap_live+=n; if(heap_live>heap_peak)heap_peak=heap_live;
    return (char *)p+sizeof(max_align_t);
}
void st_test_free(void *v) {
    if(!v)return;
    size_t *p=(size_t *)((char *)v-sizeof(max_align_t)); heap_live-=*p; free(p);
}
static struct {
    struct model m;
    char paths[128][260]; uint32_t path_generation[128]; unsigned opens;
    enum setup_phase phase,cancel_phase,cut_phase;
    unsigned errors,retries,fail_reads,change_generation,fail_manifest_reads,fail_identity_reads,fail_info_reads;
    int write_error,sync_error,preflight_error;
    bool corrupt,corrupted,missing,different_disc,hash_mismatch,nics,bad_template,alias_mismatch;
    uint32_t generation;
    uint64_t time_ms,removed_at;
    unsigned tray_opens,restarts,configure_calls;
    bool removed,remove_in_copy,cancel_error;
    unsigned publication_cut; bool publishing_mbr;
    enum setup_phase fail_flush_phase;
    struct fs_ro_volume reserved;
} S;
static void source_path(char out[1024],char d,const char *p) {
    if(d=='C' && !strncasecmp(p,"system",6) && (!p[6] || p[6]=='/')) snprintf(out,1024,"%s/C/system%s",work,p+6);
    else snprintf(out,1024,"%s/%c/%s",work,d,p);
}
static int s_preflight(void *ctx,const struct setup_answers *a) { (void)ctx; (void)a; return S.preflight_error; }
static int s_open(void *ctx,char d,const char *p,bool writing)
{
    (void)ctx;
    if(d=='C' || d=='D') {
        if(writing)return -EROFS;
        char full[1024]; source_path(full,d,p);
        int h=st_hopen(full); if(h<0)return h;
        CHECK(h<128); snprintf(S.paths[h],260,"%s",p); S.path_generation[h]=S.generation; S.opens++;
        return h+1000;
    }
    char path[272]; path_of(path,sizeof(path),d,p);
    int h=vfs_open(&H.table,path,writing ? VFS_READ|VFS_WRITE|VFS_CREATE|VFS_TRUNCATE : VFS_READ,VFS_DENY_NONE,FAT_ATTR_ARCHIVE);
    if(h>=0)S.opens++;
    if(h>=0)snprintf(S.paths[h],260,"%s",p);
    return h;
}
static int s_read(void *ctx,int h,void *buf,unsigned n)
{
    (void)ctx;
    if(h>=1000) {
        const char *p=S.paths[h-1000];
        if(S.path_generation[h-1000]!=S.generation)return -EIO;
        if(!strcasecmp(p,ST_RELEASE) && S.fail_identity_reads) { S.fail_identity_reads--; return -EIO; }
        if(S.phase==SETUP_PHASE_COPY && !strcmp(p,ST_MANIFEST) && S.fail_manifest_reads) { S.fail_manifest_reads--; return -EIO; }
        if(strstr(p,"Long filename number 003") && S.fail_reads) { S.fail_reads--; return -EIO; }
        if(strstr(p,"Long filename number 003") && S.change_generation) { S.change_generation--; S.generation++; return -EAGAIN; }
        int r=st_hread(h-1000,buf,n);
        if(r>0 && S.bad_template && !strcmp(p,ST_MANIFEST)) {
            const char key[]="boot_cfg_sha256\": \"";
            for(unsigned i=0;i+sizeof(key)<=(unsigned)r;i++)if(!memcmp((char *)buf+i,key,sizeof(key)-1)) {
                char *hex=(char *)buf+i+sizeof(key)-1;*hex=*hex=='a' ? 'b' : 'a';break;
            }
        }
        if(r>0 && ((S.different_disc && S.generation>1 && !strcasecmp(p,ST_RELEASE)) || (S.hash_mismatch && strstr(p,"Long filename number 003")))) ((uint8_t *)buf)[0]^=1;
        return r;
    }
    size_t done=0; int e=vfs_read(&H.table,h,buf,n,&done); return e ? e : (int)done;
}
static int s_write(void *ctx,int h,const void *buf,unsigned n)
{
    (void)ctx;
    if(S.phase==SETUP_PHASE_COPY && S.write_error)return S.write_error;
    size_t done=0; int e=vfs_write(&H.table,h,buf,n,&done); return e ? e : (int)done;
}
static int s_seek(void *ctx,int h,uint64_t offset)
{
    (void)ctx; if(h>=1000)return st_hseek(h-1000,offset);
    return vfs_seek(&H.table,h,(int64_t)offset,VFS_SEEK_SET,NULL);
}
static int s_sync(void *ctx,int h) { (void)ctx; return S.phase==SETUP_PHASE_COPY && S.sync_error ? S.sync_error : vfs_commit(&H.table,h); }
static int s_close(void *ctx,int h) { (void)ctx; S.opens--; return h>=1000 ? st_hclose(h-1000) : vfs_close(&H.table,h); }
static int s_info(void *ctx,char d,const char *p,struct st_info *out)
{
    (void)ctx; memset(out,0,sizeof(*out));
    const char *base=strrchr(p,'/'); base=base ? base+1 : p;
    snprintf(out->name,sizeof(out->name),"%s",base);
    if(d=='C' || d=='D') {
        if(S.phase==SETUP_PHASE_COPY && strstr(p,"Long filename number 003") && S.fail_info_reads) { S.fail_info_reads--; return -EIO; }
        if(S.missing && strstr(p,"Long filename number 003"))return -ENOENT;
        char full[1024]; source_path(full,d,p); int dir=0;
        int e=st_hinfo(full,&out->size,&dir); if(e)return e;
        out->attr=dir ? 16 : 32;
        unsigned i;
        if(sscanf(base,"Long filename number %u.bin",&i)==1)out->attr=i%2 ? 33 : 38;
        return 0;
    }
    char path[272]; path_of(path,sizeof(path),d,p); struct fat_entry f;
    int e=vfs_stat(&H.table,path,&f); if(e)return e;
    out->size=f.size; out->attr=f.attr; snprintf(out->name,sizeof(out->name),"%s",f.name); return 0;
}
static int s_mkdir(void *ctx,char d,const char *p) { return b_mkdir(ctx,d,p); }
static int s_attrib(void *ctx,char d,const char *p,uint8_t attr)
{ (void)ctx; char path[272]; path_of(path,sizeof(path),d,p); return vfs_attrib(&H.table,path,attr); }
static int s_cluster(void *ctx,char d,const char *p,uint32_t index,uint64_t *lba)
{
    (void)ctx; char path[272]; path_of(path,sizeof(path),d,p); struct fat_entry f;
    int e=vfs_stat(&H.table,path,&f); if(e)return e;
    struct fat_volume *vol=&H.s->volumes[d-'A'].fat;
    uint32_t cluster=f.first;
    for(uint32_t i=0;i<index && cluster>=2 && cluster<0x0ffffff8;i++) { e=fat_get_cluster(vol,cluster,&cluster); if(e)return e; }
    if(!cluster || cluster>=0x0ffffff8)return -ENOENT;
    if(cluster<2 || cluster>=vol->clusters+2)return -EIO;
    *lba=DISK_PART_LBA+vol->start+vol->data_sector+(uint64_t)(cluster-2)*vol->spc;
    if(S.corrupt && !S.corrupted) {
        uint8_t sector[512]; OK(fake_raw_read(&S.m.vol,*lba-DISK_PART_LBA,sector)); sector[0]^=1;
        OK(fake_raw_write(&S.m.vol,*lba-DISK_PART_LBA,sector)); S.corrupted=true;
    }
    return 0;
}
static int s_configure(void *ctx,char d,const struct setup_answers *a)
{
    (void)ctx; S.configure_calls++;
    return st_native_platform()->configure(NULL,d,a);
}
static int s_media(void *ctx,bool tray,bool *removed,uint32_t *generation)
{
    (void)ctx; if(tray)S.tray_opens++;
    if(S.removed_at && S.time_ms>=S.removed_at)S.removed=true;
    *removed=S.removed; *generation=S.generation; return 0;
}
static uint64_t s_ms(void *ctx) { (void)ctx; return S.time_ms; }
static void s_sleep(void *ctx,unsigned ms) { (void)ctx; S.time_ms+=ms; }
static int s_restart(void *ctx) { (void)ctx; S.restarts++; return 0; }
static const struct st_platform platform={NULL,s_preflight,s_open,s_read,s_write,s_seek,s_sync,s_close,s_info,s_mkdir,s_attrib,s_cluster,s_configure,s_media,s_ms,s_sleep,s_restart};
/* Production native.c is compiled with only its POSIX entry points renamed.
 * These translate native names/descriptors to the production file-backed VFS
 * fixture. The transaction uses st_native_platform unchanged. */
static unsigned rtc_writes;
static char native_drive[128];
static struct { char drive,path[260]; bool live; } native_dirs[8];
static struct ciuki_setup_clock last_clock;
static int native_path(const char *full,char *drive,const char **relative)
{
    if (!strncmp(full,"/mnt/",5) && full[5]>='a' && full[5]<='z' && full[6]=='/') {
        *drive=(char)(full[5]-'a'+'A'); *relative=full+7;
    } else if (*full=='/') { *drive='C'; *relative=full+1; }
    else return -EINVAL;
    return 0;
}
int st_native_open(const char *full,int flags,...)
{
    if (!strcmp(full,"/dev/net")) return 2000;
    if (!strcmp(full,"/dev/media")) return 2001;
    char drive; const char *rel; int e=native_path(full,&drive,&rel);
    int h=e ? e : s_open(NULL,drive,rel,(flags&O_ACCMODE)!=O_RDONLY);
    if(h==-EISDIR && drive>='E' && (flags&O_ACCMODE)==O_RDONLY) {
        for(unsigned i=0;i<8;i++)if(!native_dirs[i].live) {
            native_dirs[i].live=true; native_dirs[i].drive=drive; snprintf(native_dirs[i].path,260,"%s",rel); S.opens++; return 3000+(int)i;
        }
        h=-EMFILE;
    }
    if(h<0){errno=-h;return -1;}
    if(h<128)native_drive[h]=drive;
    return h;
}
long st_native_read(int h,void *p,unsigned long n) { int e=s_read(NULL,h,p,(unsigned)n); if(e<0){errno=-e;return -1;}return e; }
long st_native_write(int h,const void *p,unsigned long n) { int e=s_write(NULL,h,p,(unsigned)n); if(e<0){errno=-e;return -1;}return e; }
long st_native_seek(int h,long off,int whence) { CHECK(whence==0); int e=s_seek(NULL,h,(uint64_t)off); if(e){errno=-e;return -1;}return off; }
int st_native_sync(int h) { int e=s_sync(NULL,h);if(e){errno=-e;return -1;}return 0; }
int st_native_close(int h) { if(h>=3000 && h<3008){CHECK(native_dirs[h-3000].live);native_dirs[h-3000].live=false;S.opens--;return 0;} if(h==2000 || h==2001)return 0; int e=s_close(NULL,h);if(e){errno=-e;return -1;}return 0; }
int st_native_mkdir(const char *full,unsigned mode) {
    (void)mode; char drive;const char *rel;int e=native_path(full,&drive,&rel);if(!e)e=s_mkdir(NULL,drive,rel);
    if(e){errno=-e;return -1;}return 0;
}
int st_native_rename(const char *from,const char *to) {
    char a,b;const char *first,*second;int e=native_path(from,&a,&first);if(!e)e=native_path(to,&b,&second);
    char source[272],dest[272];if(!e)path_of(source,sizeof(source),a,first);if(!e)path_of(dest,sizeof(dest),b,second);
    if(!e)e=vfs_rename(&H.table,source,dest);if(e){errno=-e;return -1;}return 0;
}
int st_native_unlink(const char *full) {
    char d;const char *rel;int e=native_path(full,&d,&rel);char path[272];if(!e)path_of(path,sizeof(path),d,rel);
    if(!e)e=vfs_delete(&H.table,path);if(e){errno=-e;return -1;}return 0;
}
long st_native_time(long *out) { if(out)*out=1792700000;return 1792700000; }
int ciuki_devices(struct ciuki_devices_info *d) { d->count=0;return 0; }
int st_test_devctl(int h,uint32_t op,void *data,uint32_t bytes)
{
    if(h==2001 && op==MEDIA_CD_STATE) {
        struct ciuki_media_cd_state *m=data; m->present=m->mounted=m->slot==1; m->drive=3;
        m->generation=S.generation; m->state=S.removed ? CIUKI_CD_NO_MEDIUM : CIUKI_CD_READY; return 0;
    }
    if(h==2000 && op==CIUKI_SETUP_NET_QUERY) {
        struct ciuki_setup_net *q=data;q->count=S.nics ? 2 : 0;
        for(unsigned i=0;i<q->count;i++) { q->identity[i].vendor=0x8086;q->identity[i].device=0x1229;q->identity[i].mac[0]=2;q->identity[i].mac[5]=(uint8_t)(i+1); }
        return 0;
    }
    if(h==2000 && op==NET_GET_STATE) { CHECK(bytes==sizeof(struct ciuki_net_state));struct ciuki_net_state *n=data;n->present=0;return 0; }
    if(op==CIUKI_SETUP_CLOCK_SET) { CHECK(bytes==sizeof(last_clock));last_clock=*(struct ciuki_setup_clock *)data;rtc_writes++;return 0; }
    if(op!=CIUKI_SETUP_FAT_QUERY && op!=CIUKI_SETUP_FAT_ATTRIB)return -ENOTSUP;
    CHECK(bytes==sizeof(struct ciuki_setup_fat));struct ciuki_setup_fat *q=data;
    char drive;const char *rel; char path[272];
    if(h>=3000) { CHECK(h<3008 && native_dirs[h-3000].live);drive=native_dirs[h-3000].drive;rel=native_dirs[h-3000].path; }
    else if(h>=1000) { drive='C';rel=S.paths[h-1000]; if(!strcmp(rel,ST_MANIFEST) || !strncmp(rel,"SOURCES/",8))drive='D'; }
    else { CHECK(h>=0 && h<128);drive=native_drive[h];rel=S.paths[h]; }
    if(op==CIUKI_SETUP_FAT_ATTRIB)return s_attrib(NULL,drive,rel,(uint8_t)q->attributes);
    struct st_info info; int e=s_info(NULL,drive,rel,&info);if(e)return e;
    q->attributes=info.attr;q->file_size=info.size;memcpy(q->name,info.name,sizeof(q->name));
    if(q->cluster_index!=CIUKI_SETUP_INDEX_NONE) { if(q->mount_generation!=1 || q->chain_generation!=1)return -EAGAIN;
        e=s_cluster(NULL,drive,rel,q->cluster_index,&q->lba);if(e)return e; }
    q->mount_generation=1;q->chain_generation=1;q->cluster_sectors=8;snprintf(q->alias,sizeof(q->alias),S.alias_mismatch && drive=='E' && strstr(rel,"number 003") ? "ALIAS~2.BIN" : drive=='C' ? "alias~1.bin" : "ALIAS~1.BIN");
    (void)path;return 0;
}
static int setup_pwrite(void *ctx,int h,const void *buf,uint32_t n,uint64_t offset)
{
    bool final=offset==0 && n==512 && !memcmp(buf,disk_canonical_mbr_code,440);
    if(final && S.publication_cut==1)return -EIO;
    int r=b_pwrite(ctx,h,buf,n,offset);
    if(final && r==(int)n) {
        S.publishing_mbr=true;
        if(S.publication_cut==2) { H.fail_op=DISK_FLUSH; H.fail_after=1; H.fail_result=-EIO; H.op_calls=0; }
    }
    return r;
}
static int setup_pread(void *ctx,int h,void *buf,uint32_t n,uint64_t offset)
{
    if(S.publishing_mbr && S.publication_cut==3 && offset==0)return -EIO;
    return b_pread(ctx,h,buf,n,offset);
}
static struct disk_backend diskio;
static void on_progress(void *ctx,const struct setup_progress *p)
{
    (void)ctx; S.phase=p->phase;
    if(S.remove_in_copy && p->phase==SETUP_PHASE_COPY)S.removed=true;
    if(S.fail_flush_phase && p->phase==S.fail_flush_phase) {
        H.fail_op=DISK_FLUSH; H.fail_after=1; H.fail_result=-EIO; H.op_calls=0;
        S.fail_flush_phase=0;
    }
    if(S.cut_phase && p->phase==S.cut_phase)S.m.head.cut=S.m.vol.cut=true;
}
static bool on_cancel(void *ctx) { (void)ctx; return S.cancel_phase && S.phase==S.cancel_phase; }
static enum setup_decision on_error(void *ctx,const struct setup_error *e)
{
    (void)ctx; S.errors++; CHECK(e->retryable && e->attempt<=SETUP_RETRY_MAX);
    if(S.cancel_error)return SETUP_DECISION_CANCEL;
    return S.retries++<SETUP_RETRY_MAX ? SETUP_DECISION_RETRY : SETUP_DECISION_CANCEL;
}
static const struct setup_callbacks callbacks={on_progress,on_error,on_cancel,NULL};
static void begin(uint64_t capacity)
{
    memset(&S,0,sizeof(S)); S.generation=1;
    model_open(&S.m,"setup",capacity,NULL); start(&S.m,0);
    /* D: is the CD; publish the destination as E:, as in Setup mode. */
    S.reserved.mounted=true; H.s->vfs.ro_volumes[3]=&S.reserved;
    diskio=backend; diskio.pwrite=setup_pwrite; diskio.pread=setup_pread;
    disk_set_backend(&diskio); st_set_platform(&platform);
}
static void end(void)
{
    CHECK(S.opens==0 && heap_live==0);
    H.s->vfs.ro_volumes[3]=NULL; stop(); model_close(&S.m);
}
/* sec-user-02: deterministic test entropy; production uses getentropy. */
static int test_entropy(void *ctx,void *out,size_t n)
{ unsigned *next=ctx; for(size_t i=0;i<n;i++)((uint8_t *)out)[i]=(uint8_t)(++*next*37u); return 0; }
static uint8_t test_seed[CIUKI_PROVISION_SEED_BYTES];
static struct setup_answers answers(void)
{
    struct setup_answers a; setup_answers_defaults(&a); a.accept_licences=1; a.erase_disk=1; a.reinstall=1;
    a.components=SETUP_COMP_SYSTEM|SETUP_COMP_SOURCES; a.lobby=0;
    snprintf(a.user_name,sizeof(a.user_name),"Tester"); snprintf(a.computer_name,sizeof(a.computer_name),"CIUKI-PC");
    snprintf(a.keyboard,sizeof(a.keyboard),"it");
    a.hardware_report="Generic PC hardware fixture\n"; a.hardware_report_length=strlen(a.hardware_report);
    static unsigned counter; if(!test_seed[0]) OK(st_seed_build(test_seed,"Tester","tester password","recovery password",test_entropy,&counter));
    a.account_seed=test_seed; a.account_seed_length=sizeof(test_seed);
    return a;
}
/* sec-user-02: the destination carries exactly the Setup seed and its
 * UNPROVISIONED state, decoded by the account service's codecs, bound to the
 * formatted volume (partition LBA and BPB serial); no accounts.db. */
static void check_provisioning(const struct setup_answers *a)
{
    static uint8_t seed[CIUKI_PROVISION_SEED_BYTES+1],state[CIUKI_PROVISION_STATE_BYTES+1];
    CHECK(b_read_file(NULL,'E',"System/Security/provision.seed",seed,sizeof(seed))==(int)CIUKI_PROVISION_SEED_BYTES);
    CHECK(!memcmp(seed,a->account_seed,CIUKI_PROVISION_SEED_BYTES));
    struct acct_db *db=calloc(1,sizeof(*db)); CHECK(db);
    OK(acct_decode(db,1,seed,CIUKI_PROVISION_SEED_BYTES));
    CHECK(db->origin==2 && db->count==3 && !db->auto_logon_uid && db->records[2].uid==1000 && !strcmp(db->records[2].name,"Tester"));
    CHECK(db->records[0].flags==6 && db->records[1].flags==2 && db->records[2].flags==5);
    uint8_t derived[32];
    OK(acct_derive("tester password",db->records[2].salt,ACCT_ITERATIONS,derived)); CHECK(!memcmp(derived,db->records[2].verifier,32));
    OK(acct_derive("recovery password",db->records[0].salt,ACCT_ITERATIONS,derived)); CHECK(!memcmp(derived,db->records[0].verifier,32));
    CHECK(memcmp(db->records[0].salt,db->records[2].salt,16));
    for(unsigned i=0;i<db->count;i++) CHECK(strcmp(db->records[i].name,"CiukiUser"));
    free(db);
    CHECK(b_read_file(NULL,'E',"System/Security/bootstrap.state",state,sizeof(state))==(int)CIUKI_PROVISION_STATE_BYTES);
    struct acct_bootstrap b; OK(acct_boot_decode(&b,state,CIUKI_PROVISION_STATE_BYTES));
    uint8_t boot[512],digest[32]; OK(fake_raw_read(&S.m.vol,0,boot)); sha256(seed,CIUKI_PROVISION_SEED_BYTES,digest);
    CHECK(b.state==1 && b.origin==2 && b.next_uid==1001 && !b.flags && b.generation==1 && !b.transaction);
    CHECK(b.volume==ciuki_provision_volume_id(2048,disk_rd32(boot+67)) && b.volume>>32==2048 && !memcmp(b.seed_digest,digest,32));
    char probe[8]; CHECK(b_read_file(NULL,'E',"System/Security/accounts.db",probe,sizeof(probe))<0);
    struct fat_entry f; OK(vfs_stat(&H.table,"E:/Windows/Profiles",&f)); CHECK(f.attr&16);
    CHECK(vfs_stat(&H.table,"E:/Windows/Profiles/u000003e8",&f)==-ENOENT);
}
static void expect_error(int result,enum setup_error_class cls,enum setup_phase phase)
{
    if(result!=-(int)cls) {
        struct setup_error e; setup_txn_last_error(&e);
        fprintf(stderr,"expected %d got %d phase=%d err=%d path=%s\n",-(int)cls,result,e.phase,e.err,e.path);
    }
    CHECK(result==-(int)cls);
    struct setup_error e; OK(setup_txn_last_error(&e)); CHECK(e.cls==cls && e.phase==phase);
    enum setup_phase p; OK(setup_txn_state(&p)); CHECK(p==phase);
}
static void expect_stub(void)
{
    uint8_t mbr[512]; OK(fake_raw_read(&S.m.head,0,mbr)); CHECK(!memcmp(mbr,disk_stub_mbr_code,440));
}
static void test_prepare(void)
{
    struct setup_answers a=answers(); struct setup_plan p;
    for(unsigned size=1;size<=4;size*=4) {
        begin((uint64_t)size*2097152); OK(setup_txn_prepare(&a,&p));
        CHECK(S.m.head.writes==0 && S.m.vol.writes==0);
        uint64_t volume=((uint64_t)size*2097152-2048)*512;
        CHECK(p.reserve_bytes==32*1024*1024+volume/20);
        CHECK(p.partition_start==2048 && p.partition_count==volume/512);
        CHECK(p.required_bytes+p.free_after_bytes==volume);
        /* Hand-counted fixture: 222 immutable files, five generated budgets,
         * 295728 bytes, 281 clusters, 448 conservative directory entries.
         * mkfs.fat geometry: 261357 / 1046256 data clusters at 1 / 4 GiB. */
        CHECK(p.overhead_bytes==(size==1 ? 4934864ull : 11214032ull));
        CHECK(p.required_bytes==(size==1 ? 92419686ull : 259760128ull));
        CHECK(p.free_after_bytes==(size==1 ? 980273562ull : 4034158592ull));
        end();
    }
    /* sec-user-02: no seed, a seed for another name or a ready seed is refused before any write. */
    begin(2097152);
    { struct setup_answers b=answers(); b.account_seed=NULL; b.account_seed_length=0;
      expect_error(setup_txn_prepare(&b,&p),SETUP_ERR_INTERNAL,SETUP_PHASE_PLAN);
      b=answers(); snprintf(b.user_name,sizeof(b.user_name),"Other"); expect_error(setup_txn_prepare(&b,&p),SETUP_ERR_INTERNAL,SETUP_PHASE_PLAN);
      static uint8_t ready[CIUKI_PROVISION_SEED_BYTES]; memcpy(ready,test_seed,sizeof(ready)); ready[52]=1;
      b=answers(); b.account_seed=ready; expect_error(setup_txn_prepare(&b,&p),SETUP_ERR_INTERNAL,SETUP_PHASE_PLAN);
      b=answers(); b.account_seed_length--; expect_error(setup_txn_prepare(&b,&p),SETUP_ERR_INTERNAL,SETUP_PHASE_PLAN);
      CHECK(!S.m.head.writes && !S.m.vol.writes); }
    end();
    begin(2097152); S.missing=true;
    expect_error(setup_txn_prepare(&a,&p),SETUP_ERR_SOURCE_READ,SETUP_PHASE_PLAN);
    CHECK(!S.m.head.writes && !S.m.vol.writes); S.missing=false;
    a.components=SETUP_COMP_SYSTEM|SETUP_COMP_GAMES;
    expect_error(setup_txn_prepare(&a,&p),SETUP_ERR_INTERNAL,SETUP_PHASE_PLAN);
    a=answers(); S.preflight_error=-ENOTSUP;
    expect_error(setup_txn_prepare(&a,&p),SETUP_ERR_INTERNAL,SETUP_PHASE_PLAN);
    CHECK(!S.m.head.writes && !S.m.vol.writes);
    S.preflight_error=0; a.components|=SETUP_COMP_DEVTOOLS;
    expect_error(setup_txn_prepare(&a,&p),SETUP_ERR_DEST_FULL,SETUP_PHASE_PLAN);
    CHECK(p.free_after_bytes==0 && p.required_bytes>p.partition_count*512);
    CHECK(!S.m.head.writes && !S.m.vol.writes);
    end();
    begin(2097152); a=answers(); st_set_platform(NULL);
    OK(setup_txn_prepare(&a,&p));
    CHECK(!S.m.head.writes && !S.m.vol.writes && !rtc_writes);
    int native_result=setup_txn_execute(&p,&callbacks,NULL);
    if(native_result) { struct setup_error err;setup_txn_last_error(&err);fprintf(stderr,"native error cls=%u errno=%d phase=%u path=%s\n",err.cls,err.err,err.phase,err.path); }
    OK(native_result);
    CHECK(rtc_writes==1 && last_clock.offset_minutes==a.tz_minutes && last_clock.rtc_local==(a.clock==SETUP_CLOCK_LOCAL));
    end();
}
static void test_complete(void)
{
    begin(2097152); struct setup_answers a=answers(); struct setup_plan p;
    static struct setup_log ui_log; memset(&ui_log,0,sizeof(ui_log));
    static char long_ui_line[40001]; memset(long_ui_line,'x',40000); long_ui_line[40000]=0;
    setup_log_append(&ui_log,long_ui_line);
    setup_log_append(&ui_log,"[setup] Welcome fixture"); a.log=&ui_log;
    OK(setup_txn_prepare(&a,&p));
    int result=setup_txn_execute(&p,&callbacks,NULL);
    if(result) { struct setup_error error; setup_txn_last_error(&error); fprintf(stderr,"execute result=%d phase=%d err=%d path=%s lba=%llu\n",result,error.phase,error.err,error.path,(unsigned long long)error.lba); }
    OK(result);
    enum setup_phase phase; OK(setup_txn_state(&phase)); CHECK(phase==SETUP_PHASE_COMPLETE);
    CHECK(S.configure_calls==1 && heap_peak<=512*1024);
    char text[4096]={0}; int n=b_read_file(NULL,'E',"SYSTEM/BOOT.CFG",text,sizeof(text)-1); CHECK(n>0);
    CHECK(!strcmp(text,"version=1 serial=1\n") && !strstr(text,"setup=1"));
    memset(text,0,sizeof(text)); CHECK(b_read_file(NULL,'E',"SYSTEM/SETUP.STATE",text,sizeof(text)-1)>0);
    CHECK(strstr(text,"phase=installed\n") && strstr(text,"firstboot=pending\n"));
    static char log_text[SETUP_LOG_BYTES+1]; memset(log_text,0,sizeof(log_text));
    CHECK(b_read_file(NULL,'E',"SYSTEM/SETUP.LOG",log_text,SETUP_LOG_BYTES)>0);
    CHECK(strstr(log_text,"0 plan ui [setup] Welcome fixture\n"));
    for(char *line=log_text;*line;) {
        unsigned long long ms; char phase_word[32],event_word[64];
        CHECK(sscanf(line,"%llu %31s %63s",&ms,phase_word,event_word)==3);
        CHECK(!strcmp(phase_word,"plan") || !strcmp(phase_word,"partition") || !strcmp(phase_word,"format") ||
              !strcmp(phase_word,"copy") || !strcmp(phase_word,"configure") || !strcmp(phase_word,"publish"));
        char *nl=strchr(line,'\n'); CHECK(nl); line=nl+1;
    }
    CHECK(b_read_file(NULL,'E',"SYSTEM/HARDWARE.TXT",text,sizeof(text))==(int)a.hardware_report_length);
    check_provisioning(&a);
    CHECK(!strstr(log_text,"tester password") && !strstr(log_text,"recovery password") && strstr(log_text,"accounts staged origin=setup"));
    struct fat_entry f; CHECK(vfs_stat(&H.table,"E:/SYSTEM/PAGEFILE.SWP",&f)==-ENOENT);
    CHECK(vfs_stat(&H.table,"E:/Optional/DOS component.txt",&f)==-ENOENT);
    OK(vfs_stat(&H.table,"E:/Empty directory",&f)); CHECK(f.attr&16);
    for(unsigned i=0;i<220;i++) {
        char path[260]; snprintf(path,sizeof(path),"Tree/Directory %u/Long filename number %03u.bin",i%9,i);
        struct st_info src,dst; OK(s_info(NULL,'C',path,&src)); OK(s_info(NULL,'E',path,&dst));
        CHECK(src.size==dst.size && src.attr==dst.attr && !strcmp(src.name,dst.name));
        uint8_t data[5000],expected[5000];
        int h=s_open(NULL,'C',path,false); CHECK(h>=1000); int k=s_read(NULL,h,expected,sizeof(expected)); OK(s_close(NULL,h));
        n=b_read_file(NULL,'E',path,data,sizeof(data)); CHECK(n==k && !memcmp(data,expected,(size_t)n));
    }
    uint8_t mbr[512]; OK(fake_raw_read(&S.m.head,0,mbr)); CHECK(!memcmp(mbr,disk_canonical_mbr_code,440));
    CHECK(disk_rd32(mbr+454)==2048 && disk_rd32(mbr+458)==p.partition_count);
    char loader_path[1024]; snprintf(loader_path,sizeof(loader_path),"%s/loader.bin",work);
    long loader_n=hf_size(loader_path)*512; CHECK(loader_n>0 && loader_n<=32768);
    static uint8_t raw_loader[32768],expected_loader[32768];
    OK(hf_read(S.m.head_path,512,raw_loader,(size_t)loader_n));
    OK(hf_read(loader_path,0,expected_loader,(size_t)loader_n));
    CHECK(!memcmp(raw_loader,expected_loader,(size_t)loader_n));
    uint8_t fat[512]; OK(fake_raw_read(&S.m.vol,DISK_RESERVED,fat)); CHECK(disk_rd32(fat+4)&DISK_FAT32_CLEAN);
    char command[1500],out[8192]; snprintf(command,sizeof(command),"fsck.fat -n '%s'",S.m.vol_path); CHECK(hf_run(command,out,sizeof(out))==0);
    snprintf(command,sizeof(command),"python3 tests/host/libsetup_oracle.py '%s' '%s'",work,S.m.vol_path); CHECK(hf_run(command,out,sizeof(out))==0);
    a.reinstall=0;
    unsigned before=S.m.head.writes+S.m.vol.writes;
    expect_error(setup_txn_prepare(&a,&p),SETUP_ERR_DEST_CHANGED,SETUP_PHASE_PLAN);
    CHECK(S.m.head.writes+S.m.vol.writes==before);
    end();
}
static void test_errors(void)
{
    struct setup_answers a=answers(); struct setup_plan p;
    for(unsigned kind=0;kind<8;kind++) {
        begin(2097152); OK(setup_txn_prepare(&a,&p));
        enum setup_error_class error=SETUP_ERR_SOURCE_MISMATCH;
        if(kind==0) { S.fail_reads=99; error=SETUP_ERR_SOURCE_READ; }
        if(kind==1)S.hash_mismatch=true;
        if(kind==2) { S.change_generation=1; S.different_disc=true; }
        if(kind==3) { S.write_error=-ENOSPC; error=SETUP_ERR_DEST_FULL; }
        if(kind==4) { S.write_error=-EIO; error=SETUP_ERR_DEST_WRITE; }
        if(kind==5) { S.sync_error=-EIO; error=SETUP_ERR_DEST_FLUSH; }
        if(kind==6)S.corrupt=true;
        if(kind==7) { uint8_t mbr[512]={0}; mbr[1]=1; OK(fake_raw_write(&S.m.head,0,mbr)); error=SETUP_ERR_DEST_CHANGED; }
        int r=setup_txn_execute(&p,&callbacks,NULL);
        enum setup_phase failed=kind==7 ? SETUP_PHASE_PARTITION : SETUP_PHASE_COPY;
        expect_error(r,error,failed);
        if(kind==0) { CHECK(S.errors==5 && S.retries==5); struct setup_error e; OK(setup_txn_last_error(&e)); CHECK(e.attempt==5); }
        if(kind!=7 && kind!=2)expect_stub();
        end();
    }
    for(unsigned pphase=SETUP_PHASE_COPY;pphase<=SETUP_PHASE_PUBLISH;pphase+=2) {
        begin(2097152); OK(setup_txn_prepare(&a,&p)); S.fail_flush_phase=(enum setup_phase)pphase;
        expect_error(setup_txn_execute(&p,&callbacks,NULL),SETUP_ERR_DEST_FLUSH,(enum setup_phase)pphase);
        expect_stub(); end();
    }
    begin(2097152); OK(setup_txn_prepare(&a,&p)); S.remove_in_copy=true;
    expect_error(setup_txn_execute(&p,&callbacks,NULL),SETUP_ERR_MEDIA_REMOVED,SETUP_PHASE_COPY);
    CHECK(S.errors==5); expect_stub(); end();
    begin(2097152); OK(setup_txn_prepare(&a,&p)); S.fail_reads=1; S.cancel_error=true;
    expect_error(setup_txn_execute(&p,&callbacks,NULL),SETUP_ERR_CANCELLED,SETUP_PHASE_COPY);
    CHECK(S.errors==1); expect_stub(); end();
    begin(2097152); OK(setup_txn_prepare(&a,&p)); S.fail_manifest_reads=1;
    OK(setup_txn_execute(&p,&callbacks,NULL)); CHECK(S.errors==1); end();
    begin(2097152); OK(setup_txn_prepare(&a,&p)); S.fail_manifest_reads=99;
    expect_error(setup_txn_execute(&p,&callbacks,NULL),SETUP_ERR_SOURCE_READ,SETUP_PHASE_COPY);
    CHECK(S.errors==5); expect_stub(); end();
    begin(2097152); OK(setup_txn_prepare(&a,&p)); S.fail_identity_reads=1;
    OK(setup_txn_execute(&p,&callbacks,NULL)); CHECK(S.errors==1); end();
    begin(2097152); OK(setup_txn_prepare(&a,&p)); S.fail_info_reads=1;
    OK(setup_txn_execute(&p,&callbacks,NULL)); CHECK(S.errors==1); end();
    begin(2097152); OK(setup_txn_prepare(&a,&p)); S.fail_info_reads=99;
    expect_error(setup_txn_execute(&p,&callbacks,NULL),SETUP_ERR_SOURCE_READ,SETUP_PHASE_COPY);
    CHECK(S.errors==5); expect_stub(); end();
    /* Matching media replacement: reopen + identity verification + resume. */
    begin(2097152); OK(setup_txn_prepare(&a,&p)); S.change_generation=1;
    OK(setup_txn_execute(&p,&callbacks,NULL)); CHECK(S.errors==1); end();
}
static void test_cancel_and_cuts(void)
{
    struct setup_answers a=answers(); struct setup_plan p;
    for(unsigned phase=SETUP_PHASE_PARTITION;phase<=SETUP_PHASE_PUBLISH;phase++) {
        begin(2097152); OK(setup_txn_prepare(&a,&p)); S.cancel_phase=(enum setup_phase)phase;
        expect_error(setup_txn_execute(&p,&callbacks,NULL),SETUP_ERR_CANCELLED,(enum setup_phase)phase);
        if(phase>SETUP_PHASE_PARTITION)expect_stub();
        end();
    }
    /* Cut at each durable boundary P1..P4. The next phase's first I/O
     * fails; power_loss drops the volatile suffix, then CD reinstall. */
    for(unsigned phase=SETUP_PHASE_FORMAT;phase<=SETUP_PHASE_PUBLISH;phase++) {
        begin(2097152); S.m.head.reorder=true; S.m.vol.reorder=true;
        OK(setup_txn_prepare(&a,&p)); S.cut_phase=(enum setup_phase)phase;
        CHECK(setup_txn_execute(&p,&callbacks,NULL)<0);
        fake_power_loss(&S.m.head); fake_power_loss(&S.m.vol); expect_stub();
        if(phase>SETUP_PHASE_FORMAT) {
            char state[512]={0}; CHECK(b_read_file(NULL,'E',"SYSTEM/SETUP.STATE",state,sizeof(state)-1)>0);
            const char *previous=phase==SETUP_PHASE_COPY ? "phase=format\n" : phase==SETUP_PHASE_CONFIGURE ? "phase=copy\n" : "phase=configure\n";
            CHECK(strstr(state,previous));
            char command[1200],raw_state[512];
            snprintf(command,sizeof(command),"mtype -i '%s' ::/SYSTEM/SETUP.STATE",S.m.vol_path);
            OK(hf_run(command,raw_state,sizeof(raw_state))); CHECK(strstr(raw_state,previous));
        }
        H.s->vfs.ro_volumes[3]=NULL; stop();
        S.m.head.cut=S.m.vol.cut=false; S.cut_phase=0; S.phase=SETUP_PHASE_PLAN;
        start(&S.m,0); H.s->vfs.ro_volumes[3]=&S.reserved;
        st_set_platform(&platform); OK(setup_txn_prepare(&a,&p)); OK(setup_txn_execute(&p,&callbacks,NULL)); end();
    }
}
static void test_final_mbr_cuts(void)
{
    struct setup_answers a=answers(); struct setup_plan p;
    for(unsigned cut=1;cut<=3;cut++) {
        begin(2097152); OK(setup_txn_prepare(&a,&p));
        S.m.head.reorder=true; S.publication_cut=cut;
        int r=setup_txn_execute(&p,&callbacks,NULL);
        CHECK(r<0);
        fake_power_loss(&S.m.head); fake_power_loss(&S.m.vol);
        uint8_t mbr[512]; OK(fake_raw_read(&S.m.head,0,mbr));
        CHECK(!memcmp(mbr,cut<3 ? disk_stub_mbr_code : disk_canonical_mbr_code,440));
        char command[1200],state[512];
        snprintf(command,sizeof(command),"mtype -i '%s' ::/SYSTEM/SETUP.STATE",S.m.vol_path);
        OK(hf_run(command,state,sizeof(state))); CHECK(strstr(state,"phase=installed\n"));
        S.publication_cut=0; S.publishing_mbr=false;
        H.s->vfs.ro_volumes[3]=NULL; stop();
        start(&S.m,0); H.s->vfs.ro_volumes[3]=&S.reserved;
        a.reinstall=cut<3 ? 0 : 1;
        st_set_platform(&platform); OK(setup_txn_prepare(&a,&p)); OK(setup_txn_execute(&p,&callbacks,NULL)); end();
    }
}
static void test_large_preview(void)
{
    begin(2097152);struct setup_answers a=answers();struct setup_plan p;
    uint8_t sector[512]={0};sector[510]=0x55;sector[511]=0xaa;sector[450]=0x0f;
    disk_wr32(sector+454,2048);disk_wr32(sector+458,2097152-2048);
    OK(fake_raw_write(&S.m.head,0,sector));
    for(unsigned i=0;i<19;i++) {
        memset(sector,0,sizeof(sector));sector[510]=0x55;sector[511]=0xaa;sector[450]=0x83;
        disk_wr32(sector+454,1);disk_wr32(sector+458,1024);
        if(i<18){sector[466]=0x0f;disk_wr32(sector+470,(i+1)*4096);disk_wr32(sector+474,2097152-2048-(i+1)*4096);}
        OK(fake_raw_write(&S.m.vol,(uint64_t)i*4096,sector));
    }
    unsigned before=S.m.head.writes+S.m.vol.writes;
    OK(setup_txn_prepare(&a,&p));CHECK(p.erased_partitions==16 && p.erased_total==20 && p.erased[0].type==0x0f);
    CHECK(S.m.head.writes+S.m.vol.writes==before);end();
    begin(2097152);S.bad_template=true;
    expect_error(setup_txn_prepare(&a,&p),SETUP_ERR_SOURCE_MISMATCH,SETUP_PHASE_PLAN);
    CHECK(!S.m.head.writes && !S.m.vol.writes);end();
    begin(2097152);S.alias_mismatch=true;st_set_platform(NULL);
    OK(setup_txn_prepare(&a,&p));expect_error(setup_txn_execute(&p,&callbacks,NULL),SETUP_ERR_CONSISTENCY,SETUP_PHASE_COPY);end();
}
static void test_native_configuration(void)
{
    for(unsigned mode=SETUP_NET_DHCP;mode<=SETUP_NET_NONE;mode++) {
        begin(2097152);S.nics=true;struct setup_answers a=answers();struct setup_plan p;
        a.network=(enum setup_net_mode)mode;a.tz_minutes=120;a.clock=mode==SETUP_NET_STATIC ? SETUP_CLOCK_UTC : SETUP_CLOCK_LOCAL;a.clock_adjust_s=3600;
        if(mode==SETUP_NET_STATIC) {
            const uint8_t ip[]={192,0,2,2},gw[]={192,0,2,1},dns[]={1,1,1,1};
            memcpy(a.ip,ip,4);memcpy(a.gateway,gw,4);memcpy(a.dns,dns,4);a.prefix=24;
            a.components|=SETUP_COMP_DOS|SETUP_COMP_GAMES;
        }
        st_set_platform(NULL);unsigned rtc_before=rtc_writes;
        OK(setup_txn_prepare(&a,&p));CHECK(!S.m.head.writes && !S.m.vol.writes && rtc_writes==rtc_before);
        CHECK(p.version==2 && p.payload_bytes+p.overhead_bytes+p.reserve_bytes==p.required_bytes);
        OK(setup_txn_execute(&p,&callbacks,NULL));
        CHECK(rtc_writes==rtc_before+1 && last_clock.seconds==1792703600 && last_clock.offset_minutes==120 && last_clock.rtc_local==(a.clock==SETUP_CLOCK_LOCAL));
        char text[4096]={0};int n=b_read_file(NULL,'E',"SYSTEM/NETWORK.INI",text,sizeof(text));CHECK(n>0);
        struct ciuki_net_config config[2];struct net_config_identity ids[2];
        OK(net_config_parse(text,(unsigned)n,config,ids));CHECK(ids[0].vendor==0x8086 && ids[1].mac[5]==2);
        CHECK(config[0].enabled==(mode!=SETUP_NET_NONE) && config[0].dhcp==(mode!=SETUP_NET_STATIC));
        CHECK(config[1].dhcp && !strcmp(config[0].hostname,"CIUKI-PC"));
        if(mode==SETUP_NET_STATIC)CHECK(!memcmp(&config[0].address,a.ip,4) && !memcmp(&config[0].gateway,a.gateway,4) && !memcmp(&config[0].dns[0],a.dns,4));
        n=b_read_file(NULL,'E',"System/Registry/System.reg",text,sizeof(text));CHECK(n>0);
        struct rt *tree=reg_parse("System",text,(unsigned)n);CHECK(tree);
        struct rk *key=reg_key(tree,"System\\Identity");CHECK(key);bool user=false,computer=false;
        for(struct rv *v=key->values;v;v=v->next) { if(!strcmp(v->name,"UserName"))user=!strcmp(v->data,"Tester");if(!strcmp(v->name,"ComputerName"))computer=!strcmp(v->data,"CIUKI-PC"); }
        CHECK(user && computer);reg_tree_free(tree);
        n=b_read_file(NULL,'E',"System/Registry/Programs.reg",text,sizeof(text));CHECK(n>0);text[n]=0;
        CHECK(strstr(text,"wolf3d-shareware") && strstr(text,"doom-shareware") && strstr(text,"quake-shareware"));
        tree=reg_parse("Programs",text,(unsigned)n);CHECK(tree);key=reg_key(tree,"Programs\\BundledGames\\doom-shareware");CHECK(key);
        bool installed=false;for(struct rv *v=key->values;v;v=v->next)if(!strcmp(v->name,"Installed")) { int32_t value;memcpy(&value,v->data,4);installed=value==(mode==SETUP_NET_STATIC); }
        CHECK(installed);reg_tree_free(tree);
        n=b_read_file(NULL,'E',"system/settings/desktop.cfg",text,sizeof(text));CHECK(n>0);
        struct ciuki_settings cfg;cfg_defaults(&cfg);CHECK(!cfg_parse(&cfg,text,(unsigned)n) && cfg.keyboard==CFG_KEYS_IT);
        /* G1: a fixed offset is recorded as such (clock.cfg version 2). */
        n=b_read_file(NULL,'E',"system/settings/clock.cfg",text,sizeof(text));CHECK(n>0);
        struct tzr_config clock;CHECK(tzr_config_parse(text,(size_t)n,&clock) && !strcmp(clock.tz,"<+02>-2") && !clock.zone[0]);
        CHECK(clock.rtc_local==(a.clock==SETUP_CLOCK_LOCAL) && clock.rtc_bias_minutes==(clock.rtc_local ? 120 : 0));
        end();
    }
    /* G1: Europe/Rome. The confirmed time 1792703600 (22 October 2026) is
     * CEST: the RTC gets UTC+2 and clock.cfg records that bias; the
     * registry names the zone and its rule. */
    static const int64_t adjust[2]={3600,3600+60*86400};   /* 22 October (CEST), 21 December (CET) */
    for(unsigned k=0;k<2;k++) {
        begin(2097152);S.nics=true;struct setup_answers a=answers();struct setup_plan p;
        a.tz_minutes=60;snprintf(a.tz_zone,sizeof(a.tz_zone),"Europe/Rome");a.clock=SETUP_CLOCK_LOCAL;a.clock_adjust_s=adjust[k];
        st_set_platform(NULL);unsigned rtc_before=rtc_writes;
        OK(setup_txn_prepare(&a,&p));OK(setup_txn_execute(&p,&callbacks,NULL));
        CHECK(rtc_writes==rtc_before+1 && last_clock.offset_minutes==(k ? 60 : 120) && last_clock.rtc_local);
        char text[4096]={0};int n=b_read_file(NULL,'E',"system/settings/clock.cfg",text,sizeof(text));CHECK(n>0);
        struct tzr_config clock;CHECK(tzr_config_parse(text,(size_t)n,&clock));
        CHECK(!strcmp(clock.zone,"Europe/Rome") && !strcmp(clock.tz,"CET-1CEST,M3.5.0,M10.5.0/3") && clock.auto_dst && clock.offset_minutes==60);
        CHECK(clock.rtc_local && clock.rtc_bias_minutes==(k ? 60 : 120));
        n=b_read_file(NULL,'E',"System/Registry/System.reg",text,sizeof(text));CHECK(n>0);text[n]=0;
        CHECK(strstr(text,"Europe/Rome") && strstr(text,"CET-1CEST,M3.5.0,M10.5.0/3") && strstr(text,"AutoDST"));
        end();
    }
    /* A zone that is not bundled, or whose offset disagrees, is refused before any write. */
    begin(2097152);{struct setup_answers a=answers();struct setup_plan p;
        snprintf(a.tz_zone,sizeof(a.tz_zone),"Mars/Olympus");CHECK(setup_txn_prepare(&a,&p)!=0);
        snprintf(a.tz_zone,sizeof(a.tz_zone),"Europe/Rome");a.tz_minutes=120;CHECK(setup_txn_prepare(&a,&p)!=0);}
    end();
}
static void test_restart(void)
{
    memset(&S,0,sizeof(S)); st_set_platform(&platform);
    S.removed_at=2500; OK(setup_restart(SETUP_RESTART_EJECT)); CHECK(S.tray_opens==1 && S.restarts==1 && S.time_ms==2500);
    S.time_ms=0; S.removed=false; S.removed_at=70000; OK(setup_restart(SETUP_RESTART_WAIT)); CHECK(S.time_ms==70000);
    S.time_ms=0; S.removed=false; S.removed_at=0; OK(setup_restart(SETUP_RESTART_FORCE)); CHECK(S.time_ms==60000);
    S.time_ms=0; S.removed=false; OK(setup_restart(SETUP_RESTART_EJECT)); CHECK(S.time_ms==60000);
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    work=argv[1];
    test_prepare(); test_complete(); test_errors(); test_cancel_and_cuts(); test_final_mbr_cuts(); test_native_configuration(); test_large_preview(); test_restart();
    free(H.log_data);
    printf("PASS libsetup: %u checks; transaction heap peak=%zu bytes; fixed transfer buffers=196608 bytes (libdisk included)\n",checks,heap_peak);
    return 0;
}
