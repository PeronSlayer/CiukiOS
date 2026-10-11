/* T0: production DOS lifecycle/BIOS/disk/monitor and paging functions.
 * Only CPU, scheduler, PMM and VFS transport are fake. No DOS boot claimed.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/input.h>
#include <ciuki/dosvm.h>

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#define KERNEL_VBASE 0xC0000000u
#define P2V(p) ((void *)(ram + (p)))
#define V2P(p) ((uint32_t)((uint8_t *)(p) - ram))
#define HOST_PAGES 32768u
static unsigned checks, used_pages, heap_blocks, allocation_calls, fail_at;
static bool allocated[HOST_PAGES];
static uint8_t *ram;
static uint32_t cpu_cr3, cpu_flags = V86_IF, next_pfn = 4096;
static uint32_t kpd[1024], kpd_phys;
static struct task caller;
struct task *g_current = &caller;
uint32_t g_task_count, g_user_mappings;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
static uint32_t read_cr3(void) { return cpu_cr3; }
static void write_cr3(uint32_t x) { cpu_cr3 = x; }
static uint32_t read_cr2(void) { return 0xDEAD000; }
static void cli(void) { cpu_flags = 0; }
static void sti(void) { cpu_flags = V86_IF; }
static void invlpg(uint32_t va) { (void)va; }
static uint32_t *table_va(uint32_t p) { return P2V(p); }
static uint32_t irq_save(void) { uint32_t f=cpu_flags; cli(); return f; }
static void irq_restore(uint32_t f) { cpu_flags=f; }
#include "../../src/kernel/vm/dosvm_internal.h"
static bool allocation_fails(void) { return ++allocation_calls == fail_at; }
uint32_t pmm_alloc(void)
{
    if (allocation_fails()) return 0;
    /* Deliberately separate consecutive mappings in physical memory. */
    for (unsigned n = 0; n < HOST_PAGES; n++) {
        unsigned p = next_pfn; next_pfn += 2;
        if (next_pfn >= HOST_PAGES) next_pfn = 4096 + ((next_pfn & 1) ^ 1);
        if (!allocated[p]) { allocated[p] = true; used_pages++; memset(P2V(p * PAGE_SIZE),0xA5,PAGE_SIZE); return p * PAGE_SIZE; }
    }
    return 0;
}
uint32_t pmm_alloc_zone(int zone) { CHECK(zone == ZONE_NORMAL); return pmm_alloc(); }
void pmm_free(uint32_t p) { CHECK(p >= 0x1000000 && !(p & 4095) && p / PAGE_SIZE < HOST_PAGES && allocated[p / PAGE_SIZE]); allocated[p / PAGE_SIZE] = false; used_pages--; }
uint32_t pmm_free_count(void) { return HOST_PAGES - 4096 - used_pages; }
uint32_t vmm_kernel_pd(void) { return kpd_phys; }
void panic(const char *fmt, ...) { fprintf(stderr,"unexpected panic: %s\n",fmt); abort(); }
void klog(const char *fmt, ...) { (void)fmt; }
void *kmalloc(size_t n) { if (allocation_fails()) return 0; void *p = malloc(n); if (p) heap_blocks++; return p; }
void kfree(void *p) { if (p) { CHECK(heap_blocks); heap_blocks--; free(p); } }
/* Generated from the unmodified production user-address-space section. */
#include "dosvm_aspace.inc"

