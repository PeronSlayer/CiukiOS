/* Production broker + libreg transport with storage/kernel boundary spies.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../../../apps/registry/broker.h"
#include "../../../apps/registry/restart.h"
#include "../../../sdk/libreg/internal.h"
#include "../../../sdk/libreg/transport.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct boundary {
    struct rb_identity peer[3];int live[3];unsigned refs[3];
    char *roots[2];size_t lengths[2];unsigned writes,reads;
    uint8_t *journal;size_t journal_size;unsigned launches,applies,quarantines;
    char *user_file;size_t user_bytes;unsigned user_reads,user_writes;
    unsigned cut,step;int demoted,deleted,replaced,launch_fail;
};
static struct boundary f;
static struct rb_broker broker;
static int role(void *p){(void)p;return 0;}
static int inspect(void *p,uintptr_t lease,struct rb_identity *out){struct boundary *b=p;if(!lease||lease>3||!b->live[lease-1])return -ESRCH;*out=b->peer[lease-1];return 0;}
static int retain(void *p,uintptr_t l){struct boundary *b=p;if(!l||l>3||!b->live[l-1])return -ESRCH;b->refs[l-1]++;return 0;}
static void release(void *p,uintptr_t l){struct boundary *b=p;assert(l&&l<=3&&b->refs[l-1]);b->refs[l-1]--;}
static struct rb_authority authority(void){return (struct rb_authority){&f,role,retain,release,inspect};}
static int storage_read(void *p,unsigned root,char **out,size_t *n){struct boundary *b=p;if(root<1||root>2)return -EACCES;b->reads++;*n=b->lengths[root-1];*out=malloc(*n);assert(*out);memcpy(*out,b->roots[root-1],*n);return 0;}
static int storage_write(void *p,unsigned root,const char *v,size_t n,uint64_t expected){struct boundary *b=p;struct rt *old=reg_parse(root==1?"System":"Programs",b->roots[root-1],b->lengths[root-1]);assert(old);int e=old->generation==expected?0:-EAGAIN;reg_tree_free(old);if(e)return e;char *data=malloc(n);assert(data);memcpy(data,v,n);free(b->roots[root-1]);b->roots[root-1]=data;b->lengths[root-1]=n;b->writes++;return 0;}
void reg_remote_close(struct reg_remote *r){if(r->fd>=0)rb_disconnect(&broker,(unsigned)r->fd);r->fd=-1;}
int reg_registry_rpc(struct reg_remote *r,unsigned op,const void *p,size_t n,void *out,size_t cap,size_t *used){
    if(r->fd<0){unsigned slot;int e=rb_connect(&broker,1,&slot);if(e)return reg_fail(-e);r->fd=(int)slot;}
    int e=rb_exchange(&broker,(unsigned)r->fd,op,p,n,out,cap,used);return e?reg_fail(-e):0;
}
/* No machine file read/write is reachable through libreg in this build. */
int reg_file_read(const char *p,char **out,size_t *n){
    if(strcmp(p,"/user/import.reg")||!f.live[0]||!f.user_file)return reg_fail(EACCES);
    *out=malloc(f.user_bytes+1);assert(*out);memcpy(*out,f.user_file,f.user_bytes);(*out)[f.user_bytes]=0;*n=f.user_bytes;f.user_reads++;return 0;
}
int reg_file_write(const char *p,const char *r,const char *s,size_t n,uint64_t g){
    (void)r;(void)g;if(strcmp(p,"/user/export.reg")||!f.live[0])return reg_fail(EACCES);
    char *copy=malloc(n+1);assert(copy);memcpy(copy,s,n);copy[n]=0;free(f.user_file);f.user_file=copy;f.user_bytes=n;f.user_writes++;return 0;
}
int reg_recover(const char *p,const char *r){(void)r;return !strcmp(p,"/user/export.reg")?0:reg_fail(EACCES);}
struct rt *reg_hardware(uint64_t previous){(void)previous;reg_fail(EROFS);return NULL;}
static void native_cases(void){
    struct reg_root *r=reg_open_root_at("System","/caller-cannot-select-authority");assert(r);
    assert(reg_generation(r)==0);
    assert(reg_set_int32(r,"System\\Clock","value",123)==-1&&errno==EACCES&&f.writes==0);
    f.peer[0].groups[0]=544;
    assert(!reg_set_int32(r,"System\\Clock","value",123));assert(f.writes==1);
    int32_t v=0;assert(!reg_get_int32(r,"System\\Clock","value",&v)&&v==123);
    /* Root bigger than a logical RPC body: production parser/serializer and
     * upload/page loop stream it without raising the 8192-byte bound. */
    char value[4090];memset(value,'x',sizeof(value)-1);value[sizeof(value)-1]=0;
    for(unsigned i=0;i<5;i++){char key[32];snprintf(key,sizeof(key),"k%u",i);assert(!reg_set_string(r,"System\\Large",key,value));}
    assert(f.lengths[0]>8192);assert(reg_generation(r)==6);
    assert(!reg_export(r,"System\\Clock","/user/export.reg")&&f.user_writes==1);
    assert(strstr(f.user_file,"Clock")&&strstr(f.user_file,"123"));
    free(f.user_file);f.user_file=strdup("; ciuki-registry generation=0\n[System\\Imported]\nvalue = 456\n");assert(f.user_file);f.user_bytes=strlen(f.user_file);
    assert(!reg_import(r,"/user/import.reg")&&f.user_reads==1);
    assert(!reg_get_int32(r,"System\\Imported","value",&v)&&v==456&&reg_generation(r)==7);
    unsigned writes=f.writes;f.peer[0].groups[0]=545;
    assert(reg_import(r,"/user/import.reg")==-1&&errno==EACCES&&f.writes==writes&&reg_generation(r)==7);
    f.peer[0].groups[0]=544;
    f.live[0]=0;assert(reg_generation(r)==REG_GENERATION_ERROR&&errno==ESRCH);
    assert(reg_get_int32(r,"System\\Clock","value",&v)==-1&&errno==ESRCH);
    reg_close_root(r);assert(!f.refs[0]);f.live[0]=1;
    unsigned slot;assert(!rb_connect(&broker,1,&slot));uint8_t req[32]={0},out[32];size_t used;
    rb_put32(req,1);rb_put32(req+4,1);rb_put32(req+12,10);rb_put64(req+16,5);
    assert(!rb_exchange(&broker,slot,RB_NATIVE_BEGIN,req,32,out,32,&used));
    assert(rb_exchange(&broker,slot,RB_NATIVE_COMMIT,req,32,out,32,&used)==-EINVAL);
    rb_disconnect(&broker,slot);assert(!f.refs[0]);
    struct rt *candidate=reg_parse("System",f.roots[0],f.lengths[0]);assert(candidate);candidate->generation=6;
    struct reg_remote stale={.fd=-1,.root=1};unsigned writes_before=f.writes;
    assert(reg_remote_commit(&stale,candidate,5)==-1&&errno==EAGAIN&&f.writes==writes_before);
    reg_remote_close(&stale);reg_tree_free(candidate);
    puts("H07 native: no backing access, rights, generation, staged >8192 root, revoke, cleanup PASS");
}
static unsigned hive_writes;
static struct {char path[160];uint8_t *bytes;size_t size;} hive_files[8];
static unsigned hive_file(const char *name,int create){for(unsigned i=0;i<8;i++)if(!strcmp(hive_files[i].path,name))return i;
    if(create)for(unsigned i=0;i<8;i++)if(!hive_files[i].path[0]){assert(strlen(name)<160);strcpy(hive_files[i].path,name);return i;}return 8;}
