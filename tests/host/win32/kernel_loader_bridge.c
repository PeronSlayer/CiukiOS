/* Production kernel model for the real CiukiOS services backend. Host mmap
 * mirrors the fixture's fake physical RAM so native H0 C can dereference VAs.
 * This never executes PE code and is not i386 ABI or QEMU evidence.
 * SPDX-License-Identifier: GPL-2.0-only */
#define w32_pe_validate w32_kernel_pe_validate
#define W32_KERNEL_BRIDGE
#include "kernel_core_test.c"
#undef main
#include <stdarg.h>
extern void *mmap(void *, size_t, int, int, int, long);
extern int munmap(void *, size_t);
static struct process *model_process;
static struct proc_thread *model_thread;
static uint32_t record_va;
static uint32_t mirrors[HOST_PAGES];
static unsigned mirror_count, peak_pages, peak_cow;
static unsigned operations[0x109];
static char model_root[1024];
/* The kernel logs use protected klog in production; print its exact payload
 * in this fixture too. The inherited harness's silent sink is renamed. */
void klog(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
}
static int disk_read(void *cookie, uint32_t off, void *dst, uint32_t n) {
    FILE *f = cookie;
    if (fseek(f, (long)off, SEEK_SET))
        return -EIO;
    return fread(dst, 1, n, f) == n ? 0 : -EIO;
}
static void disk_close(void *cookie) { fclose(cookie); }
static bool observing_search;
static int search_choice;
static unsigned search_seen;
static const char *search_paths[]={"/EXE/target.exe","/target.exe","/WINDOWS/SYSTEM/target.exe","/WINDOWS/target.exe","/TOOLS/target.exe"};
static int disk_open(void *cwd, const char *path, struct ciuki_file *file) {
    (void)cwd;
    if(observing_search && strstr(path,"target.exe")) {
        CHECK(search_seen<5&&!strcmp(path,search_paths[search_seen]));
        unsigned at=search_seen++;
        if(search_choice==-2)return -EACCES;
        if(at!=(unsigned)search_choice)return -ENOENT;
        path="/console.exe";
    }
    /* VFAT's 255 Unicode characters can exceed Linux NAME_MAX bytes. Keep
     * the logical path intact in the loader; adapt only this backing open. */
    if(strlen(path)>255 && !strncmp(path,"/caf\xc3\xa9/",7))path="/exports.dll";
    if (!strcmp(path,"/access-denied.exe")) return -EACCES;
    char name[2048];
    if (strlen(model_root) + strlen(path) + 2 >= sizeof(name))
        return -ENOENT;
    snprintf(name, sizeof(name), "%s/%s", model_root, path[0] == '/' ? path + 1 : path);
    FILE *f = fopen(name, "rb");
    if (!f)
        return -ENOENT;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return -EIO;
    }
    long size = ftell(f);
    if (size < 0 || size > 512 * 1024 * 1024) {
        fclose(f);
        return -ENOEXEC;
    }
    *file = (struct ciuki_file){f, (uint32_t)size, disk_read, disk_close};
    return 0;
}
static bool live_page(uint32_t va) {
    const struct ua_extent *e = ua_find(model_process->memory, va);
    return e && e->w32_state == W32_MEM_COMMIT;
}
static void sync_to_kernel(void) {
    uint8_t bytes[PAGE_SIZE];
    for (unsigned i = 0; i < mirror_count; i++) {
        uint32_t va = mirrors[i];
        struct ua_extent *e = ua_find(model_process->memory, va);
        if (!e || !(e->prot & PROT_WRITE) || !live_page(va))
            continue;
        CHECK(!ua_read(model_process->memory, bytes, va, PAGE_SIZE));
        if (memcmp(bytes, (void *)(uintptr_t)va, PAGE_SIZE))
            CHECK(!ua_write(model_process->memory, va, (void *)(uintptr_t)va, PAGE_SIZE));
    }
}
static void sync_from_kernel(void) {
    for (unsigned i = 0; i < mirror_count;) {
        uint32_t va = mirrors[i];
        if (!live_page(va)) {
            CHECK(!munmap((void *)(uintptr_t)va, PAGE_SIZE));
            mirrors[i] = mirrors[--mirror_count];
        } else
            i++;
    }
    for (struct ua_extent *e = model_process->memory->head; e; e = e->next) {
        if (e->w32_state != W32_MEM_COMMIT)
            continue;
        for (uint32_t va = e->base; va < e->end; va += PAGE_SIZE) {
            bool present = false;
            for (unsigned i = 0; i < mirror_count; i++)
                if (mirrors[i] == va) {
                    present = true;
                    break;
                }
            if (!present) {
                void *m = mmap((void *)(uintptr_t)va, PAGE_SIZE, 3, 0x100022, -1, 0);
                CHECK(m == (void *)(uintptr_t)va && mirror_count < HOST_PAGES);
                mirrors[mirror_count++] = va;
                memset(m, 0, PAGE_SIZE);
            }
            if (e->prot & PROT_READ)
                CHECK(!ua_read(model_process->memory, (void *)(uintptr_t)va, va, PAGE_SIZE));
        }
    }
    if (pages_used > peak_pages)
        peak_pages = pages_used;
    if (w32_accounts.cow_credits > peak_cow)
        peak_cow = w32_accounts.cow_credits;
}
int32_t w32_kernel_model_call(void *record, uint32_t size) {
    if (!size) {
        fprintf(stderr, "TERMINATE %u\n", *(uint32_t *)record);
        exit(90);
    }
    sync_to_kernel();
    CHECK(size <= 240);
    CHECK(!ua_write(model_process->memory, record_va, record, size));
    uint32_t op = ((struct ciuki_w32_header *)record)->operation;
    if (op < 0x109)
        operations[op]++;
    struct trap_frame tf = {.ebx = record_va, .ecx = size};
    int32_t status = w32_syscall(&tf);
    if (!status)
        CHECK(!ua_read(model_process->memory, record, record_va, size));
    sync_from_kernel();
    return status;
}
void w32_kernel_model_init(const char *root, uint32_t *pid, uint32_t *tid) {
    ram = calloc(HOST_PAGES, PAGE_SIZE);
    CHECK(ram);
    g_current = &controller;
    controller.state = T_RUNNING;
    proc_init();
    CHECK(!proc_prepare(proc_supervisor(), &model_process));
    model_process->personality = PROC_WIN32;
    model_process->memory->win32 = true;
    CHECK(!w32_process_init(model_process));
    CHECK(!proc_thread_prepare(model_process, 0x400000, 0, 0, 0, 0, 0, true, &model_thread));
    model_process->state = PROC_LIVE;
    model_process->published = true;
    g_current = model_thread->task;
    g_current->state = T_RUNNING;
    struct ciuki_w32_vm a = {.address = 0x6fff0000,
                             .bytes = 4096,
                             .flags = W32_MEM_RESERVE | W32_MEM_COMMIT,
                             .protect = W32_PAGE_READWRITE};
    CHECK(!w32_vm_alloc(model_process->memory, &a));
    record_va = a.address;
    static const struct ciuki_file_ops ops = {disk_open};
    proc_set_file_ops(&ops);
    CHECK(strlen(root) < sizeof(model_root));
    strcpy(model_root, root);
    *pid = model_process->pid;
    *tid = model_thread->tid;
    sync_from_kernel();
}
void w32_kernel_model_finish(void) {
    sync_to_kernel();
    printf("KERNEL_MODEL peak_pages=%u peak_bytes=%u cow_credits=%u loader_calls=%u vm_calls=%u "
           "protect_calls=%u word_calls=%u\n",
           peak_pages, peak_pages * PAGE_SIZE, peak_cow, operations[W32_LOADER],
           operations[W32_VM_ALLOC], operations[W32_VM_PROTECT], operations[W32_WORD]);
    g_current = &controller;
    proc_discard(model_process);
    for (unsigned i = 0; i < mirror_count; i++)
        CHECK(!munmap((void *)(uintptr_t)mirrors[i], PAGE_SIZE));
    mirror_count = 0;
    proc_set_file_ops(0);
    CHECK(!pages_used && !w32_accounts.objects && !w32_accounts.handles && !w32_accounts.views &&
          !w32_accounts.cow_credits);
    free(ram);
    puts("KERNEL_MODEL teardown PASS");
}

