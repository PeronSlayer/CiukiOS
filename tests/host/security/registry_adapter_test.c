/* Real call-101 endpoints and production REGISTRY server; only account and
 * disk metadata are hardware/storage boundary fakes. SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_HOST_MAIN registry_unused_desktop_main
#include "../proc/desktop_test.c"
#include "../../../apps/registry/server.h"
#define U_ARGS (CIUKI_IMAGE_BASE+PAGE_SIZE+0x100)
#define U_OUT (U_ARGS+0x100)
#define U_IN (U_ARGS+0x200)
#define U_MSG (U_ARGS+0x300)
static struct proc_thread *acting;
static struct process *current(void){desktop_select(acting);return acting->process;}
int acct_sys_security(struct ciuki_security_args *a,const void *in,uint32_t in_bytes,void *out,uint32_t out_bytes)
{
    struct process *p=current();struct ciuki_security_args copy=*a;
    if(in){CHECK(!ua_write(p->memory,U_IN,in,in_bytes));copy.spawn_address=U_IN;}
    if(out){copy.output_address=U_OUT;copy.output_bytes=out_bytes;}
    CHECK(!ua_write(p->memory,U_ARGS,&copy,64));struct trap_frame tf={.eax=101,.ebx=U_ARGS,.ecx=64};int e=proc_syscall(&tf);if(e)return e;
    CHECK(!ua_read(p->memory,&copy,U_ARGS,64));copy.spawn_address=a->spawn_address;copy.output_address=a->output_address;copy.output_bytes=a->output_bytes;*a=copy;
    if(out)CHECK(!ua_read(p->memory,out,U_OUT,out_bytes));return 0;
}
int acct_sys_channel_send(int fd,const struct ciuki_message *m){struct process *p=current();CHECK(!ua_write(p->memory,U_MSG,m,sizeof(*m)));return channel_send(p,fd,U_MSG,DONTWAIT);}
int acct_sys_channel_recv(int fd,struct ciuki_message *m,int wait){(void)wait;struct process *p=current();int r=channel_recv(p,fd,U_MSG,DONTWAIT);if(r==1)CHECK(!ua_read(p->memory,m,U_MSG,sizeof(*m)));return r;}
int acct_sys_close(int fd){return desktop_close(current(),fd);}
int acct_sys_wait_readable(const int *fds,unsigned n,unsigned ms){(void)fds;g_ticks+=ms;return (int)n;}
uint64_t acct_sys_monotonic_ms(void){return g_ticks;}
uint32_t acct_sys_pid(void){return acting->process->pid;}
static struct proc_thread *thread_of(struct process *p){for(unsigned i=0;i<CIUKI_THREAD_MAX;i++){struct proc_thread *t=proc_thread_slot(i);if(t&&t->process==p)return t;}CHECK(false);return NULL;}
static struct proc_thread *registry_role_fixture(unsigned role,struct process **out)
{
    CHECK(!proc_prepare(proc_supervisor(),out));CHECK(!security_role_prepare(proc_supervisor(),*out,role,security_role_image(role)));
    struct ciuki_file f=fixture_file();struct proc_strings *s=proc_strings_new();CHECK(s&&!proc_strings_add(s,"role",5,false));
    CHECK(!prepare_image(*out,&f,s,0));proc_strings_free(s);proc_publish(*out,0);return thread_of(*out);
}
static int account(void *p,uint32_t uid,struct rb_identity *out,int *enabled){(void)p;*out=(struct rb_identity){.uid=uid,.machine_sid={1,2,3},.volume=31,.account_generation=1};*enabled=1;return 0;}
static int read_root(void *p,unsigned root,char **out,size_t *n){(void)p;CHECK(root==1);static const char text[]="; ciuki-registry generation=1\n[System]\n";*n=sizeof(text)-1;*out=malloc(*n);CHECK(*out);memcpy(*out,text,*n);return 0;}
static int write_root(void *p,unsigned root,const char *v,size_t n,uint64_t g){(void)p;(void)root;(void)v;(void)n;(void)g;CHECK(false);return -EIO;}
struct rt;struct rt *reg_hardware(uint64_t previous){(void)previous;return NULL;}
#ifdef CIUKI_POLICY_COVERAGE
/* ---- sec-k08 H13 (tests/host/security/test_policy_coverage.py): the
 * call-101 channels and the libreg/registry broker frontend. A Standard
 * user (uid 1007) and Guest, each in a session, try the SYSTEM-only
 * services (AUTH, OWNER, LOGON), unknown services and operations (kernel
 * max_op), and every registry broker mutation of the machine and Users
 * roots plus unknown broker operations; every attempt is refused and four
 * repetitions leave no fd, channel, description, message, page, heap block
 * or subject reference behind. No write reaches storage (write_root fails
 * the test). ---- */
