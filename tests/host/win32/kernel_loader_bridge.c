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
static int disk_open(void *cwd, const char *path, struct ciuki_file *file) {
    (void)cwd;
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