static uint32_t scheduled_word;
static void release_model_word(void) {
    uint32_t zero = 0;
    CHECK(!ua_write(model_process->memory, scheduled_word, &zero, 4));
    CHECK(proc_wake_word(scheduled_word, 1) == 1);
    on_schedule = 0;
}
void w32_kernel_model_release_word(uint32_t address) {
    scheduled_word = address;
    on_schedule = release_model_word;
}

void w32_kernel_model_spawn_test(void){
    struct ciuki_w32_vm scratch={.bytes=4096,.flags=W32_MEM_RESERVE|W32_MEM_COMMIT,.protect=W32_PAGE_READWRITE};
    CHECK(!w32_vm_alloc(model_process->memory,&scratch));
    CHECK(!ua_write(model_process->memory,scratch.address,"/console",9));
    CHECK(!ua_write(model_process->memory,scratch.address+256,"console.exe /unchanged",23));
    CHECK(!ua_write(model_process->memory,scratch.address+512,"A=B\0\0",5));
    struct ciuki_w32_startup startup={.x=7,.width=640,.flags=1,.show=3,.title=scratch.address+1024};
    CHECK(!ua_write(model_process->memory,scratch.address+768,&startup,sizeof(startup)));
    CHECK(!ua_write(model_process->memory,scratch.address+1024,"Fixture title",14));
    struct ciuki_w32_spawn_args a={.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=5,.flags=4|0x80,.startup=scratch.address+768};
    CHECK(!w32_process_call(model_thread,&a));
    CHECK(!a.error&&a.process_handle&&a.thread_handle&&a.pid&&a.tid);
    struct process *child=proc_find(a.pid);CHECK(child&&child->published&&child->personality==PROC_WIN32);
    struct proc_thread *main=proc_thread_find(child,a.tid);CHECK(main&&main->w32->suspend==1);
    CHECK(child->w32->priority_class==0x80&&main->task->base_priority==proc_priority_win32(0x80,0,false));
    struct ciuki_w32_context_args resume={.action=W32_CTX_RESUME,.thread=a.thread_handle};
    CHECK(!w32_context_call(model_thread,&resume)&&!resume.error&&resume.result==1&&!main->w32->suspend);
    struct ciuki_w32_context_args remote_context={.action=W32_CTX_GET,.thread=a.thread_handle};
    CHECK(!w32_context_call(model_thread,&remote_context)&&remote_context.error==6);
    struct ciuki_w32_system_args terminate={.action=W32_S_TERMINATE_THREAD,.handle=a.thread_handle,.value=0xfeed};
    CHECK(!w32_system_call(model_thread,&terminate)&&!terminate.error&&main->w32->terminate_requested&&main->w32->terminate_code==0xfeed);
    uint32_t parameters=0;
    struct ciuki_w32_parameters_v3 v;
    CHECK(!ua_read(child->memory,&parameters,child->w32_peb+0x10,4));
    CHECK(!ua_read(child->memory,&v,parameters,sizeof(v)));
    CHECK(v.inherited.core.version==3&&v.inherited.core.size==sizeof(v));
    CHECK(v.startup.x==7&&v.startup.width==640&&v.startup.show==3&&v.startup.flags==1);
    char title[14];CHECK(!ua_read(child->memory,title,v.startup.title,sizeof(title))&&!strcmp(title,"Fixture title"));
    CHECK(!w32_handle_close(model_process,a.process_handle)&&!w32_handle_close(model_process,a.thread_handle));
    proc_discard(child);
    startup.title=0xdeadc000;
    CHECK(!ua_write(model_process->memory,scratch.address+768,&startup,sizeof(startup)));
    a=(struct ciuki_w32_spawn_args){.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=5,.startup=scratch.address+768};
    CHECK(!w32_process_call(model_thread,&a)&&a.error==998&&!a.pid&&!a.process_handle&&!a.thread_handle);
    a=(struct ciuki_w32_spawn_args){.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=5,.flags=0x100};
    CHECK(!w32_process_call(model_thread,&a)&&!a.error&&a.pid);
    child=proc_find(a.pid);CHECK(child&&child->w32->priority_class==0x80);
    main=proc_thread_find(child,a.tid);CHECK(main&&main->task->base_priority==proc_priority_win32(0x80,0,false));
    struct ciuki_w32_system_args priority={.action=W32_S_PROCESS_PRIORITY,.handle=a.process_handle,.value=0x100};
    CHECK(!w32_system_call(model_thread,&priority)&&!priority.error&&child->w32->priority_class==0x80);
    CHECK(!w32_handle_close(model_process,a.process_handle)&&!w32_handle_close(model_process,a.thread_handle));proc_discard(child);
    a=(struct ciuki_w32_spawn_args){.flags=0x20|0x40};
    CHECK(!w32_process_call(model_thread,&a)&&a.error==87&&!a.pid);
    CHECK(!ua_write(model_process->memory,scratch.address,"/not-present.exe",17));
    a=(struct ciuki_w32_spawn_args){.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=5};
    CHECK(!w32_process_call(model_thread,&a)&&a.error==2&&!a.pid&&!a.process_handle&&!a.thread_handle);
    CHECK(!ua_write(model_process->memory,scratch.address,"/access-denied.exe",19));
    a=(struct ciuki_w32_spawn_args){.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=5};
    CHECK(!w32_process_call(model_thread,&a)&&a.error==5&&!a.pid&&!a.process_handle&&!a.thread_handle);
    struct ciuki_w32_vm release={.address=scratch.address,.flags=W32_MEM_RELEASE};CHECK(!w32_vm_free(model_process->memory,&release));
    sync_from_kernel();puts("PASS kernel32 kernel CreateProcess search suspended handles rollback");
}