static struct { struct task *t; void (*fn)(void *); void *arg; } tasks[DOSVM_LIMIT];
static jmp_buf guest_return, worker_return;
static void (*sleep_hook)(void);
static unsigned native_progress, guest_enters;
static bool guest_halt;
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    CHECK(!strcmp(name,"dosvm") && prio == P_NORMAL);
    struct task *t = kmalloc(sizeof(*t));
    if (!t) return 0;
    memset(t,0,sizeof(*t)); t->state = T_BLOCKED;
    t->kstack = kmalloc(KSTACK_SIZE); t->fpu_alloc = kmalloc(512);
    if (!t->kstack || !t->fpu_alloc) { kfree(t->kstack); kfree(t->fpu_alloc); kfree(t); return 0; }
    t->fpu_area = t->fpu_alloc;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (!tasks[i].t) { tasks[i].t=t; tasks[i].fn=fn; tasks[i].arg=arg; g_task_count++; return t; }
    CHECK(false); return 0;
}
void task_start(struct task *t) { CHECK(t->state == T_BLOCKED); t->state = T_READY; }
bool task_alive(const struct task *t) { return t->state < T_ZOMBIE; }
void task_kill(struct task *t, int reason) { t->state = T_ZOMBIE; t->exit_code = reason; }
void task_reap(struct task *t)
{
    CHECK(t != g_current && t->state == T_ZOMBIE);
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (tasks[i].t == t) tasks[i].t=0;
    g_task_count--; kfree(t->kstack); kfree(t->fpu_alloc); kfree(t);
}
void task_exit(int reason) { task_kill(g_current,reason); longjmp(worker_return,1); }
void schedule(void) { native_progress++; g_need_resched = false; }
void task_sleep_ms(uint32_t ms)
{
    CHECK(cpu_flags & V86_IF); g_ticks += ms; dosvm_tick(); native_progress++;
    if (sleep_hook) sleep_hook();
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (tasks[i].t) {
        struct dosvm *v = tasks[i].arg;
        if (v->info.state == DOSVM_STOPPING) task_kill(tasks[i].t,v->info.reason);
    }
}
void tss_set_kernel_stack(uint32_t sp) { (void)sp; }
void fpu_handle_nm(void) { CHECK(g_current->fpu_area != 0); }
gen_t gen_alloc(void) { static gen_t generation; return ++generation; }
bool gen_matches(gen_t a, gen_t b) { return a && a == b; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
bool deadline_passed(uint64_t t) { return g_ticks - t < (1ull << 63); }
void kwait_init(struct kwait *q) { q->head=0; }
void kwait_wake_all(struct kwait *q) { (void)q; }
void kmutex_init(struct kmutex *m) { memset(m,0,sizeof(*m)); }
void kmutex_lock(struct kmutex *m) { CHECK((cpu_flags&V86_IF) && !m->owner); m->owner=g_current; }
void kmutex_unlock(struct kmutex *m) { CHECK(m->owner==g_current); m->owner=0; }
bool kwait_wait_until(struct kwait *q, kwait_cond_fn fn, void *arg, uint64_t until)
{ (void)q; while (!fn(arg) && !deadline_passed(until)) task_sleep_ms(1); return fn(arg); }
static bool input_pending;
static struct input_event input_next;
bool input_read(struct input_event *e) { if (!input_pending) return false; *e=input_next; input_pending=false; return true; }
void v86_leave(uint32_t continuation) { CHECK(continuation == 0x12345678); longjmp(guest_return,1); }
void v86_enter(const struct v86_frame *initial, uint32_t *continuation)
{
    struct v86_frame f = *initial; f.tf.vector=13; f.tf.edx=DOSVM_FAULT_PORT;
    if (guest_halt) {
        uint8_t opcode=0xF4;
        for (unsigned i=0;i<DOSVM_LIMIT;i++) if (tasks[i].t==g_current)
            CHECK(!dosvm_write(tasks[i].arg,f.tf.eip,&opcode,1));
    }
    *continuation=0x12345678; guest_enters++;
    CHECK(dosvm_task_cr3(g_current,0) == cpu_cr3 && dosvm_task_esp0(g_current,0) == *continuation);
    if (!setjmp(guest_return)) { dosvm_trap(&f.tf); CHECK(false); }
}
static void run_worker(struct dosvm *v)
{
    for (unsigned i=0;i<DOSVM_LIMIT;i++) if (tasks[i].t==v->task) {
        g_current=v->task;
        if (!setjmp(worker_return)) tasks[i].fn(tasks[i].arg);
        g_current=&caller; sti(); CHECK(cpu_cr3==kpd_phys); return;
    }
    CHECK(false);
}

/* File-backed fake VFS transport. Production overlay indexing remains real. */
static struct storage store = {.ready=true};
static struct { struct vfs_table *table; FILE *cow; bool base_open, cow_open; uint64_t pos[2]; char path[80]; } files[DOSVM_LIMIT];
static unsigned handles, tables, scratch, disk_reads, disk_writes, commits, deletes;
static unsigned io_calls, io_fail_at;
static bool short_read, short_write, fail_delete, fail_commit, fail_close, bad_signature;
static bool io_fails(void) { return ++io_calls == io_fail_at; }
static unsigned file_slot(struct vfs_table *table) { for (unsigned i=0;i<DOSVM_LIMIT;i++) if (files[i].table==table) return i; CHECK(false); return 0; }
static void disk_owned(struct vfs_table *table)
{ struct dosvm *v=(void *)((uint8_t *)table - offsetof(struct dosvm,disk_table)); CHECK(v->disk_lock.owner==g_current); }
struct storage *storage_get(void) { return &store; }
int vfs_table_init(struct vfs *vfs, struct vfs_table *table, unsigned drive)
{
    CHECK(vfs==&store.vfs && drive==2);
    if (io_fails()) return -V86_EIO;
    for (unsigned i=0;i<DOSVM_LIMIT;i++) if (!files[i].table) { memset(&files[i],0,sizeof(files[i])); files[i].table=table; tables++; return 0; }
    return -ENOSPC;
}
void vfs_table_destroy(struct vfs_table *table)
{ unsigned i=file_slot(table); CHECK(!files[i].cow && !files[i].base_open && !files[i].cow_open); files[i].table=0; tables--; }
int vfs_stat(struct vfs_table *table, const char *path, struct fat_entry *entry)
{ (void)file_slot(table); CHECK(!strcmp(path,"C:\\SYSTEM\\DOS\\DOSBOOT.IMG")); if (io_fails()) return -V86_EIO; memset(entry,0,sizeof(*entry)); entry->size=DOSVM_SECTORS*512u; return 0; }
int vfs_open(struct vfs_table *table, const char *path, unsigned access, enum vfs_share share, uint8_t attr)
{
    unsigned i=file_slot(table); CHECK(attr==0); if (io_fails()) return -V86_EIO;
    if (!strcmp(path,"C:\\SYSTEM\\DOS\\DOSBOOT.IMG")) { CHECK(access==VFS_READ && share==VFS_DENY_WRITE); files[i].base_open=true; handles++; return 0; }
    CHECK(access==(VFS_READ|VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE) && share==VFS_DENY_ALL && strstr(path,".COW"));
    snprintf(files[i].path,sizeof(files[i].path),"build/host/dosvm-cow-XXXXXX");
    int fd=mkstemp(files[i].path); CHECK(fd>=0); files[i].cow=fdopen(fd,"w+b"); CHECK(files[i].cow);
    files[i].cow_open=true; scratch++; handles++; return 1;
}
int vfs_close(struct vfs_table *table, int handle)
{ unsigned i=file_slot(table); if (handle==0) { CHECK(files[i].base_open); files[i].base_open=false; } else { CHECK(handle==1 && files[i].cow_open); files[i].cow_open=false; } handles--; return fail_close ? -V86_EIO : 0; }
int vfs_delete(struct vfs_table *table, const char *path)
{ unsigned i=file_slot(table); CHECK(strstr(path,".COW") && !files[i].cow_open); if (fail_delete) return -V86_EIO; CHECK(files[i].cow); CHECK(fclose(files[i].cow)==0 && unlink(files[i].path)==0); files[i].cow=0; scratch--; deletes++; return 0; }
int vfs_seek(struct vfs_table *table, int handle, int64_t offset, enum vfs_whence whence, uint64_t *position)
{ unsigned i=file_slot(table); disk_owned(table); CHECK(handle>=0 && handle<=1 && whence==VFS_SEEK_SET && offset>=0); if (io_fails()) return -V86_EIO; files[i].pos[handle]=offset; *position=offset; return 0; }
int vfs_read(struct vfs_table *table, int handle, void *data, size_t bytes, size_t *done)
{
    unsigned i=file_slot(table); disk_owned(table); CHECK(bytes==512); disk_reads++; if (io_fails()) return -V86_EIO;
    *done=short_read ? 511 : bytes;
    if (!handle) { memset(data,(files[i].pos[0]/512)%251,bytes); if (!files[i].pos[0]) { ((uint8_t *)data)[0]=0xEE; ((uint8_t *)data)[510]=bad_signature ? 0 : 0x55; ((uint8_t *)data)[511]=0xAA; } }
    else { CHECK(fseek(files[i].cow,(long)files[i].pos[1],SEEK_SET)==0); CHECK(fread(data,1,*done,files[i].cow)==*done); }
    return 0;
}
int vfs_write(struct vfs_table *table, int handle, const void *data, size_t bytes, size_t *done)
{ unsigned i=file_slot(table); disk_owned(table); CHECK(handle==1 && bytes==512); disk_writes++; if (io_fails()) return -V86_EIO; *done=short_write ? 511 : bytes; CHECK(fseek(files[i].cow,(long)files[i].pos[1],SEEK_SET)==0); CHECK(fwrite(data,1,*done,files[i].cow)==*done); return 0; }
int vfs_commit(struct vfs_table *table, int handle)
{ unsigned i=file_slot(table); CHECK(handle==1); commits++; if (fail_commit) return -V86_EIO; CHECK(fflush(files[i].cow)==0); return 0; }

static char last_record[256];
static unsigned records, guest_records, max_record;
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[240]; va_list ap; va_start(ap,fmt); int n=vsnprintf(extra,sizeof(extra),fmt,ap); va_end(ap);
    CHECK(n>=0 && n<(int)sizeof(extra) && !strcmp(probe,"vm-boot"));
    n=snprintf(last_record,sizeof(last_record),"CIUKI_TEST v=1 run=12345678 seq=%06u probe=%s event=%s %s",++records,probe,event,extra);
    CHECK(n>0 && n<=240); if ((unsigned)n>max_record) max_record=n;
    if (strstr(extra,"group=guest ")) guest_records++;
}
#include "../../src/kernel/vm/v86.c"
#include "../../src/kernel/vm/dosvm.c"
#include "../../src/kernel/vm/dosvm_disk.c"
#include "../../src/kernel/vm/dosvm_bios.c"

