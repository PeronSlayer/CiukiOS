/* Production personality mechanisms on the native process RAM fixture.
 * Only hardware/scheduling/VFS endpoints are mocked.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kheap_limits.h>
#define kmalloc fixture_kmalloc
#define kzalloc fixture_kzalloc
#define kfree fixture_kfree
#define main proc_fixture_main
#define fpu_task_release fixture_fpu_release
#include "../proc/proc_test.c"
#undef main
#undef kmalloc
#undef kzalloc
#undef kfree
static void (*w32_allocation_hook)(void);
void *(kmalloc)(size_t n) { if (n > KHEAP_MAX_REQUEST) return 0;
    void *p=fixture_kmalloc(n);
    if(p&&w32_allocation_hook){void (*hook)(void)=w32_allocation_hook;w32_allocation_hook=0;hook();}
    return p;
}
void *(kzalloc)(size_t n) { if (n > KHEAP_MAX_REQUEST) return 0;void *p=kmalloc(n);if(p)memset(p,0,n);return p;}
void kfree(void *p){fixture_kfree(p);}
uint32_t pmm_alloc_contig(uint32_t n){if(fail_alloc()||!n)return 0;for(unsigned first=1;first+n<=HOST_PAGES;first++){unsigned i=0;while(i<n&&!allocated[first+i])i++;if(i!=n){first+=i;continue;}for(i=0;i<n;i++){allocated[first+i]=true;pages_used++;memset(P2V((first+i)*PAGE_SIZE),0xa5,PAGE_SIZE);}return first*PAGE_SIZE;}return 0;}
#undef fpu_task_release
#define SEL_UCODE 0x1b
#define SEL_UDATA 0x23
#define CR0_MP (1u << 1)
#define CR0_EM (1u << 2)
#define CR0_TS (1u << 3)
#define CR0_NE (1u << 5)
#define CR4_OSFXSR (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)
#include <ciuki/w32_objects.h>
#include <ciuki/files.h>
static uint32_t fake_cr0, fake_cr4;
bool g_cpu_fxsr, g_cpu_sse;
static uint8_t hardware[512] __attribute__((aligned(16)));
static uint32_t read_cr0(void) { return fake_cr0; }
static uint32_t read_cr4(void) { return fake_cr4; }
static void write_cr0(uint32_t v) { fake_cr0 = v; }
static void write_cr4(uint32_t v) { fake_cr4 = v; }
static void clts(void) { fake_cr0 &= ~CR0_TS; }
void fpu_fxsave(void *p) { CHECK(!(fake_cr0 & CR0_TS) && (fake_cr4 & CR4_OSFXSR)); CHECK(!((uintptr_t)p & 15)); memcpy(p, hardware, 512); }
void fpu_fxrstor(const void *p) { CHECK(!(fake_cr0 & CR0_TS) && (fake_cr4 & CR4_OSFXSR)); CHECK(!((uintptr_t)p & 15)); memcpy(hardware, p, 512); }
void fpu_fnsave(void *p) { CHECK(!(fake_cr0 & CR0_TS)); memcpy(p, hardware, 108); memset(hardware, 0, 108); }
void fpu_frstor(const void *p) { CHECK(!(fake_cr0 & CR0_TS)); memcpy(hardware, p, 108); }
void fpu_reset_state(int sse) { CHECK(!sse); memset(hardware, 0, 512); }
/* A console record may sleep on serial backpressure: a test hook runs other
 * tasks' work during the record (Codex review of wt/q3crash, P1). The USER
 * bridge prints its own klog and runs the same hook. */
static void (*on_klog)(const char *fmt);
#ifndef W32_KERNEL_BRIDGE
void klog(const char *fmt, ...) { if (on_klog) on_klog(fmt); }
#endif
void arch_w32_fs_switch(uint32_t base) { (void)base; }
uint64_t arch_w32_counter(void) { return w32_counter_sample(g_ticks, 1193, false, 1190); }
#include "../../../src/kernel/lib/sha256.c"
#include "../../../src/kernel/proc/surface.c"
#include "../../../src/kernel/proc/channel.c"
#include "../../../src/kernel/proc/priority.c"
void task_set_base(struct task *t,enum task_prio p,unsigned level){t->prio=p;t->base_priority=(uint8_t)level;}
uint32_t supervisor_desktop_pid(void){return fake_desktop_pid;}
void grant_release(struct desktop_description *d){(void)d;}
static bool registry_grant=true;
bool grants_display_owner(const struct process *p){return registry_grant && p && p->pid==fake_desktop_pid;}
void proc_signal_pipe(struct proc_thread *t){(void)t;}
void kmutex_init(struct kmutex *m){memset(m,0,sizeof(*m));}
/* sync.c thread_context(): a blocking primitive panics with IF cleared or a
 * current task that is not running (the trap_return hook runs with IF off). */
static void thread_context(void){CHECK((host_flags&0x200)&&g_current&&g_current->state==T_RUNNING);}
void kmutex_lock(struct kmutex *m){thread_context();CHECK(!m->owner);m->owner=g_current;}
void kmutex_unlock(struct kmutex *m){thread_context();CHECK(m->owner==g_current);m->owner=0;}
int file_clock_now(uint32_t clock,const struct process *p,struct ciuki_timespec *out){(void)clock;(void)p;*out=(struct ciuki_timespec){.tv_sec=1700000000+(int64_t)(g_ticks/1000),.tv_nsec=(int32_t)(g_ticks%1000)*1000000};return 0;}
static int32_t promises[MM_OWNERS][2], mapping_count, shared_mapping_count;
void ledger_promise(enum mm_owner owner,int32_t reserved,int32_t committed){promises[owner][0]+=reserved;promises[owner][1]+=committed;}
void ledger_mapping(int32_t maps,int32_t shared){mapping_count+=maps;shared_mapping_count+=shared;}
static struct file_description model_file;
static struct px_node model_node;
static struct px_namespace model_space;
static struct vfs model_vfs;
static uint8_t model_data[131072];
static bool model_io_error;
static void (*model_io_interleave)(void);
static unsigned model_file_refs;
static void model_file_retain(struct proc_object *o){(void)o;model_file_refs++;}
static void model_file_release(struct proc_object *o){(void)o;CHECK(model_file_refs);model_file_refs--;}
struct file_description *file_fd(const struct process *p,int32_t fd){return p&&p->fds&&fd>=0&&fd<CIUKI_OPEN_MAX&&p->fds[fd].object==&model_file.object?&model_file:0;}
int file_truncate_locked(struct file_description *d,uint64_t bytes){if(bytes>sizeof(model_data))return -EFBIG;d->node->entry.size=(uint32_t)bytes;return 0;}
int file_io_locked(struct file_description *d,void *buffer,size_t bytes,bool write,bool positioned,uint64_t off,size_t *done){
    CHECK(d==&model_file&&positioned);*done=0;if(model_io_error)return -EIO;
    if(model_io_interleave)model_io_interleave();
    if(off>sizeof(model_data)||bytes>sizeof(model_data)-off)return -EINVAL;
    int err=w32_file_sync(d->node);if(err)return err;
    if(write){memcpy(model_data+off,buffer,bytes);w32_file_changed(d->node,off,buffer,(uint32_t)bytes);}else memcpy(buffer,model_data+off,bytes);
    *done=bytes;return 0;
}
int file_section_io_locked(struct file_description *d,void *buffer,size_t bytes,bool write,
                           bool positioned,uint64_t off,size_t *done)
{return file_io_locked(d,buffer,bytes,write,positioned,off,done);}
#include "../../../src/kernel/proc/win32/objects.c"
#include "../../../src/kernel/proc/win32/wait.c"
#include "../../../src/kernel/proc/win32/apc.c"
#include "../../../src/kernel/proc/win32/message.c"
/* fx-launch: count the image bytes the section code hashes. */
static uint64_t sha256_bytes_hashed;
static void counted_sha256_update(struct sha256_ctx *ctx,const void *data,size_t bytes)
{ sha256_bytes_hashed+=bytes; sha256_update(ctx,data,bytes); }
#define sha256_update counted_sha256_update
#include "../../../src/kernel/proc/win32/sections.c"
#undef sha256_update
#include "../../../src/kernel/proc/win32/service.c"
#include "../../../src/kernel/proc/sigframe.c"
#include "../../../src/kernel/core/fpu.c"
#include "../../../src/kernel/proc/win32/memory.c"
#include "../../../src/kernel/proc/win32/records.c"
#define w32_log klog
#define ksnprintf snprintf
#include "../../../src/kernel/proc/win32/diagnostics.c"
#undef ksnprintf
#undef w32_log
#include "../../../src/kernel/proc/win32/context.c"
#include "../../../src/kernel/proc/win32/exception.c"
#include "../../../src/kernel/proc/win32/pe.c"
#include "../../../src/kernel/proc/win32/loader.c"
#include "../../../src/kernel/proc/win32/personality.c"
/* Display is a device endpoint here; its production implementation is
 * exercised by display_present_test.c. */
