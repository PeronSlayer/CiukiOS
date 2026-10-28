/* sec-runtime1: real production client frames -> production service decoders
 * and handlers. Only kernel channels/identity, storage and clocks are fixtures.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "fake.h"
#include "../../../apps/registry/server.h"
#include "../../../sdk/libreg/transport.h"
#include "../../../apps/desktop/win32_adapter.h"
#include "../../../apps/desktop/startup.h"
#include "../../../apps/control/accounts_client.h"
#include "../../../apps/logon/logon_proto.h"
#include <ciuki/abi.h>
#include <ciuki/sha256.h>
#include <pwd.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct fake storage;
static struct acct_service accounts;
static struct acct_transport decoder;
static struct rb_server registry;
static struct wr_server hive;
static int lose_reply;
static unsigned caller=1, open_account[FAKE_PEERS], open_registry, serial;
static struct ciuki_message replies[FAKE_PEERS+1][64], inbound;
static unsigned heads[FAKE_PEERS+1], tails[FAKE_PEERS+1];
static int incoming;
static const void *pointers[32];
uint32_t runtime_pointer(const void *p){unsigned i=++serial%32;pointers[i]=p;return i+1;}
static void *pointer(uint32_t i){assert(i>=1&&i<=32);return (void *)(uintptr_t)pointers[i-1];}
static unsigned queue_for(int fd){assert(fd>=500&&fd<=500+FAKE_PEERS);return (unsigned)(fd-500);}
static void enqueue(int fd,const struct ciuki_message *m){unsigned q=queue_for(fd);assert(tails[q]<64);replies[q][tails[q]++]=*m;}
static void reset_queue(int fd){unsigned q=queue_for(fd);assert(heads[q]==tails[q]);heads[q]=tails[q]=0;}
static int deliver(void *ctx,const struct acct_subject *s,unsigned op,uint64_t generation,int status,const void *body,size_t n)
{
    (void)ctx;int peer=-1;
    for(unsigned i=0;i<FAKE_PEERS;i++)if(open_account[i]&&s->endpoint_generation==storage.peers[i].subject.endpoint_generation)peer=(int)i;
    assert(peer>=0);size_t at=0;
    do{struct ciuki_message m={.sender_pid=5};size_t bytes=0;
        assert(!acct_reply_frame(s,op,s->request_id,generation,status,body,n,at,m.data,&bytes));
        m.length=(uint32_t)bytes;enqueue(500+peer,&m);at+=bytes-64;
    }while(at<n);return 0;
}
int ciuki_security(struct ciuki_security_args *a)
{
    assert(a->operation==CIUKI_SECURITY_SERVICE_CONNECT);
    struct ciuki_security_link *q=pointer(a->spawn_address),*r=pointer(a->output_address);
    memset(r,0,sizeof(*r));r->endpoint_generation=++serial;r->service_generation=1;
    if(q->service==2){
        assert(!open_account[caller]);open_account[caller]=1;
        storage.peers[caller].last_request=0;
        storage.peers[caller].subject.endpoint_generation=r->endpoint_generation;
        r->endpoint_fd=500+(int)caller;r->server_role=CIUKI_SECURITY_ROLE_ACCOUNT;r->peer_pid=5;
        reset_queue(r->endpoint_fd);
    }else{
        assert(q->service==4&&!open_registry);open_registry=1;
        unsigned slot;assert(!rb_connect(&registry.broker,1,&slot));
        registry.clients[0]=(struct rb_client){.fd=500+FAKE_PEERS,.lease=1,.slot=slot,.endpoint=r->endpoint_generation};
        r->endpoint_fd=500+FAKE_PEERS;r->server_role=CIUKI_SECURITY_ROLE_REGISTRY;r->peer_pid=6;
        reset_queue(r->endpoint_fd);
    }
    r->service=q->service;r->peer_role=r->server_role;r->peer_process_generation=1;
    return 0;
}
int ciuki_security_self(struct ciuki_credential *c)
{struct acct_subject *s=&storage.peers[caller].subject;memset(c,0,sizeof(*c));c->uid=s->uid;c->session_id=s->session_id;c->group_count=s->group_count;memcpy(c->groups,s->groups,sizeof(c->groups));return 0;}
int runtime_close(int fd)
{
    if(fd==500+FAKE_PEERS){assert(open_registry);rb_server_destroy(&registry);open_registry=0;}
    else{unsigned p=queue_for(fd);assert(open_account[p]);acct_transport_drop(&decoder,&accounts,(int)p);open_account[p]=0;}
    return 0;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    (void)flags;reset_queue(fd);
    if(fd==500+FAKE_PEERS){assert(!incoming);inbound=*m;incoming=1;rb_server_step(&registry,0);}
    else{
        unsigned p=queue_for(fd);struct acct_response result={0};int e=acct_frame(&decoder,&accounts,(int)p,m->data,m->length,&result);
        if(e<0){struct acct_subject s=storage.peers[p].subject;s.retained=1;s.request_id=rb_get64(m->data+24);
            assert(!deliver(NULL,&s,rb_get32(m->data+16),0,e,NULL,0));}
        else if(!e)assert(acct_work(&accounts,&result)==1);
    }
    return 1;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{(void)flags;unsigned q=queue_for(fd);if(heads[q]==tails[q]){errno=EAGAIN;return -1;}*m=replies[q][heads[q]++];return 1;}
/* REGISTRY's real frame decoder runs in rb_server_step. */
int acct_sys_channel_recv(int fd,struct ciuki_message *m,int wait)
{(void)fd;(void)wait;if(!incoming)return -EAGAIN;*m=inbound;incoming=0;return 1;}
int acct_sys_channel_send(int fd,const struct ciuki_message *m)
{struct ciuki_message copy=*m;copy.sender_pid=6;enqueue(fd,&copy);return 0;}
int acct_sys_wait_readable(const int *fds,unsigned n,unsigned ms){(void)fds;(void)n;(void)ms;return 0;}
uint64_t acct_sys_monotonic_ms(void){return storage.now;}
int rb_kernel_live(struct rb_kernel *k,uintptr_t l){(void)k;(void)l;return 0;}
int rb_kernel_accept(struct rb_kernel *k,uintptr_t *l,int *fd,uint32_t *g){(void)k;(void)l;(void)fd;(void)g;return -EAGAIN;}
void rb_kernel_retire(struct rb_kernel *k,uintptr_t l){(void)k;(void)l;}
/* Persistent machine identity is a storage boundary, not an account reply. */
FILE *runtime_fopen(const char *path,const char *mode)
{
    assert(!strcmp(path,"/System/Security/machine.sid")&&!strcmp(mode,"rb"));
    static uint8_t sid[64];memset(sid,0,64);memcpy(sid,"CIUKISID",8);
    for(unsigned i=0;i<3;i++)rb_put32(sid+8+4*i,accounts.db.sid[i]);
    rb_put64(sid+24,1234);sha256(sid,32,sid+32);return fmemopen(sid,64,"rb");
}
int runtime_mkdir(const char *p,mode_t mode){(void)p;assert(mode==0700);return 0;}
static int read_hive(void *p,const char *name,uint8_t **out,size_t *n){(void)p;(void)name;(void)out;(void)n;return 0;}
static int write_hive(void *p,const char *name,const void *data,size_t n){(void)p;(void)name;(void)data;(void)n;return 0;}
static int patch_hive(void *p,const char *name,size_t at,const void *data,size_t n){(void)at;return write_hive(p,name,data,n);}
static int sync_hive(void *p,const char *name){(void)p;(void)name;return 0;}
void wr_posix_io(struct wr_io *io,const char *directory){(void)directory;*io=(struct wr_io){NULL,read_hive,write_hive,patch_hive,sync_hive,NULL};}
static int role(void *p){(void)p;return 0;}
static int retain(void *p,uintptr_t l){(void)p;assert(l==1);return 0;}
static void release(void *p,uintptr_t l){(void)p;assert(l==1);}
static int inspect(void *p,uintptr_t l,struct rb_identity *c)
{
    (void)p;assert(l==1);unsigned saved=caller;uint32_t uid=storage.peers[caller].subject.uid;
    caller=4;int enabled=0,e=rb_account_current(NULL,uid,c,&enabled);caller=saved;
    if(e)return e;assert(enabled);c->session=1;c->process_generation=1;return 0;
}
static int read_root(void *p,unsigned root,char **out,size_t *n){(void)p;(void)root;(void)out;(void)n;return -ENOENT;}
static int write_root(void *p,unsigned root,const char *v,size_t n,uint64_t g){(void)p;(void)root;(void)v;(void)n;(void)g;return -EACCES;}
static void registry_init(void)
{
    struct wr_io io;wr_posix_io(&io,NULL);assert(wr_init_secure(&hive,&io,0));
    memset(&registry,0,sizeof(registry));for(unsigned i=0;i<RB_LINK_MAX;i++)registry.clients[i].fd=-1;
    struct rb_authority a={NULL,role,retain,release,inspect};struct rb_storage s={.read=read_root,.write=write_root};assert(!rb_init(&registry.broker,&a,&s,&hive));
}
/* A session registry request hitting the USER endpoint must fail. Registry
 * routing uses the production desk_w32_exchange, not this boundary. */