static int hive_read(void *p,const char *name,uint8_t **out,size_t *n){(void)p;unsigned i=hive_file(name,0);if(i==8)return 0;
    *n=hive_files[i].size;*out=malloc(*n);assert(*out);memcpy(*out,hive_files[i].bytes,*n);return 1;}
static int hive_write(void *p,const char *name,const void *v,size_t n){(void)p;
    assert(!strncmp(name,"/System/Security/Registry/Machine/",34)||!strncmp(name,"/Windows/Profiles/u000003e8/Settings/Registry/",45));
    unsigned i=hive_file(name,1);assert(i<8);uint8_t *copy=malloc(n);assert(copy);memcpy(copy,v,n);free(hive_files[i].bytes);
    hive_files[i].bytes=copy;hive_files[i].size=n;hive_writes++;return 0;}
static int hive_patch(void *p,const char *name,size_t at,const void *v,size_t n){(void)p;unsigned i=hive_file(name,0);
    assert(i<8&&at<=hive_files[i].size&&n<=hive_files[i].size-at);memcpy(hive_files[i].bytes+at,v,n);return 0;}
static int hive_sync(void *p,const char *name){(void)p;(void)name;return 0;}
static void hive_log(void *p,const char *name){(void)p;(void)name;}
static struct w32r_reply w32_call(unsigned slot,unsigned op,uint32_t key,const char *path,uint32_t access,const void *data,uint32_t bytes)
{
    size_t n=sizeof(struct w32r_request)+strlen(path)+2+bytes;uint8_t *in=calloc(1,n),out[2048];assert(in);
    struct w32r_request request={.version=1,.op=op,.key=key,.access=access,.path_bytes=(uint32_t)strlen(path)+1,.name_bytes=1,.type=3,.data_bytes=bytes};
    if(op==W32R_OPEN||op==W32R_CREATE)request.type=0;
    memcpy(in,&request,sizeof(request));memcpy(in+sizeof(request),path,strlen(path)+1);if(bytes)memcpy(in+n-bytes,data,bytes);
    size_t used=0;int e=rb_exchange(&broker,slot,RB_WIN32,in,n,out,sizeof(out),&used);free(in);assert(!e&&used>=sizeof(struct w32r_reply));
    struct w32r_reply reply;memcpy(&reply,out,sizeof(reply));return reply;
}
static void win32_cases(void)
{
    struct wr_server s;struct wr_io io={NULL,hive_read,hive_write,hive_patch,hive_sync,hive_log};assert(wr_init_secure(&s,&io,0));
    rb_destroy(&broker);struct rb_authority a=authority();struct rb_storage storage={.context=&f,.read=storage_read,.write=storage_write};
    assert(!rb_init(&broker,&a,&storage,&s));
    struct rb_identity saved=f.peer[2];f.peer[2].uid=0;f.peer[2].session=0;unsigned system;
    assert(!rb_connect(&broker,3,&system));
    struct w32r_reply template=w32_call(system,W32R_OPEN,W32R_HKCU,"",0x20019,NULL,0);
    assert(!template.error); /* SYSTEM HKCU is the protected template, never Alice. */
    template=w32_call(system,W32R_CREATE,W32R_HKCU,"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",0xf003f,NULL,0);assert(!template.error);
    template=w32_call(system,W32R_SET,template.key,"",0,"unowned-template-command",25);assert(!template.error);
    rb_disconnect(&broker,system);f.peer[2]=saved;
    strcpy(f.peer[0].profile,"/Windows/Profiles/u000003e8");unsigned fresh;assert(!rb_connect(&broker,1,&fresh));
    template=w32_call(fresh,W32R_OPEN,W32R_HKCU,"",0x20019,NULL,0);assert(!template.error);
    struct wr_key *startup=wr_walk(&s,1,"S-1-5-21-1-2-3-1000\\Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce");
    assert(startup&&!startup->values&&!wr_child(&s,s.roots[1],".DEFAULT"));
    rb_disconnect(&broker,fresh);rb_destroy(&broker);
    s.subject.uid=1000;s.subject.session=1;strcpy(s.user_branch,"S-1-5-21-1-2-3-1000");
    assert(wr_child(&s,s.roots[1],s.user_branch));assert(wr_add_key(&s,s.roots[1],"S-1-5-21-1-2-3-1001",1));assert(!wr_flush(&s));
    assert(!rb_init(&broker,&a,&storage,&s));unsigned slot;assert(!rb_connect(&broker,1,&slot));f.peer[0].groups[0]=545;
    unsigned before=hive_writes;struct w32r_reply r=w32_call(slot,W32R_CREATE,W32R_HKLM,"Forbidden",0xf003f,NULL,0);
    assert(r.error==5&&hive_writes==before&&!wr_child(&s,s.roots[0],"Forbidden"));
    r=w32_call(slot,W32R_CREATE,W32R_HKCU,"Software",0xf003f,NULL,0);assert(!r.error);uint32_t own=r.key;
    r=w32_call(slot,W32R_OPEN,own,"",0x20019,NULL,0);assert(!r.error);uint32_t read_only=r.key;
    r=w32_call(slot,W32R_SET,read_only,"",0,"x",1);assert(r.error==5);
    r=w32_call(slot,W32R_OPEN,W32R_HKU,".DEFAULT",0x20019,NULL,0);assert(r.error==5);
    r=w32_call(slot,W32R_OPEN,W32R_HKU,"S-1-5-21-1-2-3-1001",0x20019,NULL,0);assert(r.error==5);
    unsigned other;assert(!rb_connect(&broker,1,&other));r=w32_call(other,W32R_SET,own,"",0,"x",1);assert(r.error==6);rb_disconnect(&broker,other);
    f.peer[0].groups[0]=544;
    r=w32_call(slot,W32R_OPEN,W32R_HKU,"S-1-5-21-1-2-3-1001",0x20019,NULL,0);assert(!r.error);
    r=w32_call(slot,W32R_DELETE_KEY,W32R_HKU,s.user_branch,0,NULL,0);assert(r.error==5);
    r=w32_call(slot,W32R_CREATE,W32R_HKLM,"Allowed",0xf003f,NULL,0);assert(!r.error);
    rb_disconnect(&broker,slot);rb_destroy(&broker);wr_destroy(&s);
    for(unsigned i=0;i<8;i++)free(hive_files[i].bytes);
    puts("H07 Win32: retained rights, HKCU, .DEFAULT/foreign HKU, connection handles and machine admin admission PASS");
}
static int jr_read(void *p,unsigned s,uint32_t uid,uint8_t **out,size_t *n){struct boundary *b=p;(void)s;(void)uid;if(!b->journal)return -ENOENT;*out=malloc(b->journal_size);assert(*out);memcpy(*out,b->journal,b->journal_size);*n=b->journal_size;return 0;}
static int jr_write(void *p,unsigned s,uint32_t uid,const void *v,size_t n){struct boundary *b=p;(void)s;(void)uid;if(++b->step==b->cut)return -EIO;uint8_t *data=malloc(n);assert(data);memcpy(data,v,n);free(b->journal);b->journal=data;b->journal_size=n;return 0;}
static int account(void *p,uint32_t uid,struct rb_identity *c,int *enabled){struct boundary *b=p;*c=b->peer[0];c->uid=uid;c->account_generation=7;if(b->demoted){c->groups[0]=545;c->group_count=1;}*enabled=!b->deleted;return 0;}
static int authorize(void *p,uintptr_t lease,const struct rb_identity *c,unsigned scope,uint8_t *job,int replay){struct boundary *b=p;(void)lease;(void)c;(void)scope;if(replay&&b->replaced)return -ESTALE;if(!replay){memset(job+112,0x55,80);}else for(unsigned i=112;i<192;i++)assert(job[i]==0x55);return 0;}
static int apply(void *p,uintptr_t lease,const uint8_t *j){struct boundary *b=p;(void)lease;(void)j;b->applies=1;return 0;}
static int launch(void *p,uintptr_t lease,const uint8_t *j){struct boundary *b=p;(void)j;assert(lease>=1&&lease<=3&&b->live[lease-1]);assert(rb_get32(j+24)==b->peer[lease-1].uid);b->launches++;return b->launch_fail?-EIO:0;}
static int quarantine(void *p){((struct boundary *)p)->quarantines++;return 0;}
static struct rj_backend backend(void){return (struct rj_backend){&f,jr_read,jr_write,account,authorize,apply,launch,quarantine};}
static void restart_cases(void){
    struct rb_authority a=authority();struct rj_backend b=backend();struct rj_queue q;uint32_t sid[]={1,2,3};uint64_t id;
    struct rj_spec spec={.kind=RJ_PROGRAM,.mask=1,.source="",.target="",.executable="/Games/start.exe",.arguments="",.cwd="/Games"};
    assert(!rj_open(&q,&a,&b,RJ_PERSONAL,1000,sid,42));assert(!rj_admit(&q,1,&spec,&id));
    f.live[0]=0;assert(rj_replay(&q,1,id)==-ESRCH);assert(rj_replay(&q,2,id)==-EACCES&&f.launches==0);
    rj_close(&q);f.live[0]=1;f.peer[0].session=11;f.launch_fail=1;
    assert(!rj_open(&q,&a,&b,RJ_PERSONAL,1000,sid,42));assert(rj_replay(&q,1,id)==-EIO&&f.launches==1);
    rj_close(&q);assert(!rj_open(&q,&a,&b,RJ_PERSONAL,1000,sid,42));
    assert(rj_replay(&q,1,id)==-EAGAIN&&f.launches==1);assert(rj_reconcile(&q,2,id,1)==-EACCES);
    assert(!rj_reconcile(&q,1,id,1));assert(!rj_replay(&q,1,id)&&f.launches==1);
    assert(!rj_quarantine(&q)&&f.quarantines==1);rj_close(&q);free(f.journal);f.journal=NULL;
    f.launch_fail=0;assert(!rj_open(&q,&a,&b,RJ_MACHINE,0,sid,42));
    f.peer[0].groups[0]=545;assert(rj_admit(&q,1,&spec,&id)==-EACCES);
    f.peer[0].groups[0]=544;assert(!rj_admit(&q,1,&spec,&id));
    f.demoted=1;assert(rj_replay(&q,1,id)==-EACCES);f.demoted=0;
    f.deleted=1;assert(rj_replay(&q,1,id)==-ENOENT);assert(!rj_cancel_deleted(&q));f.deleted=0;
    assert(!rj_replay(&q,1,id));assert(!rj_admit(&q,1,&spec,&id));
    f.replaced=1;assert(rj_replay(&q,1,id)==-ESTALE);f.replaced=0;
    f.peer[1].groups[0]=544;uint64_t approved=0;assert(!rj_reapprove(&q,2,id,&approved)&&approved>id);
    assert(rj_replay(&q,1,approved)==-EACCES);assert(!rj_replay(&q,2,approved));f.peer[1].groups[0]=545;
    rj_close(&q);free(f.journal);f.journal=NULL;
    /* Cut at PREPARED/APPLY_REQUIRED/APPLIED/COMPLETE persistence, reopen and
     * resume as Alice, never Bob. File callback is receipt-idempotent. */
    spec.kind=RJ_FILE;spec.source="/Games/a";spec.target="/Games/b";spec.executable="";
    for(unsigned cut=1;cut<=4;cut++){
        f.cut=cut;f.step=0;f.applies=0;
        assert(!rj_open(&q,&a,&b,RJ_PERSONAL,1000,sid,42));
        int e=rj_admit(&q,1,&spec,&id);if(!e)e=rj_replay(&q,1,id);assert(e==-EIO);rj_close(&q);
        f.cut=0;assert(!rj_open(&q,&a,&b,RJ_PERSONAL,1000,sid,42));
        if(cut>1){assert(rj_replay(&q,2,id)==-EACCES);assert(!rj_replay(&q,1,id)&&f.applies==1);}
        rj_close(&q);free(f.journal);f.journal=NULL;
    }
    assert(!rj_open(&q,&a,&b,RJ_PERSONAL,1000,sid,42));
    for(unsigned i=0;i<RJ_JOBS;i++)assert(!rj_admit(&q,1,&spec,&id));
    unsigned before=f.step;assert(rj_admit(&q,1,&spec,&id)==-ENOSPC&&f.step==before);
    rj_close(&q);f.journal[80]^=1;assert(rj_open(&q,&a,&b,RJ_PERSONAL,1000,sid,42)==-EIO);f.journal[80]^=1;
    rb_put32(f.journal+8,1);assert(rj_open(&q,&a,&b,RJ_PERSONAL,1000,sid,42)==-EIO);
    free(f.journal);f.journal=NULL;
    puts("H07 restart: Alice/Bob, demotion/deletion, replacement, crash phases, uncertain HANDED, quarantine PASS");
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    for(unsigned i=0;i<3;i++){f.live[i]=1;f.peer[i]=(struct rb_identity){.uid=1000+i,.session=1+i,.groups={545},.group_count=1,.process_generation=1,.machine_sid={1,2,3},.volume=42,.account_generation=1};}
    for(unsigned i=0;i<2;i++){struct rt *t=reg_tree_new(i?"Programs":"System");assert(t);f.roots[i]=reg_serialise(t,NULL,&f.lengths[i]);assert(f.roots[i]);reg_tree_free(t);}
    struct rb_authority a=authority();struct rb_storage s={.context=&f,.read=storage_read,.write=storage_write};
    assert(!rb_init(&broker,&a,&s,NULL));native_cases();win32_cases();rb_destroy(&broker);restart_cases();
    free(f.user_file);
    for(unsigned i=0;i<2;i++)free(f.roots[i]);for(unsigned i=0;i<3;i++)assert(!f.refs[i]);
    puts("H07 broker production: PASS");return 0;
}