uint32_t w32_display_call(struct proc_thread *t,struct ciuki_w32_display_args *a)
{ (void)t;(void)a;return W32_STATUS_NOT_IMPLEMENTED; }
#include <ciuki/w32_core.h>
/* Cwd conversion is independently exercised by kernel32_files.c. */
uint32_t w32_native_path(struct process *p,const char *n,char *out,char *full){(void)p;(void)n;(void)out;(void)full;return 2;}
int px_chdir(struct process *p,const char *path){(void)p;(void)path;return -ENOENT;}
#include "../../../src/kernel/proc/win32/process.c"
uint32_t w32_file_call(struct proc_thread *t,struct ciuki_w32_file_args *a){(void)t;(void)a;return W32_STATUS_NOT_IMPLEMENTED;}
uint32_t w32_render3d_call(struct proc_thread *t,struct ciuki_w32_render3d_args *a) __attribute__((weak));
uint32_t w32_render3d_call(struct proc_thread *t,struct ciuki_w32_render3d_args *a){(void)t;(void)a;return W32_STATUS_NOT_IMPLEMENTED;}
void task_yield(void){if(on_schedule)schedule();}
#include "../../../src/kernel/proc/win32/system.c"
#include "../../../src/kernel/proc/win32/snapshot.c"
#include "../../../src/kernel/proc/win32/syscall.c"
int px_getcwd_locked(struct px_node *n, char path[CIUKI_PATH_MAX]) { (void)n; strcpy(path, "/"); return 0; }
/* This fixture mocks the cwd endpoint; no volume admission or I/O occurs. */
void vfs_lock(struct vfs *v,uint32_t mask) { CHECK(!mask); fs_lock_take(&v->lock); }
void vfs_unlock(struct vfs *v) { fs_lock_drop(&v->lock); }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
bool kwait_wait_until(struct kwait *q, bool (*p)(void *), void *arg, uint64_t deadline)
{ (void)q; thread_context(); if(p(arg))return true; CHECK(deadline>=g_ticks); schedule();return p(arg); }
void kwait_set_owner(struct kwait *q,struct task *owner) { q->owner=owner; }
bool kwait_wait_ticket_until(struct kwait *q,kwait_cond_fn p,void *arg,uint64_t deadline,uint32_t *ticket)
{ (void)ticket;return kwait_wait_until(q,p,arg,deadline); }
static void vm_init(struct uaddr *u) { CHECK(!ua_init(u,u)); u->win32 = true; }
static struct ciuki_w32_vm request(uint32_t address, uint32_t bytes, uint32_t flags, uint32_t protect)
{ return (struct ciuki_w32_vm){ .address=address,.bytes=bytes,.flags=flags,.protect=protect }; }
static void test_memory(void)
{
    struct uaddr u; vm_init(&u);
    struct ciuki_w32_vm a = request(0x10003,0x1000,W32_MEM_RESERVE,W32_PAGE_READWRITE);
    CHECK(!w32_vm_alloc(&u,&a) && a.address==0x10000 && a.bytes==0x2000 && !u.backing && !u.as.pages);
    a = request(0x10fff,2,W32_MEM_COMMIT,W32_PAGE_EXECUTE_READWRITE);
    CHECK(!w32_vm_alloc(&u,&a) && a.address==0x10000 && a.bytes==0x2000 && u.backing==0);
    CHECK(ua_range(&u,0x10000,8192,PROT_READ|PROT_WRITE|PROT_EXEC));
    uint32_t value=0xfeed,read=0; CHECK(!ua_write(&u,0x10000,&value,4));
    a = request(0x10000,4096,W32_MEM_COMMIT,W32_PAGE_READONLY);
    CHECK(!w32_vm_alloc(&u,&a) && !ua_read(&u,&read,0x10000,4) && read==value && ua_find(&u,0x10000)->w32_protect==W32_PAGE_EXECUTE_READWRITE);
    a = request(0x10fff,2,0,W32_PAGE_NOACCESS);
    CHECK(!w32_vm_protect(&u,&a) && a.old_protect==W32_PAGE_EXECUTE_READWRITE && u.backing==1 && !u.as.pages);
    struct ciuki_w32_memory_info q;
    CHECK(!w32_vm_query(&u,0x10100,&q) && q.base==0x10000 && q.allocation_base==0x10000 && q.region_size==8192 && q.state==W32_MEM_COMMIT && q.protect==W32_PAGE_NOACCESS);
    a = request(0x10000,0,W32_MEM_DECOMMIT,0);
    CHECK(!w32_vm_free(&u,&a) && !u.backing && !u.as.tables);
    CHECK(!w32_vm_free(&u,&a));
    a = request(0x10000,4096,W32_MEM_COMMIT,W32_PAGE_READWRITE);
    CHECK(!w32_vm_alloc(&u,&a) && !ua_read(&u,&read,0x10000,4) && !read);
    a = request(0x10000,4096,W32_MEM_RELEASE,0); CHECK(w32_vm_free(&u,&a)==W32_STATUS_INVALID_PARAMETER);
    a.bytes=0;a.address++;CHECK(w32_vm_free(&u,&a)==W32_STATUS_CONFLICTING_ADDRESSES);
    a.address--;CHECK(!w32_vm_free(&u,&a) && !u.head && !u.backing);
    a=request(0,4096,W32_MEM_RESERVE|W32_MEM_COMMIT|W32_MEM_TOP_DOWN,W32_PAGE_READWRITE);
    CHECK(!w32_vm_alloc(&u,&a) && a.address==0x6fff0000);
    a=request(0x7fff0000,4096,W32_MEM_RESERVE,W32_PAGE_READWRITE);CHECK(w32_vm_alloc(&u,&a)==W32_STATUS_INVALID_PARAMETER);
    a=request(0,UINT32_MAX,W32_MEM_RESERVE,W32_PAGE_READWRITE);CHECK(w32_vm_alloc(&u,&a)==W32_STATUS_INVALID_PARAMETER);
    CHECK(ua_brk(&u,0)==-EPERM);struct ciuki_mmap_args n=map_args(4096,3);CHECK(ua_mmap(&u,&n)==-EPERM);
    ua_destroy(&u);
    /* Commit/protect cannot span two adjacent allocations. Pins prohibit all
     * backing/protection/release mutations through both public/internal calls. */
    vm_init(&u);
    for(unsigned i=0;i<2;i++){a=request(0x10000+i*65536,65536,W32_MEM_RESERVE|W32_MEM_COMMIT,W32_PAGE_READWRITE);CHECK(!w32_vm_alloc(&u,&a));}
    a=request(0x1f000,8192,0,W32_PAGE_READONLY);CHECK(w32_vm_protect(&u,&a)==W32_STATUS_CONFLICTING_ADDRESSES);
    CHECK(ua_range(&u,0x1f000,8192,PROT_WRITE));
    struct ua_pin pin;CHECK(!ua_pin(&u,&pin,0x10000,4,true));
    a=request(0x10000,0,W32_MEM_RELEASE,0);CHECK(w32_vm_free(&u,&a)==W32_STATUS_DEVICE_BUSY);ua_unpin(&u,&pin);
    a=request(0x10000,4096,0,W32_PAGE_GUARD|W32_PAGE_READWRITE);CHECK(!w32_vm_protect(&u,&a));
    struct process p={.memory=&u};struct proc_thread t={.process=&p};bool retry;
    CHECK(w32_guard_fault(&t,0x10000,&retry)==W32_EXCEPTION_GUARD && !retry && ua_range(&u,0x10000,1,PROT_WRITE));
    CHECK(w32_guard_fault(&t,0x10000,&retry)==W32_STATUS_ACCESS_VIOLATION);
    ua_destroy(&u);
    /* Allocation-failure sweep: initial reserve, split metadata, tables and
     * backing must all roll back, including partially committed reservations. */
    unsigned failures=0;
    for(int fail=0;fail<40;fail++){
        vm_init(&u);unsigned baseline=pages_used,blocks=heap_blocks;
        a=request(0,20*PAGE_SIZE,W32_MEM_RESERVE|W32_MEM_COMMIT,W32_PAGE_NOACCESS);
        alloc_fail=fail;uint32_t status=w32_vm_alloc(&u,&a);alloc_fail=-1;
        if(status){failures++;CHECK(status==W32_STATUS_NO_MEMORY && !u.head && !u.backing && !u.as.tables && pages_used==baseline && heap_blocks==blocks);}
        ua_destroy(&u);
    }
    CHECK(failures==3); /* area, object and one metadata block; data/PT faults tested separately */
    puts("w32 memory: rounding, W+X, noaccess commitment, repeated commit, queries, pins, zero recommit, guard, OOM rollback PASS");
}
static void test_records(void)
{
    uint32_t record[60]={0};struct ciuki_w32_header *h=(void*)record;
    for(unsigned op=1;op<=11;op++){
        memset(record,0,sizeof(record));*h=(struct ciuki_w32_header){1,w32_record_size(op),op,0};
        if(op>=3&&op<=5){struct ciuki_w32_vm *a=(void*)record;a->protect=op==4?0:4;a->flags=op==3?W32_MEM_RESERVE:op==4?W32_MEM_RELEASE:0;}
        if(op==11)((struct ciuki_w32_diagnostic*)record)->kind=1;
        CHECK(!w32_record_validate(record,h->size,true));
        CHECK(w32_record_validate(record,h->size,false)==-EPERM);
        CHECK(w32_record_validate(record,h->size-1,true)==-EINVAL);
        h->reserved=1;CHECK(w32_record_validate(record,h->size,true)==-EINVAL);h->reserved=0;
        h->version=2;CHECK(w32_record_validate(record,h->size,true)==-EINVAL);h->version=1;
    }
    *h=(struct ciuki_w32_header){1,16,0xffff,0};CHECK(w32_record_validate(record,16,true)==-ENOSYS);
    CHECK(w32_status(-EEXIST)==W32_STATUS_CONFLICTING_ADDRESSES && w32_last_error(w32_status(-EEXIST))==487);
    CHECK(w32_last_error(w32_status(-ENOEXEC))==193 && w32_last_error(w32_status(-EFAULT))==998);
    for(unsigned p=1;p<=128;p<<=1){uint32_t native;CHECK(!w32_protection(p,&native));CHECK(!w32_protection(p|W32_PAGE_NOCACHE,&native));}
    uint32_t native;CHECK(w32_protection(3,&native)==W32_STATUS_INVALID_PARAMETER);CHECK(w32_protection(0x101,&native)==W32_STATUS_INVALID_PARAMETER);CHECK(w32_protection(0x404,&native)==W32_STATUS_INVALID_PARAMETER);
    struct ciuki_w32_identity id={0};w32_identity(&id);CHECK(id.major==4&&id.minor==10&&id.build==2222&&id.platform==1&&(id.get_version>>31)&&!strcmp(id.csd," A ")&&id.directx==0x7000a);
    uint8_t desc[8];arch_w32_fs_encode(desc,0x12345678);CHECK(desc[0]==255&&desc[1]==31&&desc[2]==0x78&&desc[3]==0x56&&desc[4]==0x34&&desc[5]==0xf2&&desc[6]==0x40&&desc[7]==0x12);
    CHECK(w32_counter_sample(9,1193,false,1190)==9*1193+3);
    CHECK(w32_counter_sample(9,1190,true,1189)==10*1193+4);
    CHECK(w32_counter_sample(9,3,false,1191)==10*1193+2);
    CHECK(w32_counter_sample(UINT32_MAX,1193,false,1193)==(uint64_t)UINT32_MAX*1193);
    struct ciuki_w32_display_args display={.header={1,sizeof(display),W32_DISPLAY,0},.action=W32_DISPLAY_GRANT};
    CHECK(w32_record_size(W32_DISPLAY)==80);
    CHECK(!w32_record_validate(&display,sizeof(display),false));
    display.token=1;CHECK(w32_record_validate(&display,sizeof(display),true)==-EINVAL);
    display.token=0;display.action=W32_DISPLAY_FRAME;
    CHECK(!w32_record_validate(&display,sizeof(display),true));
    display.surface.width=1;CHECK(w32_record_validate(&display,sizeof(display),true)==-EINVAL);
    display.surface.width=0;display.count=1;CHECK(w32_record_validate(&display,sizeof(display),true)==-EINVAL);
    display.action=W32_DISPLAY_PALETTE;CHECK(!w32_record_validate(&display,sizeof(display),true));
    display.error=1;CHECK(w32_record_validate(&display,sizeof(display),true)==-EINVAL);
    struct ciuki_w32_system_args feedback={.header={1,sizeof(feedback),W32_SYSTEM,0},.action=W32_S_LAUNCH_FEEDBACK};
    CHECK(!w32_record_validate(&feedback,sizeof(feedback),false));
    CHECK(!w32_record_validate(&feedback,sizeof(feedback),true));
    feedback.high=1;CHECK(w32_record_validate(&feedback,sizeof(feedback),false)==-EINVAL);feedback.high=0;
    feedback.action=W32_S_TIME;CHECK(w32_record_validate(&feedback,sizeof(feedback),false)==-EPERM);
    feedback.action=W32_S_LAUNCH_IDLE;CHECK(!w32_record_validate(&feedback,sizeof(feedback),true));
    CHECK(w32_record_validate(&feedback,sizeof(feedback),false)==-EINVAL);
    puts("w32 records: all sizes, versions, reserved fields, personality, protections, errors, identity, FS encoding, PIT rollover, display transport PASS");
}
static void test_fpu(void)
{
    for(unsigned mode=0;mode<3;mode++){
        g_cpu_fxsr=mode!=0;g_cpu_sse=mode==2;fpu_init();
        uint8_t images[3][512] __attribute__((aligned(16)));
        struct task tasks[3]={{.fpu_area=images[0]},{.fpu_area=images[1],.win32=true},{.fpu_area=images[2]}};
        g_current=&tasks[1];fpu_task_switch(g_current);fpu_handle_nm();
        CHECK(*(uint16_t*)hardware==0x27f);CHECK(!!(fake_cr4&CR4_OSFXSR)==(mode==2));
        memset(hardware+160,0x53,128);
        g_current=&tasks[0];fpu_task_switch(g_current);CHECK(!(fake_cr4&(CR4_OSFXSR|CR4_OSXMMEXCPT)));
        fpu_handle_nm();CHECK(*(uint16_t*)hardware==0x37f);
        if(mode)for(unsigned i=160;i<288;i++)CHECK(tasks[1].fpu_area[i]==0x53);
        /* Native/DPMI-style task -> Win32 with the old owner still resident. */
        g_current=&tasks[1];fpu_task_switch(g_current);fpu_handle_nm();
        if(mode)for(unsigned i=160;i<288;i++)CHECK(hardware[i]==0x53);
        struct ciuki_w32_context c={.flags=W32_CONTEXT_FULL|W32_CONTEXT_FLOAT};fpu_w32_export(g_current,&c);
        CHECK(!fpu_is_owner(g_current)&&fake_cr0&CR0_TS);
        if(mode)for(unsigned i=160;i<288;i++)CHECK(c.extended[i]==0x53);
        fpu_task_release(&tasks[1]);g_current=&tasks[2];fpu_task_switch(g_current);fpu_handle_nm();
        if(mode)for(unsigned i=160;i<288;i++)CHECK(!hardware[i]);
        fpu_task_release(&tasks[2]);
    }
    g_current=&controller;
    puts("w32 FPU: non-FXSR, FXSR-only, SSE; lazy native/Win32/VM owner save, release, initialization and CR4 policy PASS");
}
static void test_context(void)
{
    struct uaddr u;vm_init(&u);struct ciuki_w32_vm a=request(0x400000,4096,W32_MEM_RESERVE|W32_MEM_COMMIT,W32_PAGE_EXECUTE_READWRITE);CHECK(!w32_vm_alloc(&u,&a));
    struct trap_frame tf={.gs=0x23,.fs=0x4b,.es=0x23,.ds=0x23,.cs=0x1b,.user_ss=0x23,.eip=0x400000,.user_esp=0x400800,.eflags=0x202};
    struct ciuki_w32_context good,c;w32_context_capture(&good,&tf);
    CHECK(w32_context_validate(&good,&u,false,0));
    for(unsigned i=0;i<9;i++){
        c=good;uint32_t *field[]={&c.gs,&c.fs,&c.ds,&c.cs,&c.ss,&c.eflags,&c.eip,&c.esp,&c.flags};
        *field[i]=i==5?0x3202:i==6?0xc0000000:i==7?0x7ffe0000:0;CHECK(!w32_context_validate(&c,&u,false,0));
    }
    c=good;c.flags|=W32_CONTEXT_EXTENDED;memset(c.extended,255,512);CHECK(w32_context_validate(&c,&u,true,0xffbf));
    uint32_t mx;memcpy(&mx,c.extended+24,4);CHECK(mx==0xffbf);for(unsigned i=288;i<512;i++)CHECK(!c.extended[i]);
    c.debug[0]=0x400000;CHECK(!w32_context_validate(&c,&u,true,0));
    uint8_t fast[]={0x66,0x0f,0x34};CHECK(w32_sysenter(fast,3));CHECK(!w32_sysenter(fast,2));
    uint8_t cli[]={0x66,0xfa},softint[]={0xcd,0x2e};CHECK(w32_privileged(cli,2));CHECK(!w32_privileged(softint,2));
    CHECK(w32_exception_code(0,false,0,0,0)==W32_EXCEPTION_DIVIDE);
    CHECK(w32_exception_code(4,false,0,0,0)==W32_EXCEPTION_OVERFLOW);
    CHECK(w32_exception_code(5,false,0,0,0)==W32_EXCEPTION_BOUNDS);
    CHECK(w32_exception_code(13,true,0,0,0)==W32_EXCEPTION_PRIVILEGED);
    CHECK(w32_exception_code(16,false,0,4,0)==0xc000008e);
    CHECK(w32_exception_code(16,false,0,0x41,0)==0xc0000092);
    ua_destroy(&u);
    puts("w32 contexts: exact selectors, flags, address permissions, x87/MXCSR sanitization, fault classes PASS");
}
static uint8_t pe_data[0x2400],loader_data[0x2400],dll_data[0x2400];
static int pe_read(void *cookie,uint32_t off,void *dst,uint32_t n){CHECK(off<=sizeof(pe_data)&&n<=sizeof(pe_data)-off);memcpy(dst,(uint8_t*)cookie+off,n);return 0;}
static void put16(uint8_t *p,uint16_t x){memcpy(p,&x,2);}static void put32(uint8_t *p,uint32_t x){memcpy(p,&x,4);}
static void make_pe(uint8_t *b,bool dll){memset(b,0,0x2400);put16(b,0x5a4d);put32(b+60,0x80);put32(b+0x80,0x4550);put16(b+0x84,0x14c);put16(b+0x86,1);put16(b+0x94,224);put16(b+0x96,dll?0x2102:0x102);uint8_t *o=b+0x98;put16(o,0x10b);put32(o+16,0x1000);put32(o+28,dll?0x70010000:0x400000);put32(o+32,4096);put32(o+36,512);put32(o+56,0x3000);put32(o+60,512);put16(o+68,3);put32(o+72,0x100000);put32(o+76,4096);put32(o+92,16);uint8_t *s=o+224;put32(s+8,0x1000);put32(s+12,0x1000);put32(s+16,512);put32(s+20,512);put32(s+36,0xe0000020);b[512]=0xc3;
    if(dll){
        put32(b+0x98+96,0x1100);put32(b+0x98+100,0x100);
        put32(b+0x300+20,1);put32(b+0x300+24,1);
        put32(b+0x300+28,0x1140);put32(b+0x300+32,0x1144);put32(b+0x300+36,0x1148);
        put32(b+0x340,0x1000);put32(b+0x344,0x1150);
        memcpy(b+0x350,"CiukiProcessStart",18);
    }
}
static bool loader_missing;
static int loader_open(void *cwd,const char *path,struct ciuki_file *out){
    (void)cwd;
    if(!strcmp(path,"/fixture.exe")){*out=(struct ciuki_file){pe_data,sizeof(pe_data),pe_read,0};return 0;}
    if(!strcmp(path,"/fixture.dll")){*out=(struct ciuki_file){dll_data,sizeof(dll_data),pe_read,0};return 0;}
    CHECK(!strcmp(path,"/System/Win32/ntdll.dll"));if(loader_missing)return -ENOENT;
    *out=(struct ciuki_file){loader_data,sizeof(loader_data),pe_read,0};return 0;
}
static void test_advpack_process_flags(struct proc_thread *parent)
{
    struct ciuki_w32_vm scratch={.bytes=4096,.flags=W32_MEM_RESERVE|W32_MEM_COMMIT,.protect=W32_PAGE_READWRITE};
    CHECK(!w32_vm_alloc(parent->process->memory,&scratch));
    CHECK(!ua_write(parent->process->memory,scratch.address,"C:\\fixture.exe",15));
    const char command[]="\"C:\\fixture.exe\" /from-inf",env[]="A=B\0\0";
    CHECK(!ua_write(parent->process->memory,scratch.address+256,command,sizeof(command)));
    CHECK(!ua_write(parent->process->memory,scratch.address+512,env,sizeof(env)));
    const uint32_t flags[]={0,0x04000000,0x200,0x04000200,0x04000210};
    for(unsigned i=0;i<ARRAY_SIZE(flags);i++){
        struct ciuki_w32_spawn_args a={.image=scratch.address,.command=scratch.address+256,
            .environment=scratch.address+512,.environment_bytes=sizeof(env),.flags=flags[i]};
        CHECK(!w32_process_call(parent,&a) && !a.error && a.pid && a.process_handle && a.thread_handle);
        struct process *child=proc_find(a.pid);CHECK(child && child->published);
        CHECK(security_same(parent->process,child));
        CHECK(child->subject!=parent->process->subject);
        CHECK(child->pgid==((flags[i]&0x200) && !(flags[i]&0x10)?child->pid:parent->process->pgid));
        uint32_t parameters;struct ciuki_w32_parameters p;char copied[sizeof(command)];
        CHECK(!ua_read(child->memory,&parameters,child->w32_peb+16,4));
        CHECK(!ua_read(child->memory,&p,parameters,sizeof(p)));
        CHECK(!ua_read(child->memory,copied,p.command_line,sizeof(copied)) && !memcmp(copied,command,sizeof(command)));
        CHECK(!w32_handle_close(parent->process,a.process_handle) && !w32_handle_close(parent->process,a.thread_handle));
        proc_discard(child);
    }
    /* fx-launchfb: real CreateProcess publication, independent pending children,
     * FORCEOFF/ON, delegated desktop query, presentation, idle, exit and timeout. */
    struct process *children[4];
    const uint32_t feedback_flags[]={0,0x40,0x80,0xc0};
    struct ciuki_w32_system_args denied={.action=W32_S_LAUNCH_FEEDBACK};
    CHECK(!w32_system_call(parent,&denied) && denied.error==5);
    for(unsigned i=0;i<4;i++){
        struct ciuki_w32_startup startup={.flags=feedback_flags[i]};
        CHECK(!ua_write(parent->process->memory,scratch.address+768,&startup,sizeof(startup)));
        struct ciuki_w32_spawn_args spawn={.image=scratch.address,.command=scratch.address+256,
            .environment=scratch.address+512,.environment_bytes=sizeof(env),.startup=scratch.address+768};
        CHECK(!w32_process_call(parent,&spawn) && !spawn.error);
        children[i]=proc_find(spawn.pid);CHECK(children[i]);
        CHECK(!!children[i]->w32->launch_feedback_until==(i<2));
        CHECK(!w32_handle_close(parent->process,spawn.process_handle));
        CHECK(!w32_handle_close(parent->process,spawn.thread_handle));
    }
    uint32_t saved_desktop=fake_desktop_pid;
    struct w32_process desktop_w32={.service=W32_SERVICE_DESKTOP};
    struct process desktop={.pid=0x123456,.state=PROC_LIVE,.personality=PROC_NATIVE,.w32=&desktop_w32,.subject=parent->process->subject};
    struct proc_thread desktop_thread={.process=&desktop};fake_desktop_pid=desktop.pid;
    parent->process->w32->launch_feedback_until=0;
    struct ciuki_w32_system_args query={.action=W32_S_LAUNCH_FEEDBACK};
    CHECK(!w32_system_call(&desktop_thread,&query) && !query.error && query.result==2);
    query.value=children[0]->pid;
    CHECK(!w32_system_call(&desktop_thread,&query) && !query.error && query.result==1);
    struct proc_thread child_thread={.process=children[1]};
    struct ciuki_w32_system_args idle={.action=W32_S_LAUNCH_IDLE};
    CHECK(!w32_system_call(&child_thread,&idle) && !idle.error);
    query.value=0;CHECK(!w32_system_call(&desktop_thread,&query) && query.result==0);
    children[0]->w32->launch_feedback_until=w32_now()+5000;
    children[1]->w32->launch_feedback_until=w32_now()+5000;
    children[1]->state=PROC_ZOMBIE;
    CHECK(!w32_system_call(&desktop_thread,&query) && query.result==1);
    uint64_t saved_ticks=g_ticks;g_ticks+=5000;
    CHECK(!w32_system_call(&desktop_thread,&query) && !query.result && !query.low && !query.high);
    g_ticks=saved_ticks;
    children[1]->state=PROC_LIVE;
    for(unsigned i=0;i<4;i++)proc_discard(children[i]);
    fake_desktop_pid=saved_desktop;
    puts("PASS fx-launchfb kernel: CreateProcess default/ON/OFF/both, overlap, authority, present, idle, exit, timeout");
    struct ciuki_w32_spawn_args bad={.flags=0x04000202};
    CHECK(!w32_process_call(parent,&bad) && bad.error==87 && !bad.pid && !bad.process_handle && !bad.thread_handle);
    struct ciuki_w32_vm release={.address=scratch.address,.flags=W32_MEM_RELEASE};
    CHECK(!w32_vm_free(parent->process->memory,&release));
    puts("w32 advpack spawn: default error mode, new group, console precedence, verbatim command and unknown-flag refusal PASS");
}
static void test_spawn_exceptions(void)
{
    proc_init();make_pe(pe_data,false);make_pe(loader_data,true);
    struct ciuki_file f={pe_data,sizeof(pe_data),pe_read,0};struct w32_image *im=malloc(sizeof(*im));CHECK(im);
    CHECK(w32_is_pe(&f)&&!w32_pe_validate(&f,im));
    put32(pe_data+60,UINT32_MAX);CHECK(w32_is_pe(&f)&&w32_pe_validate(&f,im)==-ENOEXEC);make_pe(pe_data,false);
    put32(pe_data+0x98+96+4*8,0x2300);put32(pe_data+0x98+100+4*8,8);CHECK(!w32_pe_validate(&f,im));make_pe(pe_data,false);
    const struct ciuki_file_ops ops={.open=loader_open};proc_set_file_ops(&ops);
    struct proc_strings *s=proc_strings_new();CHECK(s);CHECK(!proc_strings_add(s,"/fixture.exe",13,false));CHECK(!proc_strings_add(s,"\"quoted  argument\"",19,false));
    struct process *p;loader_missing=true;unsigned baseline=pages_used;
    CHECK(proc_spawn_file(proc_supervisor(),&f,s,0,0,0,0,&p)==-ENOENT&&pages_used==baseline);
    loader_missing=false;CHECK(proc_spawn_file(proc_supervisor(),&f,s,0,0,0,0,&p)>0);proc_strings_free(s);
    struct proc_thread *t=0;for(unsigned i=0;i<CIUKI_THREAD_MAX;i++)if(proc_thread_slot(i)&&proc_thread_slot(i)->process==p){t=proc_thread_slot(i);break;}
    CHECK(t&&p->personality==PROC_WIN32&&t->teb&&t->emergency&&t->stack_base+t->stack_bytes<p->w32_image);
    uint32_t prefix[14];CHECK(!ua_read(p->memory,prefix,t->teb,sizeof(prefix))&&prefix[0]==UINT32_MAX&&prefix[6]==t->teb&&prefix[8]==p->pid&&prefix[9]==t->tid&&prefix[12]==p->w32_peb);
    uint32_t params;CHECK(!ua_read(p->memory,&params,p->w32_peb+16,4));struct ciuki_w32_parameters par;CHECK(!ua_read(p->memory,&par,params,sizeof(par)));char command[24];CHECK(!ua_read(p->memory,command,par.command_line,19)&&!strcmp(command,"\"quoted  argument\""));
    uint8_t gate[3];CHECK(!ua_read(p->memory,gate,CIUKI_W32_GATE,3)&&gate[0]==0xcd&&gate[1]==0x80&&gate[2]==0xc3);
    p->w32_dispatcher=0x70011000;t->task->win32=true;
    uint8_t fp[512] __attribute__((aligned(16)));t->task->fpu_area=fp;g_cpu_fxsr=true;g_cpu_sse=true;fpu_init();g_current=t->task;t->task->state=T_RUNNING;
    test_advpack_process_flags(t);
    uint32_t original_source=p->w32_source;
    struct ciuki_w32_compat_args fingerprint={.action=1,.settings=t->teb+1024,.bytes=65};
    CHECK(original_source&&!w32_compat_call(t,&fingerprint)&&p->w32_source==original_source&&fingerprint.image_size==0x3000);
    CHECK(!ua_write(p->memory,t->teb+1536,"/fixture.exe",13));
    struct ciuki_w32_loader_args original_open={.action=W32_LDR_OPEN,.path=t->teb+1536};
    CHECK(!w32_loader_call(t,&original_open)&&original_open.address==original_source&&!p->w32_source);
    /* fx-launch: LDR_IMAGE of a DLL not yet in the image-section cache hashes
     * its pinned snapshot once (it hashed it twice: the loader, then the
     * section lookup), and a second process view reuses the section. */
    make_pe(dll_data,true);put32(dll_data+0x98+28,0x10000000);
    CHECK(!ua_write(p->memory,t->teb+1536,"/fixture.dll",13));
    struct ciuki_w32_loader_args dll_open={.action=W32_LDR_OPEN,.path=t->teb+1536};
    CHECK(!w32_loader_call(t,&dll_open)&&dll_open.address&&dll_open.bytes==sizeof(dll_data));
    /* The file may change after OPEN; IMAGE must still use the private snapshot. */
    dll_data[512]=0x90;
    uint64_t hashed=sha256_bytes_hashed;
    struct ciuki_w32_loader_args dll_image={.action=W32_LDR_IMAGE,.source=dll_open.address,.bytes=dll_open.bytes};
    CHECK(!w32_loader_call(t,&dll_image)&&dll_image.address==0x10000000);
    CHECK(sha256_bytes_hashed-hashed==sizeof(dll_data));
    uint8_t mapped[2];CHECK(!ua_read(p->memory,mapped,0x10000000,2)&&mapped[0]=='M'&&mapped[1]=='Z');
    CHECK(!ua_read(p->memory,mapped,0x10001000,1) && mapped[0]==0xc3);
    dll_data[512]=0xc3;
    struct ciuki_w32_loader_args dll_unmap={.action=W32_LDR_UNMAP,.address=0x10000000};
    CHECK(!w32_loader_call(t,&dll_unmap));
    struct ciuki_w32_vm dll_release={.address=dll_open.address,.flags=W32_MEM_RELEASE};
    CHECK(!w32_vm_free(p->memory,&dll_release));
    struct trap_frame tf={.vector=6,.gs=0x23,.fs=0x4b,.es=0x23,.ds=0x23,.cs=0x1b,.user_ss=0x23,.eip=0x401000,.user_esp=t->stack_base+t->stack_bytes-32,.eflags=0x202};
    struct ciuki_w32_context ctx;w32_context_capture(&ctx,&tf);fpu_w32_export(t->task,&ctx);
    struct ciuki_w32_exception exc={.code=W32_EXCEPTION_ILLEGAL,.address=tf.eip};
    w32_deliver(t,&tf,&exc,&ctx);CHECK(t->w32_delivery&&!t->in_handler&&tf.eip==p->w32_dispatcher);
    w32_deliver(t,&tf,&exc,&ctx);CHECK(t->w32_delivery->next);
    CHECK(w32_continue(t,&tf,&ctx)&&t->w32_delivery);CHECK(w32_continue(t,&tf,&ctx)&&!t->w32_delivery&&tf.eip==0x401000);
    CHECK(w32_continue(t,&tf,&ctx)&&!t->w32_delivery); /* RtlUnwind outside delivery */
    exc.flags=1;w32_deliver(t,&tf,&exc,&ctx);
    CHECK(w32_continue(t,&tf,&ctx)&&!t->w32_delivery); /* noncontinuable handled by unwind */
    exc.flags=0;
    tf.user_esp=1;w32_deliver(t,&tf,&exc,&ctx);CHECK(t->w32_delivery->emergency&&tf.user_esp>=t->emergency);CHECK(w32_continue(t,&tf,&ctx));
    exc.code=W32_EXCEPTION_STACK;w32_context_restore(&tf,&ctx);
    w32_deliver(t,&tf,&exc,&ctx);CHECK(t->w32_delivery->emergency);CHECK(w32_continue(t,&tf,&ctx));exc.code=W32_EXCEPTION_ILLEGAL;
    uint32_t guard=t->stack_guard;bool retry;CHECK(!w32_guard_fault(t,guard,&retry)&&retry&&t->stack_guard==guard-4096);
    struct ciuki_w32_identity id={.header={1,sizeof(id),W32_IDENTITY,0}};uint32_t va=t->teb+256;CHECK(!ua_write(p->memory,va,&id,sizeof(id)));
    tf=(struct trap_frame){.eax=96,.ebx=va,.ecx=sizeof(id)};CHECK(!w32_syscall(&tf));CHECK(!ua_read(p->memory,&id,va,sizeof(id))&&id.build==2222&&!p->memory->pins);
    tf=(struct trap_frame){.eax=CIUKI_SYS_BRK};CHECK(proc_syscall(&tf)==-EPERM);
    /* Invalid continuation kills only its process; a native peer survives. */
    struct process *peer;struct proc_thread *peer_thread=make_process(proc_supervisor(),&peer);CHECK(peer_thread);
    w32_context_restore(&tf,&ctx);tf.vector=6;w32_deliver(t,&tf,&exc,&ctx);
    ctx.cs=8;expect_exit=true;
    if(!setjmp(exited)){w32_continue(t,&tf,&ctx);CHECK(false);}
    expect_exit=false;CHECK(p->raw_exit==(int32_t)W32_STATUS_INVALID_PARAMETER&&peer->state==PROC_LIVE);
    fpu_task_release(t->task);g_current=&controller;proc_collect();proc_stop(peer,0,0);proc_collect();proc_set_file_ops(0);free(im);
    CHECK(!g_user_mappings&&!pages_used);
    puts("w32 spawn/delivery: missing-loader rollback, preferred image, TEB/PEB/verbatim parameters, gate, nested continue, emergency stack, stack growth, syscall copying, teardown PASS");
}
struct file_blob { uint8_t *data; uint32_t bytes; };
static int blob_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    const struct file_blob *b = cookie;
    if (off > b->bytes || bytes > b->bytes - off) return -EIO;
    memcpy(dst, b->data + off, bytes); return 0;
}