static struct dosvm *create(void)
{ struct dosvm *v; struct dosvm_config c={DOSVM_LOW_CAP}; CHECK(dosvm_create(&c,&v)==0 && v); CHECK(v->info.state==DOSVM_CREATING && v->info.resident<=DOSVM_LOW_CAP); return v; }
static void empty(void)
{ struct dosvm_ledger l; dosvm_snapshot(&l); CHECK(!l.vms && !l.pages && !l.tasks && !l.handles && !l.devices && !l.resident && !l.reserved); CHECK(!used_pages && !heap_blocks && !handles && !tables && !scratch && !g_task_count && !g_user_mappings); for (unsigned i=0;i<DOSVM_LIMIT;i++) CHECK(!slots[i].disk_lock.owner); }
static void destroy(struct dosvm *v, bool forced)
{ struct dosvm_key key=dosvm_key(v); CHECK(dosvm_destroy(key,forced)==0); CHECK(dosvm_destroy(key,forced)==0 && !dosvm_valid(key) && !dosvm_keyboard(key,'X') && !dosvm_irq_raise(key,0)); }
static void line(struct dosvm *v, const char *text)
{ while (*text) CHECK(dosvm_record_byte(v,*text++)==0); CHECK(dosvm_record_byte(v,10)==0); }
static void test_admission(void)
{
    struct dosvm_admission a;
    CHECK(dosvm_admit(40ull<<20,0,0,DOSVM_LOW_CAP,&a) && a.outstanding_bytes==0 && a.headroom_bytes==(32u<<20));
    CHECK(!dosvm_admit((40ull<<20)-1,0,0,DOSVM_LOW_CAP,&a));
    CHECK(dosvm_admit(47ull<<20,8ull<<20,1ull<<20,DOSVM_LOW_CAP,&a) && a.outstanding_bytes==(7ull<<20));
    CHECK(!dosvm_admit(46ull<<20,8ull<<20,1ull<<20,DOSVM_LOW_CAP,0));
    CHECK(!dosvm_admit(UINT64_MAX,1,2,DOSVM_LOW_CAP,0));
    CHECK(!dosvm_admit(UINT64_MAX,0,0,DOSVM_LOW_CAP-1,0));
    CHECK(!dosvm_admit(UINT64_MAX,0,0,DOSVM_RESIDENT_CAP+4096,0));
}
static void test_creation_failures(void)
{
    allocation_calls=0; struct dosvm *v=create(); unsigned allocations=allocation_calls; destroy(v,true); empty();
    for (unsigned i=1;i<=allocations;i++) { allocation_calls=0; fail_at=i; v=(void *)1; CHECK(dosvm_create(0,&v)==-ENOMEM && !v); fail_at=0; empty(); }
    for (unsigned i=1;i<=4;i++) { io_calls=0; io_fail_at=i; CHECK(dosvm_create(0,&v)<0 && !v); io_fail_at=0; empty(); }
    store.stopped=true; CHECK(dosvm_create(0,&v)<0 && !v); store.stopped=false; empty();
    v=create(); bad_signature=true; CHECK(dosvm_boot(v)<0 && v->info.state==DOSVM_CREATING); bad_signature=false; destroy(v,true); empty();
    printf("DOS creation failures: %u allocation sites + 4 VFS sites unwound\n",allocations);
}
static void test_memory_disk(void)
{
    struct dosvm *a=create(), *b=create(); uint8_t bytes[512], got[512]; memset(bytes,0x7B,sizeof(bytes));
    CHECK(a->frames[0]>=0x1000000 && a->frames[1]!=a->frames[0]+4096);
    CHECK(dosvm_write(a,0xFFF,bytes,sizeof(bytes))==0 && dosvm_read(a,0xFFF,got,sizeof(got))==0 && !memcmp(bytes,got,512));
    CHECK(dosvm_read(b,0xFFF,got,512)==0 && memcmp(bytes,got,512));
    CHECK(!dosvm_range(a,0x10FFEF,2,true) && !dosvm_range(a,UINT32_MAX,512,true));
    CHECK(dosvm_write(a,0xF0000,bytes,1)==-EFAULT);
    CHECK(dosvm_write(a,0xBFFF0,bytes,32)==-EFAULT); CHECK(dosvm_read(a,0xBFFF0,got,16)==0 && !got[0]);
    uint32_t x=0x12345678,y=0; CHECK(dosvm_write(a,0,&x,4)==0 && dosvm_set_a20(a,false)==0);
    CHECK(dosvm_read(a,0x100000,&y,4)==0 && y==x); CHECK(dosvm_set_a20(a,true)==0 && dosvm_read(a,0x100000,&y,4)==0 && !y);
    CHECK(dosvm_disk_write(a,77,bytes)==0 && dosvm_disk_read(a,77,got)==0 && !memcmp(bytes,got,512));
    CHECK(dosvm_disk_read(b,77,got)==0 && memcmp(bytes,got,512));
    memset(bytes,0x42,512); CHECK(dosvm_disk_write(a,77,bytes)==0 && a->sector_count==1);
    short_write=true; CHECK(dosvm_disk_write(a,78,bytes)<0 && a->sector_count==1); short_write=false;
    CHECK(dosvm_disk_read(a,78,got)==0 && got[0]==78);
    short_read=true; CHECK(dosvm_disk_read(a,0,got)<0); short_read=false;
    CHECK(dosvm_disk_write(a,DOSVM_SECTORS,bytes)==-EINVAL);
    for (unsigned i=0;i<DOSVM_OVERLAY_SECTORS;i++) if (i!=77) CHECK(dosvm_disk_write(a,i,bytes)==0);
    unsigned issued=disk_writes; CHECK(dosvm_disk_write(a,1024,bytes)==-ENOSPC && disk_writes==issued);
    CHECK(dosvm_set_a20(a,false)==0); destroy(a,true); CHECK(a->info.data_loss); destroy(b,false); empty();
}
static struct v86_frame disk_frame(unsigned ax)
{ return (struct v86_frame){.tf={.eax=ax,.edx=0x80,.ecx=1,.ebx=0xFFF,.eflags=V86_VM|V86_IF|2},.es=0x1000,.ds=0x2000}; }
static void test_bios_bounds(void)
{
    struct dosvm *v=create(); struct v86_frame f=disk_frame(0x0202); uint8_t data[1024];
    CHECK(dosvm_bios(v,&f,0x13)==0 && !(f.tf.eflags&V86_CF)); CHECK(dosvm_read(v,0x10FFF,data,1024)==0 && data[510]==0x55 && data[512]==1);
    unsigned reads=disk_reads,writes=disk_writes;
    f=disk_frame(0x0201); f.tf.edx=0x81; CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    f=disk_frame(0x0202); f.tf.ebx=0xFFFF; CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    f=disk_frame(0x0201); f.es=0xF000; CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    f=disk_frame(0x0300); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_writes==writes);
    uint8_t dap[16]={16,0,2,0,0xFF,0x0F,0,0x10,0xFE,0xFF,3,0,0,0,0,0};
    CHECK(dosvm_write(v,0x20000,dap,16)==0); f=disk_frame(0x4200); CHECK(!dosvm_bios(v,&f,0x13) && !(f.tf.eflags&V86_CF) && disk_reads==reads+2);
    reads=disk_reads; dap[2]=3; CHECK(dosvm_write(v,0x20000,dap,16)==0); f=disk_frame(0x4200); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    CHECK(dosvm_read(v,0x20000,data,16)==0 && data[2]==3);
    dap[2]=1; dap[12]=1; CHECK(dosvm_write(v,0x20000,dap,16)==0); f=disk_frame(0x4300); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_writes==writes);
    dap[12]=0; dap[0]=24; CHECK(dosvm_write(v,0x20000,dap,16)==0); f=disk_frame(0x4200); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.eflags&V86_CF) && disk_reads==reads);
    f=disk_frame(0x0800); CHECK(!dosvm_bios(v,&f,0x13) && (f.tf.ecx&63)==63 && (f.tf.edx>>8)==15);
    f=disk_frame(0x4100); f.tf.ebx=0x55AA; CHECK(!dosvm_bios(v,&f,0x13) && (uint16_t)f.tf.ebx==0xAA55);
    f=disk_frame(0xE820); CHECK(!dosvm_bios(v,&f,0x15) && (f.tf.eflags&V86_CF));
    v->clock_ms=3661000; f=disk_frame(0x0200); CHECK(!dosvm_bios(v,&f,0x1A) && f.tf.ecx==0x0101 && f.tf.edx==0x0100);
    v->clock_ms=86400000; f=disk_frame(0x0400); CHECK(!dosvm_bios(v,&f,0x1A) && f.tf.ecx==0x2026 && f.tf.edx==0x1012);
    /* Exercise the actual guest INT -> ROM service -> IRET convention,
     * including carry propagation into the original saved FLAGS. */
    const uint8_t instruction[]={0xCD,0x13}; CHECK(!dosvm_write(v,0x2000,instruction,2));
    f=disk_frame(0x0201); f.tf.cs=0; f.tf.eip=0x2000; f.tf.user_ss=0; f.tf.user_esp=0x3000; f.tf.edx=0x81;
    v->monitor.state=V86_RUNNING; v->monitor.vif=true;
    CHECK(!v86_emulate(&v->monitor,&f) && f.tf.cs==0xF000 && f.tf.eip==0x130);
    CHECK(!v86_emulate(&v->monitor,&f) && f.tf.eip==0x132 && (f.tf.eflags&V86_CF));
    CHECK(!v86_emulate(&v->monitor,&f) && f.tf.cs==0 && f.tf.eip==0x2002 && f.tf.user_esp==0x3000 && v->monitor.vif && (f.tf.eflags&V86_CF));
    destroy(v,false); empty();
}
static int port_handler(struct dosvm *v, uint16_t port, unsigned width, bool write, uint32_t *value, void *arg)
{ CHECK(v && port==0x300 && width==4 && !write && arg==(void *)7); *value=0xBEEF; return 0; }
static struct dosvm *waiting;
static void enqueue_key(void) { CHECK(dosvm_keyboard(dosvm_key(waiting),0x1E61)); sleep_hook=0; }
static void test_lifecycle_records(void)
{
    struct dosvm *v=create(); struct dosvm_key expired=dosvm_key(v); CHECK(dosvm_boot(v)==0 && v->info.state==DOSVM_BOOTING && !(v->frame.tf.eflags&0x3000));
    g_ticks+=1000; line(v,"DOSREADY 05.00 free=8000 C\r"); CHECK(v->info.dos_ready && v->info.free_paragraphs==0x8000 && v->info.drive==2 && v->info.state==DOSVM_RUNNABLE);
    unsigned count=guest_records; line(v,"CIUKI_TEST event=END status=PASS"); CHECK(guest_records==count+1 && strstr(last_record,"group=guest") && !strstr(last_record,"status=PASS"));
    for(unsigned i=0;i<80;i++) CHECK(!dosvm_record_byte(v,'X')); CHECK(!dosvm_record_byte(v,10)); CHECK(strstr(last_record,"part=2 parts=2"));
    count=guest_records; for(unsigned i=0;i<81;i++) CHECK(!dosvm_record_byte(v,'X')); CHECK(!dosvm_record_byte(v,10)); CHECK(guest_records==count);
    CHECK(!dosvm_record_byte(v,1)); line(v,"invalid"); CHECK(guest_records==count);
    unsigned accepted=v->info.guest_records; for(unsigned i=0;i<20;i++) line(v,"bounded"); CHECK(v->info.guest_records==16 && v->info.dropped_records>=9 && accepted<16);
    g_ticks+=1000; line(v,"new-window"); CHECK(v->info.guest_records==17);
    struct v86_frame peek={.tf={.eax=0x100}};
    input_next=(struct input_event){.type=INPUT_TEXT,.code='q',.tick=v->input_since-1}; input_pending=true;
    CHECK(!dosvm_bios(v,&peek,0x16) && (peek.tf.eflags&V86_ZF) && !v->key_count && !input_pending);
    input_next.tick=g_ticks; input_pending=true; peek.tf.eax=0x100;
    CHECK(!dosvm_bios(v,&peek,0x16) && peek.tf.eax=='q' && v->key_count==1 && !(peek.tf.eflags&V86_ZF));
    peek.tf.eax=0; CHECK(!dosvm_bios(v,&peek,0x16) && peek.tf.eax=='q' && !v->key_count);
    uint32_t value=0; CHECK(v86_io(&v->monitor,0x1F0,4,false,&value)==0 && value==UINT32_MAX);
    CHECK(!dosvm_port_register(expired,0x300,0x300,port_handler,(void *)7)); CHECK(!v86_io(&v->monitor,0x300,4,false,&value) && value==0xBEEF);
    CHECK(dosvm_port_register(expired,0x300,0x301,port_handler,0)<0 && dosvm_port_register(expired,0x20,0x20,port_handler,0)<0); CHECK(!dosvm_port_unregister(expired,0x300));
    struct v86_frame f={0}; waiting=v; sleep_hook=enqueue_key; unsigned progress=native_progress;
    CHECK(!dosvm_bios(v,&f,0x16) && f.tf.eax==0x1E61 && native_progress>progress && v->info.state==DOSVM_RUNNABLE);
    CHECK(dosvm_keyboard(expired,0x3062)); f.tf.eax=0x100; CHECK(!dosvm_bios(v,&f,0x16) && f.tf.eax==0x3062 && v->key_count==1);
    run_worker(v); CHECK(v->info.state==DOSVM_STOPPING && v->info.reason==DOSVM_FAULT && v->info.fault_vector==13); destroy(v,true); empty();
    struct dosvm *next=create(); CHECK(dosvm_key(next).id==expired.id && dosvm_key(next).generation!=expired.generation && !dosvm_keyboard(expired,'X'));
    CHECK(!dosvm_boot(next)); value=1; CHECK(!v86_io(&next->monitor,0x92,1,true,&value) && next->info.reason==DOSVM_RESET); destroy(next,true); empty();
    next=create(); CHECK(!dosvm_boot(next)); next->monitor.vif=false; next->boot_deadline=g_ticks+2;
    guest_halt=true; run_worker(next); guest_halt=false;
    CHECK(next->info.reason==DOSVM_TIMEOUT && next->info.state==DOSVM_STOPPING); destroy(next,true); empty();
}
static void test_cleanup_errors(void)
{
    struct dosvm *v=create(); uint8_t sector[512]={7}; CHECK(!dosvm_disk_write(v,5,sector)); unsigned committed=commits;
    fail_delete=true; CHECK(dosvm_destroy(dosvm_key(v),false)==-V86_EIO && v->info.state==DOSVM_STOPPING && !used_pages && !handles && scratch==1 && tables==1 && commits==committed+1);
    fail_delete=false; destroy(v,false); empty();
    v=create(); CHECK(!dosvm_disk_write(v,5,sector)); fail_commit=true; CHECK(dosvm_destroy(dosvm_key(v),false)==-V86_EIO && v->info.data_loss && v->info.state==DOSVM_DEAD); fail_commit=false; empty();
    v=create(); fail_close=true; CHECK(dosvm_destroy(dosvm_key(v),true)==-V86_EIO && v->info.state==DOSVM_DEAD); fail_close=false; empty();
    v=create(); CHECK(!dosvm_disk_write(v,5,sector)); committed=commits; destroy(v,true); CHECK(commits==committed && v->info.data_loss); empty();
}
int main(void)
{
    ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram);
    test_admission(); test_creation_failures(); test_memory_disk(); test_bios_bounds(); test_lifecycle_records(); test_cleanup_errors();
    CHECK(guest_enters==2 && deletes>1); free(ram);
    printf("dosvm host tests: PASS (%u checks, max record %u bytes)\n",checks,max_record); return 0;
}