uint32_t desk_w32_record(void *p,uint32_t n){(void)p;(void)n;assert(!"registry request escaped to session w32srv");return 1062;}
/* Unused kernel/UI boundaries retained by ASan's callback-table metadata. */
int ciuki_proc_list(void *p,uint32_t n){(void)p;(void)n;abort();}
int ciuki_channel_pair(int p[2]){(void)p;abort();}
ciuki_pid_t ciuki_spawn(const char *p,char *const a[],char *const e[],const struct ciuki_spawn_fd *f,uint32_t n,uint32_t flags)
{(void)p;(void)a;(void)e;(void)f;(void)n;(void)flags;abort();}
void desk_shell_message(struct desktop *d,const char *t,const char *m,int e){(void)d;(void)t;(void)m;(void)e;abort();}
int acct_sys_close(int fd){return runtime_close(fd);}
int __ciuki_account_rpc(uint32_t,const void *,uint32_t,void *,uint32_t,uint32_t *);
static void selector(uint8_t body[312],uint32_t uid,uint64_t generation)
{memset(body,0,312);rb_put32(body,1);rb_put32(body+36,uid);rb_put64(body+40,generation);}
static int client_security(void *p,struct ciuki_security_args *a,const void *in,uint32_t ni,void *out,uint32_t no)
{
    (void)p;assert(ni==64&&no==64);struct ciuki_security_args copy=*a;
    copy.spawn_address=runtime_pointer(in);copy.output_address=runtime_pointer(out);
    return ciuki_security(&copy)?-errno:0;
}
static int client_send(void *p,int fd,const struct ciuki_message *m){(void)p;return ciuki_channel_send(fd,m,1)<0?-errno:0;}
static int client_recv(void *p,int fd,struct ciuki_message *m){(void)p;int n=ciuki_channel_recv(fd,m,1);return n<0?-errno:n;}
static int client_wait(void *p,int fd,unsigned ms){(void)p;(void)fd;(void)ms;return 0;}
static int client_close(void *p,int fd){(void)p;return runtime_close(fd);}
static uint64_t client_now(void *p){(void)p;return storage.now;}
static int panel_call(void *p,uint32_t op,uint64_t generation,const void *body,size_t n,void *out,size_t cap,size_t *used,int *status)
{
    (void)p;static const struct lgn_client_sys sys={NULL,client_security,client_send,client_recv,client_wait,client_close,client_now};
    struct lgn_client link;lgn_client_init(&link,&sys,CIUKI_SECURITY_SERVICE_ACCOUNTS,CIUKI_SECURITY_ROLE_ACCOUNT);
    /* The same production client encoder used by accounts_call in main.c. */
    int e=lgn_client_call(&link,op,generation,body,n,out,cap,used,status,15000);lgn_client_close(&link);
    if(lose_reply&&op>=2&&op<=5&&!e&&!*status){lose_reply=0;return -EIO;}
    return e;
}
static int self(void *p,uint32_t *uid,uint32_t groups[8],unsigned *count)
{(void)p;struct acct_subject *s=&storage.peers[caller].subject;*uid=s->uid;*count=s->group_count;memcpy(groups,s->groups,sizeof(s->groups));return 0;}
static struct cpa_view panel;
static void panel_manage(unsigned action,uint32_t uid,const char *name,const char *old,const char *password,int admin,int enable)
{
    struct cpa_transport t={NULL,panel_call,self,NULL};struct cpa_request q={.action=action,.uid=uid,.admin=admin,.enable=enable};
    snprintf(q.name,sizeof(q.name),"%s",name);snprintf(q.old_password,sizeof(q.old_password),"%s",old);
    snprintf(q.password,sizeof(q.password),"%s",password);snprintf(q.confirm,sizeof(q.confirm),"%s",password);
    if(action==CPA_PICTURE)strcpy(q.picture,"catalogue/ciuki.png");
    int e=cpa_execute(&t,&q,&panel);if(e)fprintf(stderr,"panel action=%u uid=%u error=%d\n",action,uid,e);assert(!e);
}
static void panel_setup(void)
{
    caller=1;
    panel_manage(CPA_PASSWORD,1000,"","","password1",0,0);
    panel_manage(CPA_CREATE,0,"Standard","","password1",0,1);
    panel_manage(CPA_PASSWORD,501,"","","password1",0,0);
    panel_manage(CPA_ENABLE,501,"","","",0,1);
    assert(cpa_find(&panel,1001));
}
static void panel_matrix(void)
{
    caller=1;panel_manage(CPA_CREATE,0,"Temporary","","password1",1,1);
    const struct cpa_record *r=NULL;
    for(unsigned i=0;i<panel.count;i++)if(!strcmp(panel.records[i].name,"Temporary"))r=panel.records+i;
    assert(r);uint32_t uid=r->uid;
    panel_manage(CPA_RENAME,uid,"Renamed","","",0,0);
    panel_manage(CPA_TYPE,uid,"","","",0,0);
    panel_manage(CPA_ENABLE,uid,"","","",0,0);
    panel_manage(CPA_PICTURE,uid,"","","",0,0);
    lose_reply=1;panel_manage(CPA_PASSWORD,uid,"","","password2",0,0);assert(!lose_reply); /* STATUS */
    panel_manage(CPA_DELETE,uid,"","","",0,0);
    for(unsigned i=2;i<=3;i++){
        caller=i;panel_manage(CPA_PASSWORD,i==2?1001:501,"","password1","password2",0,0);
        struct cpa_transport t={NULL,panel_call,self,NULL};struct cpa_request q={.action=CPA_CREATE,.enable=1};
        strcpy(q.name,"Forbidden");strcpy(q.password,"password1");strcpy(q.confirm,"password1");
        assert(cpa_execute(&t,&q,&panel)==-EPERM);
    }
    puts("runtime Control Panel: all management kinds, RESERVE/STATUS, role refusals PASS");
}
static void account_matrix(void)
{
    const unsigned peers[]={1,2,3};const uint32_t uids[]={1000,1001,501};
    for(unsigned i=0;i<3;i++){
        caller=peers[i];
        for(unsigned current=0;current<2;current++){
            uint8_t body[312],reply[272];size_t used=0;uint32_t native_used=0;
            selector(body,uids[i],current?accounts.db.generation:0);
            struct reg_remote link={.fd=-1,.service=2};
            assert(!reg_registry_rpc(&link,1,body,sizeof(body),reply,sizeof(reply),&used)&&used==160);
            assert(!reg_registry_rpc(&link,8,body,sizeof(body),reply,sizeof(reply),&used));
            assert(used==272&&rb_get32(reply+8)==uids[i]&&rb_get64(reply+16)==accounts.db.generation);
            reg_remote_close(&link);
            assert(!__ciuki_account_rpc(8,body,sizeof(body),reply,sizeof(reply),&native_used)&&native_used==272);
        }
        struct passwd *pw=getpwuid(uids[i]);assert(pw&&pw->pw_uid==uids[i]&&strstr(pw->pw_dir,"/Documents"));
        uint8_t body[312],reply[272];uint32_t used=0;selector(body,uids[i],accounts.db.generation+1);
        assert(__ciuki_account_rpc(8,body,sizeof(body),reply,sizeof(reply),&used)==EAGAIN);
        selector(body,1000,0);
        int e=__ciuki_account_rpc(8,body,sizeof(body),reply,sizeof(reply),&used);
        assert(e==(i?EACCES:0));
        struct cpa_transport t={NULL,panel_call,self,NULL};struct cpa_request q={.action=CPA_READ};struct cpa_view v={0};
        assert(!cpa_execute(&t,&q,&v)&&v.valid&&v.self_uid==uids[i]);
        printf("runtime ACCOUNTS role=%u generation=0/current/stale PROFILE/libc/panel PASS\n",i);
    }
}
/* Seed fixture data through the production registry handler. This encoder
 * prepares disk contents only; assertions below call the production desktop. */