#include "kernel_objects_test.inc"

static struct process *alias_loader_process;
static uint32_t alias_loader_write;
static unsigned alias_loader_waits;
static void change_loader_alias(void)
{
    CHECK(image_section_building && alias_loader_process->memory->pins);
    const uint8_t changed=0x90;
    CHECK(!ua_user_write(alias_loader_process->memory,alias_loader_write+512,&changed,1));
    alias_loader_waits++;
    image_section_building=false;
    on_schedule=0;
}
static void test_loader_shared_alias(void)
{
    struct process *p;
    struct proc_thread *t=object_process(proc_supervisor(),&p);
    g_current=t->task; g_current->state=T_RUNNING;
    make_pe(dll_data,true); put32(dll_data+0x98+28,0x10000000);
    struct ciuki_w32_section_args section={.kind=W32_SEC_PAGEFILE,.bytes=sizeof(dll_data),
                                          .protect=W32_PAGE_READWRITE};
    CHECK(!w32_section_call(t,&section) && !section.error);
    struct ciuki_w32_view_args writer={.action=W32_VIEW_MAP,.section=section.result,
                                      .bytes=sizeof(dll_data),.access=W32_FILE_MAP_WRITE};
    CHECK(!w32_view_call(t,&writer) && !writer.error);
    struct ciuki_w32_view_args reader={.action=W32_VIEW_MAP,.section=section.result,
                                      .bytes=sizeof(dll_data),.access=W32_FILE_MAP_READ};
    CHECK(!w32_view_call(t,&reader) && !reader.error);
    CHECK(!ua_user_write(p->memory,writer.address,dll_data,sizeof(dll_data)));
    uint8_t byte=0;
    CHECK(!ua_user_read(p->memory,&byte,reader.address+512,1) && byte==0xc3);
    alias_loader_process=p; alias_loader_write=writer.address; alias_loader_waits=0;
    image_section_building=true; on_schedule=change_loader_alias;
    uint64_t hashed=sha256_bytes_hashed;
    struct ciuki_w32_loader_args image={.action=W32_LDR_IMAGE,.source=reader.address,
                                       .bytes=sizeof(dll_data)};
    uint32_t status=w32_loader_call(t,&image);
    /* Before the fix this waits, seeds changed bytes B with digest A, and
     * succeeds. The fixed API must refuse before hashing or waiting. */
    if(alias_loader_waits)
        fprintf(stderr,"loader alias mutated during serialization wait: status=%#x\n",status);
    CHECK(status==W32_STATUS_ACCESS_DENIED);
    CHECK(!alias_loader_waits && sha256_bytes_hashed==hashed && !p->memory->pins);
    on_schedule=0; image_section_building=false;
    /* Refusal does not freeze shared memory or leave a cache/view behind. */
    const uint8_t changed=0x90;
    CHECK(!ua_user_write(p->memory,writer.address+512,&changed,1));
    CHECK(!ua_user_read(p->memory,&byte,reader.address+512,1) && byte==changed);
    CHECK(!image.address && !w32_view_contains(p->memory,0x10000000));
    /* A private snapshot remains immutable while a writer changes the shared
     * source during the real serialization wait. Still hash just once. */
    CHECK(!ua_user_write(p->memory,writer.address,dll_data,sizeof(dll_data)));
    struct loader_snapshot source={p->memory,reader.address};
    struct ciuki_file file={&source,sizeof(dll_data),loader_read,0};
    uint32_t snapshot=0;
    CHECK(!w32_loader_snapshot(p,&file,&snapshot));
    image=(struct ciuki_w32_loader_args){.action=W32_LDR_IMAGE,.source=snapshot,.bytes=file.bytes};
    /* Reject a writable later extent even when the first extent is private RO. */
    struct ciuki_w32_vm protect={.address=snapshot+PAGE_SIZE,.bytes=PAGE_SIZE,
                                 .protect=W32_PAGE_READWRITE};
    CHECK(!w32_vm_protect(p->memory,&protect));
    CHECK(w32_loader_call(t,&image)==W32_STATUS_ACCESS_DENIED && !p->memory->pins);
    CHECK(sha256_bytes_hashed==hashed);
    protect.protect=W32_PAGE_READONLY;
    CHECK(!w32_vm_protect(p->memory,&protect));
    image_section_building=true; on_schedule=change_loader_alias;
    CHECK(!w32_loader_call(t,&image) && image.address==0x10000000);
    CHECK(alias_loader_waits==1 && !on_schedule && !image_section_building && !p->memory->pins);
    CHECK(sha256_bytes_hashed-hashed==sizeof(dll_data));
    CHECK(!ua_user_read(p->memory,&byte,reader.address+512,1) && byte==changed);
    CHECK(!ua_user_read(p->memory,&byte,image.address+PAGE_SIZE,1) && byte==0xc3);
    stop_collect(p); alias_loader_process=0;
    CHECK(!pages_used && !g_user_mappings && heap_blocks==1);
    puts("w32 loader: shared alias refusal, later writable extent, immutable snapshot across serialization/hash PASS");
}
#include "../../../src/kernel/proc/win32/counter.c"
static uint64_t shared_cycles(void) { return __builtin_ia32_rdtsc(); }
static unsigned shared_extents(const struct uaddr *u)
{ unsigned n=0; for(const struct ua_extent *e=u->head;e;e=e->next)n++; return n; }
/* What the tick did before t23-render for one live Win32 process
 * (counter.c w32_shared_update at 50a843517): one ua_range, one ua_read
 * and three ua_write calls on CIUKI_W32_SHARED. */
