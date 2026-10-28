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
    rb_server_destroy(s);free(s);g_current=&controller;proc_discard(client);proc_discard(service);proc_collect();
    CHECK(!desktop_objects.channels&&!desktop_objects.descriptions&&!desktop_objects.messages);
    CHECK(!pages_used&&!g_user_mappings&&heap_blocks==1);kfree(supervisor_task);supervisor_task=NULL;CHECK(!heap_blocks);free(ram);
    puts("H07 production adapter: real role/connect/accept/QUERY_PEER generations, CSC1 server, revocation, zero fd/subject/page leak PASS");return 0;
}