static struct process *race_process;
static void close_during_allocation(void){race_process->w32->closing=true;}
static void exit_during_spawn(void){
    on_preempt_point=0;model_process->state=PROC_STOPPING;model_process->w32->closing=true;
}
void w32_kernel_model_spawn_edges(void){
    struct ciuki_w32_vm scratch={.bytes=4096,.flags=W32_MEM_RESERVE|W32_MEM_COMMIT,.protect=W32_PAGE_READWRITE};
    CHECK(!w32_vm_alloc(model_process->memory,&scratch));
    CHECK(!ua_write(model_process->memory,scratch.address,"target",7));
    CHECK(!ua_write(model_process->memory,scratch.address+256,"target /verbatim",17));
    const char env[]="PATH=C:\\TOOLS\0A=B\0\0";
    CHECK(!ua_write(model_process->memory,scratch.address+512,env,sizeof(env)));
    strcpy(model_process->w32_source_name,"C:\\EXE\\caller.exe");
    for(int choice=0;choice<5;choice++){
        search_choice=choice;search_seen=0;observing_search=true;
        struct ciuki_w32_spawn_args a={.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=sizeof(env)};
        CHECK(!w32_process_call(model_thread,&a)&&!a.error&&a.pid&&search_seen==(unsigned)choice+1);
        observing_search=false;struct process *child=proc_find(a.pid);CHECK(child&&child->published);
        CHECK(!w32_handle_close(model_process,a.process_handle)&&!w32_handle_close(model_process,a.thread_handle));proc_discard(child);
    }
    search_choice=-2;search_seen=0;observing_search=true;
    struct ciuki_w32_spawn_args a={.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=sizeof(env)};
    CHECK(!w32_process_call(model_thread,&a)&&a.error==5&&!a.pid&&search_seen==1);observing_search=false;
    CHECK(!ua_write(model_process->memory,scratch.address,"/native-child.exe",18));
    a=(struct ciuki_w32_spawn_args){.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=sizeof(env),.flags=4};
    CHECK(!w32_process_call(model_thread,&a)&&a.error==120&&!a.pid);
    CHECK(!ua_write(model_process->memory,scratch.address,"/console.exe",13));
    unsigned pages=pages_used,objects=w32_accounts.objects,handles=w32_accounts.handles,blocks=heap_blocks,failures=0;
    for(int fail=0;fail<100;fail++){
        a=(struct ciuki_w32_spawn_args){.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=sizeof(env)};
        alloc_fail=fail;CHECK(!w32_process_call(model_thread,&a));alloc_fail=-1;
        if(!a.error){struct process *child=proc_find(a.pid);CHECK(child);CHECK(!w32_handle_close(model_process,a.process_handle)&&!w32_handle_close(model_process,a.thread_handle));proc_discard(child);}
        else {failures++;CHECK(!a.pid&&!a.process_handle&&!a.thread_handle);}
        CHECK(pages_used==pages&&w32_accounts.objects==objects&&w32_accounts.handles==handles&&heap_blocks==blocks);
    }
    CHECK(failures>20);
    on_preempt_point=exit_during_spawn;
    a=(struct ciuki_w32_spawn_args){.image=scratch.address,.command=scratch.address+256,.environment=scratch.address+512,.environment_bytes=sizeof(env)};
    CHECK(!w32_process_call(model_thread,&a)&&a.error&&!a.pid&&!a.process_handle&&!a.thread_handle&&!on_preempt_point);
    model_process->state=PROC_LIVE;model_process->w32->closing=false;
    CHECK(pages_used==pages&&w32_accounts.objects==objects&&w32_accounts.handles==handles&&heap_blocks==blocks);
    CHECK(!proc_prepare(model_process,&race_process)&&!w32_process_init(race_process));
    struct w32_object *event=w32_object_new(W32_OBJ_EVENT);CHECK(event);uint32_t handle=0xfeed;
    w32_allocation_hook=close_during_allocation;
    CHECK(w32_handle_install(race_process,event,~0u,false,&handle)==5&&handle==0xfeed&&!race_process->w32->handles&&!w32_allocation_hook);
    w32_release(event);proc_discard(race_process);race_process=0;
    CHECK(pages_used==pages&&w32_accounts.objects==objects&&w32_accounts.handles==handles&&heap_blocks==blocks);
    struct ciuki_w32_vm release={.address=scratch.address,.flags=W32_MEM_RELEASE};CHECK(!w32_vm_free(model_process->memory,&release));
    sync_from_kernel();printf("PASS kernel32 search precedence=5 access-denied stop ELF suspension=120 spawn failures=%u handle/exit races rollback\n",failures);
}