static void shared_old_update(struct process *p)
{
    struct ciuki_w32_shared data;
    CHECK(ua_range(p->memory,CIUKI_W32_SHARED,sizeof(data),PROT_READ));
    CHECK(!ua_read(p->memory,&data,CIUKI_W32_SHARED,sizeof(data)));
    CHECK(!ua_write(p->memory,CIUKI_W32_SHARED,&data.sequence,4));
    CHECK(!ua_write(p->memory,CIUKI_W32_SHARED+4,(uint8_t *)&data+4,sizeof(data)-4));
    CHECK(!ua_write(p->memory,CIUKI_W32_SHARED,&data.sequence,4));
}
/* t23-render, trial c42f556e: with the Quake III installer running, one
 * shared tick took 1.0-1.08 ms on device-work ([work] budget exceeded
 * site=shared_tick_work) and device-work held 274-342 permille. Every Win32
 * process maps the same frame (w32_shared_map); the tick now publishes it
 * once, through the direct map, without walking any process's extents. */
static void test_shared_tick(void)
{
    struct process *a,*b;
    struct proc_thread *ta=object_process(proc_supervisor(),&a),*tb=object_process(proc_supervisor(),&b);
    CHECK(ta && tb && w32_shared_frame);
    g_current=ta->task; ta->task->state=T_RUNNING;
    /* An installer-sized address space beside two small ones. */
    for(unsigned i=0;i<1500;i++){
        struct ciuki_w32_vm r=request(0x20000000u+i*0x20000u,0x10000,W32_MEM_RESERVE,W32_PAGE_READWRITE);
        CHECK(!w32_vm_alloc(a->memory,&r));
    }
    unsigned extents=shared_extents(a->memory),small=shared_extents(b->memory);
    CHECK(extents>1500);
    unsigned before=walk_work; uint64_t t0=shared_cycles();
    shared_old_update(a); shared_old_update(b);
    uint64_t old_cycles=shared_cycles()-t0; unsigned old_steps=walk_work-before;
    /* fx-uafind (AVL extents): the old per-process update no longer walks
     * the 1500 extents linearly (indexed lookups count no walk steps); the
     * assertion that matters is below: the tick itself touches no extent. */
    CHECK(old_steps<5*1500);
    g_ticks+=5;
    before=walk_work; t0=shared_cycles();
    w32_shared_tick();
    uint64_t new_cycles=shared_cycles()-t0;
    CHECK(walk_work==before && !a->memory->pins && !b->memory->pins);
    struct ciuki_w32_shared seen_a,seen_b;
    CHECK(!ua_read(a->memory,&seen_a,CIUKI_W32_SHARED,sizeof(seen_a)));
    CHECK(!ua_read(b->memory,&seen_b,CIUKI_W32_SHARED,sizeof(seen_b)));
    uint64_t interrupt=((uint64_t)seen_a.interrupt_hi<<32)|seen_a.interrupt_lo;
    uint64_t system=((uint64_t)seen_a.system_hi<<32)|seen_a.system_lo;
    struct ciuki_timespec wall; CHECK(!file_clock_now(CLOCK_REALTIME,0,&wall));
    CHECK(!memcmp(&seen_a,&seen_b,sizeof(seen_a)) && !(seen_a.sequence&1));
    CHECK(seen_a.tick==(uint32_t)g_ticks && interrupt==g_ticks*10000);
    CHECK(system/10000000==(uint64_t)wall.tv_sec+11644473600ULL);
    uint32_t sequence=seen_a.sequence; g_ticks++;
    w32_shared_tick();
    CHECK(!ua_read(b->memory,&seen_b,CIUKI_W32_SHARED,sizeof(seen_b)) && seen_b.sequence==sequence+2 && seen_b.tick==(uint32_t)g_ticks);
    /* The page stays published while any Win32 process maps it, and its
     * frame is forgotten before the last unmap frees it. */
    stop_collect(a);
    CHECK(w32_shared_frame);
    w32_shared_tick();
    CHECK(!ua_read(b->memory,&seen_b,CIUKI_W32_SHARED,sizeof(seen_b)) && seen_b.sequence==sequence+4);
    stop_collect(b);
    CHECK(!w32_shared_frame);
    w32_shared_tick();
    CHECK(!pages_used && !g_user_mappings && heap_blocks==1);
    printf("w32 shared tick: extents=%u+%u old_walk_steps=%u old_host_cycles=%llu new_walk_steps=0 new_host_cycles=%llu PASS\n",
           extents,small,old_steps,(unsigned long long)old_cycles,(unsigned long long)new_cycles);
}
#ifdef W32_KERNEL_BRIDGE
#define main kernel_mechanism_main
#endif
#include "../security/win32_isolation_test.inc"
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--pe")) {
        FILE *f = fopen(argv[2], "rb"); CHECK(f);
        CHECK(!fseek(f,0,SEEK_END)); long size = ftell(f); CHECK(size >= 0 && size <= 1048576);
        rewind(f); struct file_blob b = { malloc((size_t)size + 1), (uint32_t)size }; CHECK(b.data);
        CHECK(fread(b.data,1,b.bytes,f)==b.bytes); fclose(f);
        struct ciuki_file image={&b,b.bytes,blob_read,0};
        struct w32_image *parsed=malloc(sizeof(*parsed)); CHECK(parsed);
        int err=w32_pe_validate(&image,parsed);
        printf("%s\n",err?(parsed->failure?parsed->failure:"read-error"):"valid");
        free(parsed);free(b.data);return 0;
    }
    ram=calloc(HOST_PAGES,PAGE_SIZE);CHECK(ram);controller.state=T_RUNNING;g_current=&controller;
    if(argc==2&&!strcmp(argv[1],"--security")){
        setvbuf(stdout,NULL,_IONBF,0);test_security_isolation();
        CHECK(!pages_used&&!g_user_mappings&&heap_blocks==1);free(ram);return 0;
    }
    test_memory();test_records();test_fpu();test_context();test_spawn_exceptions();test_objects_sections();test_loader_shared_alias();test_shared_tick();
    CHECK(!pages_used&&!g_user_mappings&&heap_blocks==1);printf("w32 core: checks=%u PASS\n",checks);free(ram);return 0;
}