static unsigned h13_cases;
struct h13_state { unsigned pages,heap,fds,channels,descriptions,messages,refs; };
static void h13_snapshot(struct process *p,struct h13_state *s)
{
    unsigned fds=0;for(int i=0;i<CIUKI_OPEN_MAX;i++)fds+=!!p->fds[i].object;
    *s=(struct h13_state){pages_used,heap_blocks,fds,desktop_objects.channels,desktop_objects.descriptions,desktop_objects.messages,p->subject->refs};
}
static void h13_same(const char *what,struct process *p,const struct h13_state *b)
{
    struct h13_state e;h13_snapshot(p,&e);
    if(memcmp(b,&e,sizeof(e)))printf("H13 leak in %s: pages %u/%u heap %u/%u fds %u/%u channels %u/%u descriptions %u/%u messages %u/%u refs %u/%u\n",what,
        b->pages,e.pages,b->heap,e.heap,b->fds,e.fds,b->channels,e.channels,b->descriptions,e.descriptions,b->messages,e.messages,b->refs,e.refs);
    CHECK(!memcmp(b,&e,sizeof(e)));
}
static int h13_connect(uint32_t service,struct ciuki_security_link *out)
{
    struct ciuki_security_link in={.version=1,.size=64,.service=service};
    struct ciuki_security_args a={.version=1,.size=64,.operation=CIUKI_SECURITY_SERVICE_CONNECT};
    return acct_sys_security(&a,&in,64,out,64);
}
static uint64_t h13_id=100;
static int h13_send(int fd,uint32_t service,uint32_t op,uint32_t generation,const uint8_t *body,uint32_t n)
{
    struct ciuki_message m={.length=64+n};uint8_t *h=m.data;
    rb_put32(h,0x31435343);rb_put32(h+4,1);rb_put32(h+8,64);rb_put32(h+12,service);rb_put32(h+16,op);rb_put32(h+20,1);
    rb_put64(h+24,++h13_id);rb_put32(h+32,generation);rb_put32(h+36,n);rb_put32(h+52,n);if(n)memcpy(h+64,body,n);
    if(service==4&&op<RB_WIN32&&n>=32)rb_put64(h+40,rb_get64(body+16));   /* the broker's expected generation */
    return acct_sys_channel_send(fd,&m);
}
/* One broker exchange; the reply's status word (negative errno or 0). */
static int32_t h13_broker(struct rb_server *s,struct proc_thread *server,struct proc_thread *client,
                          const struct ciuki_security_link *l,uint32_t op,const uint8_t *body,uint32_t n)
{
    acting=client;int e=h13_send(l->endpoint_fd,4,op,l->endpoint_generation,body,n);if(e)return e;
    acting=server;rb_server_step(s,0);acting=client;
    struct ciuki_message m;CHECK(acct_sys_channel_recv(l->endpoint_fd,&m,0)==1);
    CHECK(m.length>=64&&rb_get32(m.data+20)==2);return (int32_t)rb_get32(m.data+48);
}
static void h13_channels(struct rb_server *s,struct proc_thread *server,struct process *account_server)
{
    (void)account_server;
    struct process *users[2];struct proc_thread *threads[2];
    static const uint32_t uid[2]={1007,501},gid[2]={545,546};
    for(unsigned w=0;w<2;w++){
        threads[w]=make_process(proc_supervisor(),&users[w]);struct ciuki_credential *c=&users[w]->subject->credential;
        c->uid=uid[w];c->primary_gid=gid[w];c->group_count=1;c->groups[0]=gid[w];c->session_id=200+w;c->flags=CIUKI_CRED_INTERACTIVE;
    }
    for(unsigned w=0;w<2;w++){
        struct process *p=users[w];struct proc_thread *t=threads[w];acting=t;struct ciuki_security_link out;
        /* SYSTEM-only services and unknown services: refused at CONNECT. */
        static const struct {uint32_t service;int e;} refused[]={{CIUKI_SECURITY_SERVICE_AUTH,-EPERM},
            {CIUKI_SECURITY_SERVICE_OWNER,-EPERM},{CIUKI_SECURITY_SERVICE_LOGON,-EPERM},{0,-EINVAL},{7,-EINVAL},{0xffffffffu,-EINVAL}};
        for(unsigned i=0;i<sizeof(refused)/sizeof(*refused);i++){
            CHECK(h13_connect(refused[i].service,&out)==refused[i].e);
            struct h13_state b;h13_snapshot(p,&b);
            for(unsigned k=0;k<3;k++)CHECK(h13_connect(refused[i].service,&out)==refused[i].e);
            h13_same("service connect",p,&b);h13_cases++;
        }
        /* W32SRV: only the caller's own session's server (none here). */
        CHECK(h13_connect(CIUKI_SECURITY_SERVICE_W32SRV,&out)==-ESRCH);h13_cases++;
        /* ACCOUNTS: anyone connects; operations outside 1..9 never reach the service. */
        struct ciuki_security_link accounts;CHECK(!h13_connect(CIUKI_SECURITY_SERVICE_ACCOUNTS,&accounts));
        uint8_t body[32]={0};
        static const uint32_t bad_ops[]={0,10,11,0x7fffffffu,0xffffffffu};
        for(unsigned i=0;i<sizeof(bad_ops)/sizeof(*bad_ops);i++){
            CHECK(h13_send(accounts.endpoint_fd,2,bad_ops[i],accounts.endpoint_generation,body,32)==-EOPNOTSUPP);
            struct h13_state b;h13_snapshot(p,&b);
            for(unsigned k=0;k<3;k++)CHECK(h13_send(accounts.endpoint_fd,2,bad_ops[i],accounts.endpoint_generation,body,32)==-EOPNOTSUPP);
            h13_same("ACCOUNTS operation",p,&b);h13_cases++;
        }
        /* A frame naming another service on this link is malformed. */
        CHECK(h13_send(accounts.endpoint_fd,1,1,accounts.endpoint_generation,body,32)==-EINVAL);h13_cases++;
        acting=t;CHECK(!acct_sys_close(accounts.endpoint_fd));
        /* REGISTRY: the broker refuses every machine/Users mutation. */
        struct ciuki_security_link reg;acting=t;CHECK(!h13_connect(CIUKI_SECURITY_SERVICE_REGISTRY,&reg));
        acting=server;rb_server_step(s,0);
        static const uint32_t roots[]={RB_SYSTEM,RB_PROGRAMS,RB_USERS};
        for(unsigned r=0;r<3;r++){
            uint8_t q[32]={0};rb_put32(q,1);rb_put32(q+4,roots[r]);rb_put32(q+12,32);rb_put64(q+16,1);
            CHECK(h13_broker(s,server,t,&reg,RB_NATIVE_BEGIN,q,32)==-EACCES);
            struct h13_state b;h13_snapshot(p,&b);
            for(unsigned k=0;k<3;k++)CHECK(h13_broker(s,server,t,&reg,RB_NATIVE_BEGIN,q,32)==-EACCES);
            h13_same("registry BEGIN",p,&b);h13_cases++;
            uint8_t part[64]={0};memcpy(part,q,32);rb_put32(part+12,32);
            CHECK(h13_broker(s,server,t,&reg,RB_NATIVE_PART,part,64)==-EACCES);h13_cases++;
            rb_put32(q+12,32);CHECK(h13_broker(s,server,t,&reg,RB_NATIVE_COMMIT,q,32)==-EACCES);h13_cases++;
        }
        /* Unknown broker operations (the kernel passes REGISTRY frames through). */
        static const uint32_t unknown[]={8,9,15,21,31,38,64,0xffffu,0x7fffffffu};
        for(unsigned i=0;i<sizeof(unknown)/sizeof(*unknown);i++){
            uint8_t q[32]={0};rb_put32(q,1);rb_put32(q+4,RB_SYSTEM);
            int32_t e=h13_broker(s,server,t,&reg,unknown[i],q,32);
            if(e!=-EOPNOTSUPP&&e!=-EINVAL)printf("H13 broker op %u: %d\n",unknown[i],e);
            CHECK(e==-EOPNOTSUPP||e==-EINVAL);h13_cases++;
        }
        acting=t;CHECK(h13_send(reg.endpoint_fd,4,0,reg.endpoint_generation,body,32)==-EOPNOTSUPP);h13_cases++;
        acting=t;CHECK(!acct_sys_close(reg.endpoint_fd));acting=server;rb_server_step(s,0);
    }
    for(unsigned w=0;w<2;w++){g_current=&controller;stop_collect(users[w]);}
    printf("H13 channels: %u refusals (Standard user and Guest: AUTH/OWNER/LOGON connect, unknown services, ACCOUNTS operations outside max_op, cross-service frames, registry broker machine/Users mutations and unknown broker operations) without fd/channel/description/message/page/heap/subject leaks PASS\n",h13_cases);
}
/* ---- end sec-k08 ---- */
#endif
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);controller.state=T_RUNNING;g_current=&controller;proc_init();
    struct process *service,*client;struct proc_thread *server_thread=registry_role_fixture(CIUKI_SECURITY_ROLE_REGISTRY,&service);
    struct proc_thread *client_thread=make_process(proc_supervisor(),&client);acting=server_thread;
    struct rb_server *s=calloc(1,sizeof(*s));CHECK(s);struct rb_storage store={.read=read_root,.write=write_root};
    CHECK(!rb_server_init(s,&store,NULL,account,NULL));
    acting=client_thread;struct ciuki_security_link in={.version=1,.size=64,.service=4},out;
    struct ciuki_security_args a={.version=1,.size=64,.operation=CIUKI_SECURITY_SERVICE_CONNECT};
    CHECK(!acct_sys_security(&a,&in,64,&out,64));
    acting=server_thread;rb_server_step(s,0);unsigned occupied=0;
    for(unsigned i=0;i<RB_LINK_MAX;i++)occupied+=s->clients[i].lease!=0;CHECK(occupied==1);
    acting=client_thread;struct ciuki_message m={.length=96};uint8_t *h=m.data;
    rb_put32(h,0x31435343);rb_put32(h+4,1);rb_put32(h+8,64);rb_put32(h+12,4);rb_put32(h+16,RB_NATIVE_READ);rb_put32(h+20,1);
    rb_put64(h+24,1);rb_put32(h+32,out.endpoint_generation);rb_put32(h+36,32);rb_put32(h+52,32);
    rb_put32(h+64,1);rb_put32(h+68,1);CHECK(!acct_sys_channel_send(out.endpoint_fd,&m));
    acting=server_thread;rb_server_step(s,0);acting=client_thread;CHECK(acct_sys_channel_recv(out.endpoint_fd,&m,0)==1);
    CHECK(m.length==96&&rb_get32(m.data+20)==2&&!rb_get32(m.data+48)&&rb_get64(m.data+80)==1);
    uintptr_t retained=0;for(unsigned i=0;i<RB_LINK_MAX;i++)if(s->clients[i].lease)retained=s->clients[i].lease;
    acting=server_thread;struct rb_identity identity;CHECK(!s->broker.authority.inspect(&s->kernel,retained,&identity)&&!identity.uid&&identity.volume==31&&identity.machine_sid[2]==3);
    acting=client_thread;CHECK(!acct_sys_close(out.endpoint_fd));acting=server_thread;
    CHECK(rb_kernel_live(&s->kernel,retained)==-ESRCH);rb_server_step(s,0);
    for(unsigned i=0;i<RB_LINK_MAX;i++)CHECK(!s->kernel.links[i].refs&&!s->broker.links[i].retained);
    /* A complete BEGIN does not let an abandoned staged root keep a lease
     * forever: the production server expires the stream between RPCs. */
    acting=client_thread;memset(&a,0,sizeof(a));a.version=1;a.size=64;a.operation=CIUKI_SECURITY_SERVICE_CONNECT;
    CHECK(!acct_sys_security(&a,&in,64,&out,64));acting=server_thread;rb_server_step(s,0);
    acting=client_thread;memset(&m,0,sizeof(m));m.length=96;h=m.data;
    rb_put32(h,0x31435343);rb_put32(h+4,1);rb_put32(h+8,64);rb_put32(h+12,4);rb_put32(h+16,RB_NATIVE_BEGIN);rb_put32(h+20,1);
    rb_put64(h+24,1);rb_put32(h+32,out.endpoint_generation);rb_put32(h+36,32);rb_put64(h+40,1);rb_put32(h+52,32);
    rb_put32(h+64,1);rb_put32(h+68,1);rb_put32(h+76,32);rb_put64(h+80,1);
    CHECK(!acct_sys_channel_send(out.endpoint_fd,&m));acting=server_thread;rb_server_step(s,0);
    acting=client_thread;CHECK(acct_sys_channel_recv(out.endpoint_fd,&m,0)==1&&!rb_get32(m.data+48));
    acting=server_thread;g_ticks+=5001;rb_server_step(s,0);
    for(unsigned i=0;i<RB_LINK_MAX;i++)CHECK(!s->kernel.links[i].refs&&!s->broker.links[i].stage);
#ifdef CIUKI_POLICY_COVERAGE
    {struct process *accounts_role;struct proc_thread *accounts_thread=registry_role_fixture(CIUKI_SECURITY_ROLE_ACCOUNT,&accounts_role);
     (void)accounts_thread;h13_channels(s,server_thread,accounts_role);g_current=&controller;proc_discard(accounts_role);proc_collect();} /* sec-k08 */
#endif
    rb_server_destroy(s);free(s);g_current=&controller;proc_discard(client);proc_discard(service);proc_collect();
    CHECK(!desktop_objects.channels&&!desktop_objects.descriptions&&!desktop_objects.messages);
    CHECK(!pages_used&&!g_user_mappings&&heap_blocks==1);kfree(supervisor_task);supervisor_task=NULL;CHECK(!heap_blocks);free(ram);
    puts("H07 production adapter: real role/connect/accept/QUERY_PEER generations, CSC1 server, revocation, zero fd/subject/page leak PASS");return 0;
}