static uint32_t seed_request(unsigned op,uint32_t key,const char *path,const char *name,const char *text)
{
    uint8_t in[2048]={0},out[W32R_MAX];size_t used=0,p=strlen(path)+1,n=strlen(name)+1,d=text?2*(strlen(text)+1):0;
    struct w32r_request q={.version=1,.op=op,.key=key,.access=0xf003f,.path_bytes=(uint32_t)p,.name_bytes=(uint32_t)n,
        .type=text?1:0,.data_bytes=(uint32_t)d};
    if(op==W32R_CLOSE||op==W32R_SET)q.access=0;
    memcpy(in,&q,sizeof(q));memcpy(in+sizeof(q),path,p);memcpy(in+sizeof(q)+p,name,n);
    if(text)for(size_t i=0;i<strlen(text);i++)in[sizeof(q)+p+n+2*i]=(uint8_t)text[i];
    assert(!wr_dispatch(&hive,99,in,sizeof(q)+p+n+d,out,sizeof(out),&used));
    struct w32r_reply r;memcpy(&r,out,sizeof(r));assert(!r.error);return r.key;
}
static void seed_string(uint32_t root,const char *path,const char *name,const char *text)
{
    uint32_t uid=hive.subject.uid;hive.subject.uid=0;
    uint32_t key=seed_request(W32R_CREATE,root,path,"",NULL);
    seed_request(W32R_SET,key,"",name,text);seed_request(W32R_CLOSE,key,"","",NULL);hive.subject.uid=uid;
}
static void desktop_matrix(unsigned peer)
{
    static const char base[]="Software\\Microsoft\\Windows\\CurrentVersion\\";
    char path[160];
    for(unsigned h=0;h<2;h++)for(unsigned once=0;once<2;once++){
        snprintf(path,sizeof(path),"%s%s",base,once?"RunOnce":"Run");
        seed_string(h?W32R_HKCU:W32R_HKLM,path,"Example","C:\\WINDOWS\\CDPLAYER.EXE");
    }
    struct desk_startup_entry entries[8];int n=ciuki_win32_startup_list(entries,8);assert(n==4);
    unsigned once_count=0;
    for(int i=0;i<n;i++){
        assert(!entries[i].error&&!strcmp(entries[i].launch.target,"C:\\WINDOWS\\CDPLAYER.EXE"));
        if(entries[i].source==2||entries[i].source==3){
            int e=ciuki_win32_startup_consume(entries+i);
            assert(e==(entries[i].source==2&&peer!=1?-EACCES:0));once_count++;
        }
    }
    assert(once_count==2&&ciuki_win32_startup_list(entries,8)==(peer==1?2:3));
    snprintf(path,sizeof(path),"%sUninstall\\Example",base);
    seed_string(W32R_HKLM,path,"DisplayName","Example application");
    seed_string(W32R_HKLM,path,"UninstallString","C:\\WINDOWS\\CDPLAYER.EXE");
    struct desk_uninstall_entry out[2];assert(ciuki_win32_uninstall_list(out,2)==1&&!strcmp(out[0].name,"Example application"));
    assert(ciuki_win32_uninstall_registered("Example")==1&&ciuki_win32_uninstall_registered("Missing")==0);
    puts("runtime desktop: HKLM/HKCU Run/RunOnce snapshot, consume rights, uninstall inventory PASS");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    assert(ACCT_ROLE_REGISTRY==CIUKI_SECURITY_ROLE_REGISTRY);
    fake_init(&storage);fake_boot(&storage,&accounts);accounts.store.kernel.deliver_reply=deliver;
    /* Boundary identities and committed profile mappings; service policy and
     * every protocol decoder/handler remain the production implementations. */
    storage.peers[4].subject.uid=0;storage.peers[4].subject.role=ACCT_ROLE_REGISTRY;storage.peers[4].subject.groups[0]=0;
    storage.peers[4].subject.session_id=0;
    for(unsigned i=0;i<FAKE_PEERS;i++)storage.peers[i].subject.service_generation=1;
    /* Only the real SYSTEM/session-0 REGISTRY role may read foreign profiles. */
    caller=4;uint8_t body[312],reply[272];uint32_t used=0;selector(body,1000,0);
    assert(!__ciuki_account_rpc(8,body,sizeof(body),reply,sizeof(reply),&used)&&used==272);
    storage.peers[4].subject.role=0;
    assert(__ciuki_account_rpc(8,body,sizeof(body),reply,sizeof(reply),&used)==EACCES);
    storage.peers[4].subject.role=ACCT_ROLE_REGISTRY;storage.peers[4].subject.session_id=1;
    assert(__ciuki_account_rpc(8,body,sizeof(body),reply,sizeof(reply),&used)==EACCES);
    storage.peers[4].subject.session_id=0;storage.peers[4].subject.uid=1001;
    assert(__ciuki_account_rpc(8,body,sizeof(body),reply,sizeof(reply),&used)==EACCES);
    storage.peers[4].subject.uid=0;caller=1;
    panel_setup();
    account_matrix();panel_matrix();
    assert(argc==2);unsigned p=(unsigned)atoi(argv[1]);assert(p>=1&&p<=3);
    {
        caller=p;struct wr_server user;struct wr_io io;char directory[160];
        assert(wr_user_start(&user,&io,directory,0)&&user.session_only);wr_destroy(&user);
        registry_init();assert(!desk_w32_ready());desktop_matrix(p);assert(!desk_w32_flush());
        /* The native desktop keeps its broker link. Release it through the
         * kernel seam at the end of this isolated caller instance. */
        runtime_close(500+FAKE_PEERS);wr_destroy(&hive);
        printf("runtime desktop/w32srv role=%u broker readiness/flush PASS\n",p);
    }
    acct_service_close(&accounts);fake_destroy(&storage);puts("sec-runtime1 real peers: PASS");return 0;
}
