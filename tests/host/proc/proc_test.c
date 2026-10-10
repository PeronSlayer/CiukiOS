/* Production F2 process code with fake physical RAM and scheduling only.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/kernel.h>
#include <ciuki/process.h>

#define CIUKI_CPU_H
#define KERNEL_VBASE CIUKI_MAIN_STACK_LIMIT
#define HOST_PAGES 11000u
static uint8_t *ram;
static bool allocated[HOST_PAGES];
static unsigned pages_used, heap_blocks, checks;
static int alloc_fail = -1;
static uint32_t host_flags = CIUKI_INITIAL_EFLAGS, host_cr3;
static unsigned invlpgs;
static uint32_t tls_base;
static void (*on_schedule)(void);
static jmp_buf exited;
static bool expect_exit;
static unsigned trap_calls;
char copy_user_fault_start[8], copy_user_fault_end[1], copy_user_fixup[1];
void trap_dispatch(struct trap_frame *tf) { (void)tf; trap_calls++; }
struct task *g_current;
uint32_t g_task_count, g_user_mappings;
volatile uint64_t g_ticks;
volatile bool g_need_resched;

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define P2V(p) ((void *)(ram + (p)))
#define V2P(v) ((uint32_t)((uint8_t *)(v) - ram))
static uint32_t irq_save(void) { uint32_t f = host_flags; host_flags &= ~0x200u; return f; }
static void irq_restore(uint32_t f) { host_flags = f; }
static uint32_t read_cr3(void) { return host_cr3; }
static void invlpg(uint32_t va) { (void)va; invlpgs++; }
static bool fail_alloc(void) { if (alloc_fail < 0) return false; if (!alloc_fail) return true; alloc_fail--; return false; }

uint32_t pmm_alloc(void)
{
    if (fail_alloc()) return 0;
    for (unsigned i = 1; i < HOST_PAGES; i++)
        if (!allocated[i]) { allocated[i] = true; pages_used++; memset(P2V(i * PAGE_SIZE), 0xa5, PAGE_SIZE); return i * PAGE_SIZE; }
    return 0;
}
void pmm_free(uint32_t p) { CHECK(p && !(p & (PAGE_SIZE - 1)) && p / PAGE_SIZE < HOST_PAGES && allocated[p / PAGE_SIZE]); allocated[p / PAGE_SIZE] = false; pages_used--; }
void *kmalloc(size_t n) { if (fail_alloc()) return 0; void *p = malloc(n); if (p) heap_blocks++; return p; }
void *kzalloc(size_t n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }
void kfree(void *p) { if (p) { CHECK(heap_blocks); heap_blocks--; free(p); } }
void panic(const char *fmt, ...) { fprintf(stderr, "unexpected panic: %s\n", fmt); abort(); }
int as_create(struct aspace *as) { as->pd_phys = pmm_alloc(); if (!as->pd_phys) return -ENOMEM; memset(P2V(as->pd_phys), 0, PAGE_SIZE); return 0; }
void as_destroy(struct aspace *as) { CHECK(!as->pages && !as->tables); if (as->pd_phys) pmm_free(as->pd_phys); memset(as, 0, sizeof(*as)); }
void kwait_init(struct kwait *q) { q->head = 0; }
void kwait_wake_all(struct kwait *q) { (void)q; }
void fpu_task_release(struct task *t) { t->fpu_valid = false; }
void arch_tls_switch(uint32_t base, bool native) { tls_base = native ? base : 0; }
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    (void)name; (void)fn; (void)arg;
    struct task *t = kzalloc(sizeof(*t));
    if (t) { t->state = T_BLOCKED; t->prio = prio; g_task_count++; }
    return t;
}
struct task *task_create_native(struct process *p, uint32_t entry, uint32_t esp)
{
    (void)entry;
    struct task *t = task_create_kernel("native", 0, 0, P_NORMAL);
    if (t) { t->as = p->memory->as; t->user = true; t->saved_esp = esp; }
    return t;
}
void task_native_frame(struct task *t, uint32_t esp) { t->saved_esp = esp; }
void task_start(struct task *t) { CHECK(t->state == T_BLOCKED); t->state = T_READY; }
bool task_alive(const struct task *t) { return t->state != T_ZOMBIE && t->state != T_DEAD; }
void task_kill(struct task *t, int code) { if (!task_alive(t)) return; proc_task_exiting(t, code); t->exit_code = code; t->state = T_ZOMBIE; }
void task_reap(struct task *t) { CHECK(t != g_current && t->state == T_ZOMBIE && !t->as.pd_phys); g_task_count--; kfree(t); }
void task_exit(int code) { CHECK(expect_exit); task_kill(g_current, code); longjmp(exited, 1); }
void schedule(void)
{
    CHECK(!(host_flags & 0x200));
    g_ticks++;
    if (on_schedule) on_schedule();
    g_current->state = T_RUNNING;
}
void task_sleep_ms(uint32_t ms) { uint32_t f = irq_save(); g_ticks += ms ? ms - 1 : 0; g_current->state = T_BLOCKED; schedule(); irq_restore(f); }
int copy_user(void *dst, const void *src, uint32_t bytes)
{
    struct process *p = proc_current();
    if ((uintptr_t)src <= UINT32_MAX)
        return ua_read(p->memory, dst, (uint32_t)(uintptr_t)src, bytes);
    if ((uintptr_t)dst <= UINT32_MAX)
        return ua_write(p->memory, (uint32_t)(uintptr_t)dst, src, bytes);
    return -EFAULT;
}

#include "../../../src/kernel/proc/uaddr.c"
#include "../../../src/kernel/proc/elf.c"
#include "../../../src/kernel/proc/waitword.c"
#include "../../../src/kernel/proc/process.c"
#include "../../../src/kernel/proc/thread.c"
#include "../../../src/kernel/proc/spawn.c"
#include "../../../src/kernel/proc/syscalls_proc.c"

static uint8_t fixture[4 * PAGE_SIZE];
static int fixture_read(void *cookie, uint32_t off, void *dst, uint32_t n)
{
    (void)cookie;
    /* Sparse zero suffix permits a 32 MiB file boundary without a copy. */
    if (off >= sizeof(fixture)) { memset(dst, 0, n); return 0; }
    CHECK(n <= sizeof(fixture) - off);
    memcpy(dst, fixture + off, n);
    return 0;
}
static struct ciuki_file fixture_file(void)
{
    memset(fixture, 0, sizeof(fixture));
    struct elf_header h = { .ident = { 0x7f, 'E', 'L', 'F', 1, 1, 1 }, .type = 2,
        .machine = 3, .version = 1, .entry = CIUKI_IMAGE_BASE, .phoff = sizeof(h),
        .ehsize = sizeof(h), .phentsize = sizeof(struct elf_phdr), .phnum = 2 };
    struct elf_phdr ph[] = {
        { .type = 1, .offset = PAGE_SIZE, .va = CIUKI_IMAGE_BASE, .filesz = 127, .memsz = PAGE_SIZE, .flags = 5, .align = PAGE_SIZE },
        { .type = 1, .offset = 2 * PAGE_SIZE, .va = CIUKI_IMAGE_BASE + PAGE_SIZE, .filesz = 13, .memsz = PAGE_SIZE + 3, .flags = 6, .align = PAGE_SIZE }
    };
    memcpy(fixture, &h, sizeof(h)); memcpy(fixture + sizeof(h), ph, sizeof(ph));
    memset(fixture + PAGE_SIZE, 0x37, 127); memset(fixture + 2 * PAGE_SIZE, 0x71, 13);
    return (struct ciuki_file){ .bytes = sizeof(fixture), .read = fixture_read };
}
static struct elf_header *eh(void) { return (struct elf_header *)fixture; }
static struct elf_phdr *ph(unsigned n) { return (struct elf_phdr *)(fixture + sizeof(struct elf_header)) + n; }
static void test_elf(void)
{
    struct ciuki_file f = fixture_file(); struct elf_image im;
    CHECK(elf_validate(&f, &im) == 0 && im.count == 2 && im.bytes == 3 * PAGE_SIZE);
#define REJECT(change) do { f = fixture_file(); change; CHECK(elf_validate(&f, &im) == -ENOEXEC); } while (0)
    for (unsigned n = 0; n < sizeof(struct elf_header); n++) { f = fixture_file(); f.bytes = n; CHECK(elf_validate(&f, &im) == -ENOEXEC); }
    REJECT(eh()->ident[0] = 0); REJECT(eh()->ident[4] = 2); REJECT(eh()->ident[5] = 2);
    REJECT(eh()->ident[6] = 0); REJECT(eh()->ident[7] = 3); REJECT(eh()->ident[8] = 1);
    REJECT(eh()->machine = 62); REJECT(eh()->version = 0); REJECT(eh()->type = 3);
    REJECT(eh()->flags = 1); REJECT(eh()->ehsize--); REJECT(eh()->phentsize--);
    REJECT(eh()->phnum = 0); REJECT(eh()->phnum = CIUKI_ELF_PHDR_MAX + 1);
    REJECT(eh()->phoff = UINT32_MAX - 8); REJECT(f.bytes = sizeof(struct elf_header) + sizeof(struct elf_phdr));
    for (unsigned type = 2; type <= 8; type++) if (type != 4 && type != 6) { REJECT(ph(1)->type = type); }
    REJECT(ph(1)->type = 0x12345678); REJECT(ph(1)->type = 0x6474e551; ph(1)->flags = 1);
    REJECT(ph(1)->offset = UINT32_MAX - 1); REJECT(ph(1)->filesz = ph(1)->memsz + 1);
    REJECT(ph(1)->va = UINT32_MAX - 8); REJECT(ph(1)->align = 1); REJECT(ph(1)->va++);
    REJECT(ph(1)->offset++); REJECT(ph(1)->flags = 2); REJECT(ph(1)->flags = 0x80 | 6);
    REJECT(ph(1)->flags = 7); REJECT(ph(1)->va = CIUKI_IMAGE_BASE - PAGE_SIZE);
    REJECT(ph(1)->va = CIUKI_IMAGE_LIMIT); REJECT(ph(1)->va = CIUKI_IMAGE_LIMIT - PAGE_SIZE);
    REJECT(ph(1)->va = CIUKI_IMAGE_BASE); REJECT(eh()->entry = CIUKI_IMAGE_BASE + 127);
    REJECT(eh()->entry = ph(1)->va); REJECT(f.bytes = CIUKI_ELF_BYTES_MAX + 1);
    REJECT(ph(0)->memsz = CIUKI_ELF_BYTES_MAX + 1);
    REJECT(eh()->shoff = UINT32_MAX - 8; eh()->shnum = 1; eh()->shentsize = sizeof(struct elf_shdr));
    REJECT(eh()->shoff = 256; eh()->shnum = 1; eh()->shentsize = sizeof(struct elf_shdr); ((struct elf_shdr *)(fixture + 256))->type = 6);
    REJECT(eh()->shoff = 256; eh()->shnum = 1; eh()->shentsize = sizeof(struct elf_shdr); ((struct elf_shdr *)(fixture + 256))->type = 1; ((struct elf_shdr *)(fixture + 256))->offset = UINT32_MAX; ((struct elf_shdr *)(fixture + 256))->size = 4);
    REJECT(eh()->shnum = 1); REJECT(eh()->shoff = 256; eh()->shnum = 1; eh()->shentsize = 39);
    REJECT(eh()->shoff = 256; eh()->shnum = 1; eh()->shentsize = 40; eh()->shstrndx = 1);
    f = fixture_file(); eh()->phnum = 1; ph(0)->memsz = CIUKI_ELF_BYTES_MAX; f.bytes = CIUKI_ELF_BYTES_MAX;
    CHECK(elf_validate(&f, &im) == 0 && im.bytes == CIUKI_ELF_BYTES_MAX);
    ph(0)->memsz++; CHECK(elf_validate(&f, &im) == -ENOEXEC);
    f = fixture_file(); ph(1)->type = 0; ph(1)->offset = UINT32_MAX; CHECK(elf_validate(&f, &im) == 0);
    f = fixture_file(); ph(1)->type = 4; CHECK(elf_validate(&f, &im) == 0);
    f = fixture_file(); eh()->phnum = 16; CHECK(elf_validate(&f, &im) == 0);
    f = fixture_file(); CHECK(elf_validate(&f, &im) == 0);
    struct uaddr u; CHECK(ua_init(&u, &u) == 0); CHECK(elf_load(&f, &im, &u) == 0);
    uint8_t b[PAGE_SIZE]; CHECK(ua_read(&u, b, CIUKI_IMAGE_BASE + PAGE_SIZE, PAGE_SIZE) == 0);
    for (unsigned i = 0; i < PAGE_SIZE; i++) CHECK(b[i] == (i < 13 ? 0x71 : 0));
    CHECK(ua_read(&u, b, CIUKI_IMAGE_BASE + 2 * PAGE_SIZE, PAGE_SIZE) == 0);
    for (unsigned i = 0; i < PAGE_SIZE; i++) CHECK(!b[i]);
    ua_destroy(&u);
    puts("proc ELF: valid, every header/segment rejection, bounds, BSS/padding PASS");
#undef REJECT
}
static struct ciuki_mmap_args map_args(uint32_t bytes, uint32_t prot)
{
    return (struct ciuki_mmap_args){ .size = sizeof(struct ciuki_mmap_args), .length = bytes,
        .prot = prot, .flags = MAP_PRIVATE | MAP_ANONYMOUS, .fd = -1 };
}
static uint32_t get_word(struct uaddr *u, uint32_t va) { uint32_t v; CHECK(ua_read(u, &v, va, sizeof(v)) == 0); return v; }
static void put_word(struct uaddr *u, uint32_t va, uint32_t v) { CHECK(ua_write(u, va, &v, sizeof(v)) == 0); }
static void test_arena(void)
{
    unsigned base_pages = pages_used, base_heap = heap_blocks;
    struct uaddr u; CHECK(ua_init(&u, &u) == 0);
    struct ciuki_mmap_args a = map_args(3 * PAGE_SIZE - 1, PROT_READ | PROT_WRITE);
    CHECK(ua_mmap(&u, &a) == CIUKI_MMAP_BASE);
    CHECK(u.backing == 3 && u.arena_extents == 1);
    put_word(&u, CIUKI_MMAP_BASE, 0xdeadbeef); put_word(&u, CIUKI_MMAP_BASE + 2 * PAGE_SIZE, 0x12345678);
    a.hint = CIUKI_MMAP_BASE + 17; a.length = PAGE_SIZE;
    CHECK(ua_mmap(&u, &a) == CIUKI_MMAP_BASE + 3 * PAGE_SIZE);
    CHECK(ua_munmap(&u, CIUKI_MMAP_BASE + PAGE_SIZE, 1) == 0 && u.arena_extents == 3);
    CHECK(get_word(&u, CIUKI_MMAP_BASE) == 0xdeadbeef && get_word(&u, CIUKI_MMAP_BASE + 2 * PAGE_SIZE) == 0x12345678);
    a.hint = 0; CHECK(ua_mmap(&u, &a) == CIUKI_MMAP_BASE + PAGE_SIZE);
    CHECK(get_word(&u, CIUKI_MMAP_BASE + PAGE_SIZE) == 0);
    CHECK(ua_munmap(&u, CIUKI_MMAP_BASE + 100 * PAGE_SIZE, PAGE_SIZE) == 0);
    CHECK(ua_mprotect(&u, CIUKI_MMAP_BASE, 5 * PAGE_SIZE, PROT_NONE) == -ENOMEM);
    const uint32_t prot[] = { PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC };
    for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 4; j++) {
        CHECK(ua_mprotect(&u, CIUKI_MMAP_BASE, PAGE_SIZE, prot[i]) == 0);
        CHECK(ua_mprotect(&u, CIUKI_MMAP_BASE, PAGE_SIZE, prot[j]) == 0);
        CHECK(get_word(&u, CIUKI_MMAP_BASE) == 0xdeadbeef);
        CHECK(ua_find(&u, CIUKI_MMAP_BASE)->maximum == (PROT_READ | PROT_WRITE | PROT_EXEC));
        CHECK(ua_range(&u, CIUKI_MMAP_BASE, 4, PROT_READ) == (prot[j] != PROT_NONE));
        CHECK(ua_range(&u, CIUKI_MMAP_BASE, 4, PROT_WRITE) == !!(prot[j] & PROT_WRITE));
    }
    CHECK(ua_mprotect(&u, CIUKI_MMAP_BASE, PAGE_SIZE, 7) == -EACCES);
    CHECK(ua_mprotect(&u, CIUKI_MMAP_BASE, PAGE_SIZE, PROT_WRITE) == -EACCES);
    CHECK(ua_mprotect(&u, CIUKI_MMAP_BASE, PAGE_SIZE, 8) == -EINVAL);
    CHECK(ua_munmap(&u, CIUKI_MMAP_BASE + 1, PAGE_SIZE) == -EINVAL);
    CHECK(ua_munmap(&u, CIUKI_MMAP_BASE, UINT32_MAX) == -EINVAL);
    CHECK(ua_mprotect(&u, CIUKI_MAIN_STACK_BASE, PAGE_SIZE, PROT_READ) == -EBUSY);
    a = map_args(PAGE_SIZE, PROT_NONE); a.hint = 0x90000017;
    CHECK((uint32_t)ua_mmap(&u, &a) == 0x90000000); CHECK(u.backing == 4);
    CHECK(ua_mprotect(&u, 0x90000000, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0);
    CHECK(get_word(&u, 0x90000000) == 0);
    struct ua_pin pin; CHECK(ua_pin(&u, &pin, 0x90000000, 4, true) == 0);
    CHECK(ua_munmap(&u, 0x90000000, PAGE_SIZE) == -EBUSY);
    CHECK(ua_mprotect(&u, 0x90000000, PAGE_SIZE, PROT_NONE) == -EBUSY);
    ua_unpin(&u, &pin);
    a.flags |= MAP_FIXED; CHECK(ua_mmap(&u, &a) == -EOPNOTSUPP);
    a.flags = MAP_SHARED | MAP_ANONYMOUS; CHECK(ua_mmap(&u, &a) == -EOPNOTSUPP);
    a.flags = MAP_PRIVATE; CHECK(ua_mmap(&u, &a) == -EOPNOTSUPP);
    a.flags = MAP_PRIVATE | MAP_SHARED; CHECK(ua_mmap(&u, &a) == -EINVAL);
    a = map_args(0, PROT_READ); CHECK(ua_mmap(&u, &a) == -EINVAL);
    a.length = UINT32_MAX; CHECK(ua_mmap(&u, &a) == -EINVAL);
    a = map_args(PAGE_SIZE, PROT_READ); a.fd = 0; CHECK(ua_mmap(&u, &a) == -EINVAL);
    a.fd = -1; a.offset = 1; CHECK(ua_mmap(&u, &a) == -EINVAL);
    ua_destroy(&u); CHECK(pages_used == base_pages && heap_blocks == base_heap);

    /* Fail each reservation allocation, checking data, metadata and tables. */
    for (int failure = 0; failure < 7; failure++) {
        CHECK(ua_init(&u, &u) == 0); unsigned before = pages_used;
        a = map_args(4 * PAGE_SIZE, PROT_READ | PROT_WRITE);
        alloc_fail = failure; int32_t r = ua_mmap(&u, &a); alloc_fail = -1;
        if (r < 0) CHECK(!u.extents && !u.backing && !u.as.tables && pages_used == before);
        else CHECK(r == CIUKI_MMAP_BASE);
        ua_destroy(&u); CHECK(pages_used == base_pages && heap_blocks == base_heap);
    }
    CHECK(ua_init(&u, &u) == 0); a = map_args(3 * PAGE_SIZE, PROT_NONE);
    CHECK(ua_mmap(&u, &a) == CIUKI_MMAP_BASE);
    for (int failure = 0; failure < 5; failure++) {
        unsigned before = pages_used;
        alloc_fail = failure; int r = ua_mprotect(&u, CIUKI_MMAP_BASE + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE); alloc_fail = -1;
        if (r) CHECK(r == -ENOMEM && u.extents == 1 && !u.backing && pages_used == before);
        else { CHECK(u.extents == 3); break; }
    }
    ua_destroy(&u);
    CHECK(ua_init(&u, &u) == 0); a = map_args(3 * PAGE_SIZE, PROT_NONE);
    CHECK(ua_mmap(&u, &a) == CIUKI_MMAP_BASE);
    for (unsigned i = 1; i < CIUKI_MAPPING_MAX; i++) { a = map_args(PAGE_SIZE, PROT_NONE); CHECK(ua_mmap(&u, &a) > 0); }
    CHECK(ua_mmap(&u, &a) == -ENOMEM);
    CHECK(ua_munmap(&u, CIUKI_MMAP_BASE + PAGE_SIZE, PAGE_SIZE) == -ENOMEM && u.arena_extents == CIUKI_MAPPING_MAX);
    /* Removing a complete extent makes the middle split fit exactly. */
    CHECK(ua_munmap(&u, CIUKI_MMAP_BASE + 3 * PAGE_SIZE, PAGE_SIZE) == 0);
    CHECK(ua_munmap(&u, CIUKI_MMAP_BASE + PAGE_SIZE, PAGE_SIZE) == 0 && u.arena_extents == CIUKI_MAPPING_MAX);
    ua_destroy(&u);
    CHECK(ua_init(&u, &u) == 0); u.heap_base = u.brk = CIUKI_IMAGE_BASE + 3 * PAGE_SIZE;
    CHECK(ua_brk(&u, 0) == (int32_t)u.heap_base);
    CHECK(ua_brk(&u, u.heap_base + PAGE_SIZE + 9) == (int32_t)(u.heap_base + PAGE_SIZE + 9));
    put_word(&u, u.heap_base, 123); put_word(&u, u.heap_base + PAGE_SIZE, UINT32_MAX);
    CHECK(ua_brk(&u, u.heap_base + 7) == (int32_t)(u.heap_base + 7));
    CHECK(ua_brk(&u, u.heap_base + PAGE_SIZE + 9) > 0);
    CHECK(get_word(&u, u.heap_base) == 123 && get_word(&u, u.heap_base + PAGE_SIZE) == 0);
    unsigned before = pages_used; uint32_t old = u.brk;
    alloc_fail = 0; CHECK(ua_brk(&u, u.brk + 2 * PAGE_SIZE) == -ENOMEM); alloc_fail = -1;
    CHECK(u.brk == old && pages_used == before);
    CHECK(ua_brk(&u, CIUKI_HEAP_LIMIT) == -EINVAL);
    ua_destroy(&u); CHECK(pages_used == base_pages && heap_blocks == base_heap);
    puts("proc arena: first-fit/hints, 16 protections, pins, splits/holes, limits, brk, allocation rollback PASS");
}
static void test_stack(void)
{
    struct proc_strings *s = proc_strings_new(); CHECK(s);
    CHECK(proc_strings_add(s, "program", 8, false) == 0);
    CHECK(proc_strings_add(s, "hello", 6, false) == 0);
    CHECK(proc_strings_add(s, "A=B", 4, true) == 0);
    struct uaddr u; CHECK(ua_init(&u, &u) == 0);
    CHECK(ua_map_at(&u, CIUKI_MAIN_STACK_BASE, CIUKI_THREAD_STACK_DEFAULT, PROT_READ | PROT_WRITE,
                     PROT_READ | PROT_WRITE, UA_STACK, 1) == 0);
    uint32_t esp; CHECK(proc_stack_build(&u, s, CIUKI_IMAGE_BASE, CIUKI_MMAP_BASE, &esp) == 0);
    CHECK(!(esp & 3) && get_word(&u, esp) == 2);
    CHECK(get_word(&u, esp + 3 * 4) == 0 && get_word(&u, esp + 5 * 4) == 0);
    char txt[8]; CHECK(ua_read(&u, txt, get_word(&u, esp + 4), 8) == 0 && !strcmp(txt, "program"));
    const uint32_t aux[] = { AT_PAGESZ, CIUKI_PAGE_SIZE, AT_ENTRY, CIUKI_IMAGE_BASE,
        AT_CIUKI_TLS, CIUKI_MMAP_BASE, AT_CIUKI_TLS_SIZE, CIUKI_TLS_SIZE, AT_CIUKI_ABI, CIUKI_ABI_VERSION, AT_NULL, 0 };
    for (unsigned i = 0; i < 12; i++) CHECK(get_word(&u, esp + (6 + i) * 4) == aux[i]);
    proc_strings_free(s);
    s = proc_strings_new(); CHECK(s);
    for (unsigned i = 0; i < CIUKI_ARGV_MAX; i++) CHECK(proc_strings_add(s, "", 1, false) == 0);
    CHECK(proc_strings_add(s, "", 1, false) == -E2BIG); proc_strings_free(s);
    s = proc_strings_new(); CHECK(s);
    for (unsigned i = 0; i < CIUKI_ENVP_MAX; i++) CHECK(proc_strings_add(s, "", 1, true) == 0);
    CHECK(proc_strings_add(s, "", 1, true) == -E2BIG); proc_strings_free(s);
    char *large = malloc(CIUKI_ARG_MAX); CHECK(large); memset(large, 'x', CIUKI_ARG_MAX);
    uint32_t max = CIUKI_ARG_MAX - 16 * 4; large[max - 1] = 0;
    s = proc_strings_new(); CHECK(proc_strings_add(s, large, max, false) == 0);
    CHECK(proc_stack_build(&u, s, CIUKI_IMAGE_BASE, CIUKI_MMAP_BASE, &esp) == 0 && esp == CIUKI_MAIN_STACK_LIMIT - CIUKI_ARG_MAX);
    proc_strings_free(s); large[max - 1] = 'x'; large[max] = 0;
    s = proc_strings_new(); CHECK(proc_strings_add(s, large, max + 1, false) == -E2BIG); proc_strings_free(s);
    free(large); ua_destroy(&u);
    puts("proc stack: argc/argv/envp/auxv, alignment, counts, exact ARG_MAX/one-over PASS");
}
static void test_queue(void)
{
    struct uaddr u; CHECK(ua_init(&u, &u) == 0);
    struct ciuki_mmap_args a = map_args(PAGE_SIZE, PROT_READ | PROT_WRITE);
    CHECK(ua_mmap(&u, &a) == CIUKI_MMAP_BASE);
    struct ww_key key; CHECK(ww_bind(&u, CIUKI_MMAP_BASE, &key) == 0);
    CHECK(ww_bind(&u, CIUKI_MMAP_BASE + 1, &key) == -EINVAL);
    CHECK(ww_bind(&u, CIUKI_MMAP_LIMIT, &key) == -EFAULT);
    struct ww_waiter w[3] = { 0 };
    CHECK(ww_enqueue(&w[0], &key, 0, 7, 8) == -EAGAIN && !w[0].queued);
    CHECK(ww_wake(&key, 3) == 0); /* signal before enqueue saves no credit */
    for (unsigned i = 0; i < 3; i++) CHECK(ww_enqueue(&w[i], &key, 0, 7, 7) == 0);
    CHECK(ww_wake(&key, 0) == 0); CHECK(ww_wake(&key, 1) == 1 && !w[0].queued && w[1].queued);
    CHECK(ua_munmap(&u, CIUKI_MMAP_BASE, PAGE_SIZE) == 0);
    CHECK(w[1].result == -ECANCELED && w[2].result == -ECANCELED && !w[2].queued);
    CHECK(ua_mmap(&u, &a) == CIUKI_MMAP_BASE);
    struct ww_key fresh; CHECK(ww_bind(&u, CIUKI_MMAP_BASE, &fresh) == 0 && key.generation != fresh.generation);
    CHECK(ww_enqueue(&w[0], &fresh, 0, 0, 0) == 0);
    CHECK(ww_wake(&key, 1) == 0 && w[0].queued); CHECK(ww_wake(&fresh, 1) == 1);
    CHECK(ww_enqueue(&w[1], &fresh, 0, 0, 0) == 0); ww_interrupt(&w[1], -EINTR); CHECK(w[1].result == -EINTR);
    struct ciuki_timespec time; g_ticks = 100; proc_clock_seed(1700000000, 100);
    CHECK(proc_clock_now(CLOCK_REALTIME, &time) == 0 && time.tv_sec == 1700000000 && !time.tv_nsec);
    g_ticks = 120; time = (struct ciuki_timespec){ .tv_sec = 0, .tv_nsec = 120000001 };
    CHECK(!proc_deadline_passed(&time, CLOCK_MONOTONIC)); g_ticks++; CHECK(proc_deadline_passed(&time, CLOCK_MONOTONIC));
    time = (struct ciuki_timespec){ .tv_sec = INT64_MAX }; CHECK(!proc_deadline_passed(&time, CLOCK_REALTIME));
    ua_destroy(&u);
    puts("proc waits: mismatch, FIFO, no credits, unmap/reuse generation, EINTR, deadlines PASS");
}
static struct process *parent_process;
static struct task controller;
static struct proc_thread *make_process(struct process *parent, struct process **out)
{
    struct ciuki_file file = fixture_file(); struct proc_strings *s = proc_strings_new();
    CHECK(s && proc_strings_add(s, "test", 5, false) == 0);
    CHECK(proc_spawn_file(parent, &file, s, 0, 0, 0, 0, out) > 0);
    proc_strings_free(s);
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) { struct proc_thread *t = proc_thread_slot(i); if (t && t->process == *out) return t; }
    CHECK(false); return 0;
}
static unsigned object_refs[4];
struct test_object { struct proc_object object; unsigned id; };
static void obj_retain(struct proc_object *o) { struct test_object *t = (struct test_object *)o; object_refs[t->id]++; }
static void obj_release(struct proc_object *o) { struct test_object *t = (struct test_object *)o; CHECK(object_refs[t->id]); object_refs[t->id]--; }
static void stop_collect(struct process *p) { g_current = &controller; proc_stop(p, 37, 0); proc_collect(); }
static void schedule_timeout(void) { }
static void schedule_wake(void) { struct proc_thread *t = proc_thread_for(g_current); CHECK(ww_wake(&t->word_wait.key, 1) == 1); }
static void schedule_cancel(void) { CHECK(ua_munmap(proc_current()->memory, CIUKI_MMAP_BASE + PAGE_SIZE, PAGE_SIZE) == 0); }
static void test_processes(void)
{
    proc_init(); g_current = &controller;
    struct proc_thread *parent_thread = make_process(proc_supervisor(), &parent_process);
    struct process *p = parent_process;
    struct test_object objects[4];
    for (unsigned i = 0; i < 4; i++) { objects[i] = (struct test_object){ .object = { obj_retain, obj_release, true }, .id = i }; object_refs[i] = 1; p->fds[i].object = &objects[i].object; }
    p->fds[3].flags = FD_CLOEXEC;
    struct process *child; CHECK(proc_prepare(p, &child) == 0);
    struct ciuki_spawn_fd fds[] = { { 0, 1 }, { 1, 0 }, { -1, 2 } };
    CHECK(proc_inherit(child, p, fds, 3) == 0);
    CHECK(child->fds[0].object == &objects[1].object && child->fds[1].object == &objects[0].object && !child->fds[2].object && !child->fds[3].object);
    CHECK(object_refs[0] == 2 && object_refs[1] == 2 && object_refs[2] == 1); proc_discard(child);
    CHECK(proc_prepare(p, &child) == 0); fds[0].source = 3; CHECK(proc_inherit(child, p, fds, 1) == -EBADF);
    fds[0].source = 1; objects[1].object.inheritable = false; CHECK(proc_inherit(child, p, fds, 1) == -EPERM); objects[1].object.inheritable = true;
    fds[1].target = fds[0].target; CHECK(proc_inherit(child, p, fds, 2) == -EINVAL); proc_discard(child);
    struct proc_ledger baseline; proc_snapshot(&baseline); unsigned baseline_pages = pages_used, baseline_heap = heap_blocks;
    uint32_t last_pid = 0, last_tid = 0;
    for (unsigned cycle = 0; cycle < 100; cycle++) {
        struct proc_thread *ct = make_process(p, &child);
        CHECK(child->pid > last_pid && ct->tid > last_tid); last_pid = child->pid; last_tid = ct->tid;
        CHECK(child->ppid == p->pid && child->pgid == p->pgid && child->fds[2].object == p->fds[2].object && !child->fds[3].object);
        CHECK(child->memory->as.pd_phys != p->memory->as.pd_phys);
        put_word(p->memory, CIUKI_IMAGE_BASE + PAGE_SIZE, 0x12345678);
        CHECK(get_word(child->memory, CIUKI_IMAGE_BASE + PAGE_SIZE) == 0x71717171);
        struct process *found = 0; CHECK(proc_wait_select(p, (int32_t)child->pid, &found) == 0);
        g_current = parent_thread->task; g_current->state = T_RUNNING;
        CHECK(proc_waitpid((int32_t)child->pid, 0, WNOHANG) == 0);
        g_current = &controller;
        if (cycle & 1) { ct->task->fault_vector = 14; ct->task->fault_cr2 = 0; task_kill(ct->task, EXIT_FAULT_BASE + 14); }
        else proc_stop(child, 37, 0);
        CHECK(child->state == PROC_STOPPING); proc_collect();
        CHECK(child->state == PROC_ZOMBIE && !child->memory && !child->fds && !child->threads);
        CHECK(child->status == ((cycle & 1) ? SIGSEGV : 9472));
        g_current = parent_thread->task;
        CHECK(proc_waitpid((int32_t)child->pid, CIUKI_IMAGE_BASE, 0) == -EFAULT);
        uint32_t pid = child->pid;
        CHECK(proc_waitpid((int32_t)pid, CIUKI_IMAGE_BASE + PAGE_SIZE, 0) == (int32_t)pid);
        CHECK(proc_waitpid((int32_t)pid, 0, 0) == -ECHILD);
        CHECK(proc_reap(p, child) == -ECHILD);
        g_current = &controller;
        struct proc_ledger after; proc_snapshot(&after);
        CHECK(!memcmp(&baseline, &after, sizeof(after)) && pages_used == baseline_pages && heap_blocks == baseline_heap);
    }
    printf("proc cycles=100 pages=%u/%u heap_blocks=%u/%u processes=%u threads=%u zombies=%u handles=%u maps=%u backing=%u tables=%u leaked=0\n",
        baseline_pages, pages_used, baseline_heap, heap_blocks, baseline.processes, baseline.threads, baseline.zombies, baseline.handles, baseline.extents, baseline.backing, baseline.tables);

    /* Full production blocking paths under controlled schedules. */
    g_current = parent_thread->task; g_current->state = T_RUNNING;
    struct ciuki_mmap_args a = map_args(PAGE_SIZE, PROT_READ | PROT_WRITE);
    CHECK(ua_mmap(p->memory, &a) == CIUKI_MMAP_BASE + PAGE_SIZE);
    uint32_t word = CIUKI_MMAP_BASE + PAGE_SIZE;
    CHECK(proc_wait_word(word, 1, 0, CLOCK_MONOTONIC) == -EAGAIN);
    on_schedule = schedule_wake; CHECK(proc_wait_word(word, 0, 0, CLOCK_MONOTONIC) == 0);
    struct ciuki_timespec deadline = { .tv_sec = (int64_t)(g_ticks / 1000), .tv_nsec = (int32_t)(g_ticks % 1000) * 1000000 + 2000000 };
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    CHECK(ua_write(p->memory, word + 16, &deadline, sizeof(deadline)) == 0);
    on_schedule = schedule_timeout; CHECK(proc_wait_word(word, 0, word + 16, CLOCK_MONOTONIC) == -ETIMEDOUT);
    CHECK(proc_deadline_passed(&deadline, CLOCK_MONOTONIC));
    on_schedule = schedule_cancel; CHECK(proc_wait_word(word, 0, 0, CLOCK_REALTIME) == -ECANCELED); on_schedule = 0;

    struct ciuki_thread_args args = { .size = sizeof(args), .entry = CIUKI_IMAGE_BASE,
        .return_trampoline = CIUKI_IMAGE_BASE + 1, .stack_bytes = CIUKI_THREAD_STACK_MIN, .argument = 0x1234 };
    uint32_t args_va = CIUKI_IMAGE_BASE + PAGE_SIZE + 128;
    CHECK(ua_write(p->memory, args_va, &args, sizeof(args)) == 0);
    int tid = proc_thread_create(args_va); CHECK(tid > 0);
    struct proc_thread *t = proc_thread_find(p, (uint32_t)tid); CHECK(t && t->tls != parent_thread->tls);
    CHECK(get_word(p->memory, t->tls) == t->tls && get_word(p->memory, t->tls + 4) == t->tid);
    CHECK(get_word(p->memory, t->task->saved_esp) == args.return_trampoline && get_word(p->memory, t->task->saved_esp + 4) == args.argument);
    CHECK(ua_munmap(p->memory, t->stack_base, PAGE_SIZE) == -EBUSY);
    CHECK(ua_mprotect(p->memory, t->tls, PAGE_SIZE, PROT_READ) == -EBUSY);
    CHECK(proc_tls_set(t->tls, CIUKI_TLS_SIZE) == -EBUSY);
    CHECK(proc_thread_join(parent_thread->tid, 0) == -EDEADLK);
    t->joiner = parent_thread; CHECK(proc_thread_detach(t->tid) == -EINVAL); t->joiner = 0;
    parent_thread->interrupted = true;
    CHECK(proc_thread_join(t->tid, args_va) == -EINTR && !t->joiner);
    parent_thread->interrupted = false;
    t->value = 0xfeedbeef; t->exiting = true; task_kill(t->task, 0);
    g_current = &controller; proc_collect(); CHECK(t->retained && !t->task && !t->tls && p->state == PROC_LIVE);
    g_current = parent_thread->task;
    CHECK(proc_thread_join((uint32_t)tid, CIUKI_IMAGE_BASE) == -EFAULT);
    CHECK(proc_thread_join((uint32_t)tid, args_va) == 0 && get_word(p->memory, args_va) == 0xfeedbeef);
    CHECK(proc_thread_join((uint32_t)tid, 0) == -ESRCH);
    a = map_args(2 * PAGE_SIZE, PROT_READ | PROT_WRITE); int32_t tls = ua_mmap(p->memory, &a); CHECK(tls > 0);
    CHECK(proc_tls_set((uint32_t)tls + 4, CIUKI_TLS_SIZE) == 0 && tls_base == (uint32_t)tls + 4);
    CHECK(ua_munmap(p->memory, (uint32_t)tls, PAGE_SIZE) == -EBUSY);
    CHECK(proc_tls_set((uint32_t)tls + 1, CIUKI_TLS_SIZE) == -EINVAL);
    parent_thread->in_handler = true; CHECK(proc_tls_set((uint32_t)tls, CIUKI_TLS_SIZE) == -EBUSY); parent_thread->in_handler = false;
    CHECK(proc_strings_copy((struct proc_strings[1]){{0}}, 0, 0) == -EINVAL);
    struct proc_strings *s = proc_strings_new(); CHECK(proc_strings_copy(s, CIUKI_MMAP_LIMIT, 0) == -EFAULT); proc_strings_free(s);
    g_current = &controller;
    /* Adoption of both live children and zombies; PID 1 reaps automatically. */
    struct process *orphan; make_process(p, &child); make_process(child, &orphan);
    uint32_t orphan_pid = orphan->pid; stop_collect(child); CHECK(orphan->ppid == 1);
    CHECK(proc_reap(p, child) == 0); stop_collect(orphan); CHECK(!proc_find(orphan_pid));
    struct process *second; make_process(p, &child); make_process(p, &second);
    stop_collect(second); stop_collect(child);
    struct process *selected = 0;
    CHECK(proc_wait_select(p, -1, &selected) == (int)child->pid && selected == child);
    CHECK(proc_reap(p, child) == 0);
    CHECK(proc_wait_select(p, -1, &selected) == (int)second->pid && selected == second);
    CHECK(proc_reap(p, second) == 0);
    p->actions[SIGCHLD].handler = SIG_IGN;
    make_process(p, &child); uint32_t ignored_pid = child->pid; stop_collect(child);
    CHECK(!proc_find(ignored_pid) && proc_wait_select(p, -1, &selected) == -ECHILD);
    p->actions[SIGCHLD].handler = SIG_DFL;
    struct proc_thread *last = make_process(p, &child), *detached;
    CHECK(proc_thread_prepare(child, CIUKI_IMAGE_BASE, 0, CIUKI_IMAGE_BASE, CIUKI_THREAD_STACK_MIN,
        CIUKI_THREAD_DETACHED, 0, false, &detached) == 0);
    uint32_t detached_tid = detached->tid; task_start(detached->task);
    detached->exiting = true; task_kill(detached->task, 0); proc_collect();
    CHECK(!proc_thread_find(child, detached_tid) && child->live_threads == 1);
    g_current = last->task; g_current->state = T_RUNNING;
    expect_exit = true;
    if (!setjmp(exited)) proc_thread_exit(0x12345678);
    expect_exit = false; g_current = &controller; proc_collect();
    CHECK(child->state == PROC_ZOMBIE && child->status == 0 && !child->threads);
    CHECK(proc_reap(p, child) == 0);
    last = make_process(p, &child);
    last->in_syscall = true;
    last->task->state = T_BLOCKED;
    proc_stop(child, 37, 0);
    CHECK(last->task->state == T_READY && child->state == PROC_STOPPING);
    proc_collect(); CHECK(child->memory && child->state == PROC_STOPPING);
    last->in_syscall = false;
    task_kill(last->task, 37); proc_collect();
    CHECK(child->state == PROC_ZOMBIE && child->status == 9472);
    CHECK(proc_reap(p, child) == 0);
    uint32_t parent_pid = p->pid; stop_collect(p); CHECK(!proc_find(parent_pid));
    for (unsigned i = 0; i < 4; i++) CHECK(!object_refs[i]);
    puts("proc lifecycle: inheritance, snapshots, fault status, wait copyout/reap, adoption, threads/TLS/join PASS");
}
static void test_creation_failure(void)
{
    g_current = &controller;
    struct process *p; make_process(proc_supervisor(), &p);
    struct proc_strings *s = proc_strings_new(); CHECK(s && proc_strings_add(s, "test", 5, false) == 0);
    struct ciuki_file file = fixture_file(); unsigned base_pages = pages_used, base_heap = heap_blocks;
    struct proc_ledger base; proc_snapshot(&base); unsigned successes = 0;
    for (int failure = 0; failure < 285; failure++) {
        struct process *child = 0;
        alloc_fail = failure; int r = proc_spawn_file(p, &file, s, 0, 0, 0, 0, &child); alloc_fail = -1;
        if (r > 0) { successes++; stop_collect(child); CHECK(proc_reap(p, child) == 0); }
        else CHECK(r == -ENOMEM);
        struct proc_ledger now; proc_snapshot(&now);
        CHECK(!memcmp(&base, &now, sizeof(now)) && pages_used == base_pages && heap_blocks == base_heap);
    }
    CHECK(successes); proc_strings_free(s); stop_collect(p);
    puts("proc spawn: allocation failure at every preparation stage, no publication/leaks PASS");
}
static jmp_buf canceled_spawn;
static bool cancel_during_open, cancel_during_read, corrupt_snapshot;
static unsigned snapshots_closed;
static void snapshot_close(void *cookie) { (void)cookie; snapshots_closed++; }
static int snapshot_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    if (cancel_during_read && off >= PAGE_SIZE) {
        task_kill(g_current, 37);
        longjmp(canceled_spawn, 1);
    }
    return fixture_read(cookie, off, dst, bytes);
}
static int snapshot_open(void *cwd, const char *path, struct ciuki_file *file)
{
    (void)cwd; CHECK(!strcmp(path, "/fixture"));
    *file = fixture_file(); file->close = snapshot_close; file->read = snapshot_read;
    if (corrupt_snapshot) eh()->type = 3;
    if (cancel_during_open) { task_kill(g_current, 37); longjmp(canceled_spawn, 1); }
    /* The loader must use its argument snapshot, not this changed parent. */
    CHECK(ua_write(proc_current()->memory, CIUKI_IMAGE_BASE + PAGE_SIZE + 320, "changed", 8) == 0);
    return 0;
}
static const struct ciuki_file_ops snapshot_ops = { .open = snapshot_open };
static uint32_t prepare_spawn_args(struct process *p)
{
    uint32_t base = CIUKI_IMAGE_BASE + PAGE_SIZE;
    struct ciuki_spawn_args args = { .size = sizeof(args), .path = base + 256, .argv = base + 512 };
    uint32_t vector[2] = { base + 320, 0 };
    CHECK(ua_write(p->memory, base + 128, &args, sizeof(args)) == 0);
    CHECK(ua_write(p->memory, base + 256, "/fixture", 9) == 0);
    CHECK(ua_write(p->memory, base + 320, "program", 8) == 0);
    CHECK(ua_write(p->memory, base + 512, vector, sizeof(vector)) == 0);
    return base + 128;
}
static void test_spawn_syscall(void)
{
    struct process *p; struct proc_thread *pt = make_process(proc_supervisor(), &p);
    g_current = pt->task; g_current->state = T_RUNNING;
    proc_set_file_ops(&snapshot_ops);
    p->actions[SIGUSR1].handler = CIUKI_IMAGE_BASE;
    p->actions[SIGUSR2].handler = SIG_IGN;
    pt->mask = CIUKI_SIGBIT(SIGUSR1);
    unsigned baseline_pages = pages_used, baseline_heap = heap_blocks;
    uint32_t args = prepare_spawn_args(p);
    unsigned closed = snapshots_closed;
    int pid = proc_spawn(args); CHECK(pid > 0 && snapshots_closed == closed + 1);
    struct process *child = proc_find((uint32_t)pid); CHECK(child);
    CHECK(!child->actions[SIGUSR1].handler && child->actions[SIGUSR2].handler == SIG_IGN && !child->pending);
    struct proc_thread *ct = 0;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) if (proc_thread_slot(i) && proc_thread_slot(i)->process == child) ct = proc_thread_slot(i);
    CHECK(ct && ct->mask == pt->mask);
    char arg[8]; CHECK(ua_read(child->memory, arg, get_word(child->memory, ct->task->saved_esp + 4), sizeof(arg)) == 0);
    CHECK(!strcmp(arg, "program"));
    stop_collect(child); CHECK(proc_reap(p, child) == 0);
    g_current = pt->task;
    CHECK(pages_used == baseline_pages && heap_blocks == baseline_heap);
    args = prepare_spawn_args(p); corrupt_snapshot = true;
    CHECK(proc_spawn(args) == -ENOEXEC); corrupt_snapshot = false;
    CHECK(!pt->operation && !pt->cleanup && pages_used == baseline_pages && heap_blocks == baseline_heap);
    put_word(p->memory, args + offsetof(struct ciuki_spawn_args, argv), CIUKI_MMAP_LIMIT);
    CHECK(proc_spawn(args) == -EFAULT && pages_used == baseline_pages && heap_blocks == baseline_heap);
    args = prepare_spawn_args(p);
    put_word(p->memory, args + offsetof(struct ciuki_spawn_args, reserved), 1);
    CHECK(proc_spawn(args) == -EINVAL);
    args = prepare_spawn_args(p);
    uint32_t data = CIUKI_IMAGE_BASE + PAGE_SIZE;
    for (unsigned i = 0; i <= CIUKI_ARGV_MAX; i++) put_word(p->memory, data + 1024 + i * 4, data + 320);
    put_word(p->memory, args + offsetof(struct ciuki_spawn_args, argv), data + 1024);
    CHECK(proc_spawn(args) == -E2BIG && pages_used == baseline_pages && heap_blocks == baseline_heap);
    args = prepare_spawn_args(p);
    put_word(p->memory, args + offsetof(struct ciuki_spawn_args, argv), data + 2 * PAGE_SIZE - 2);
    CHECK(proc_spawn(args) == -EFAULT && pages_used == baseline_pages && heap_blocks == baseline_heap);
    args = prepare_spawn_args(p);
    struct ciuki_spawn_fd duplicate[2] = { { -1, 0 }, { -1, 0 } };
    CHECK(ua_write(p->memory, data + 768, duplicate, sizeof(duplicate)) == 0);
    put_word(p->memory, args + offsetof(struct ciuki_spawn_args, fd_list), data + 768);
    put_word(p->memory, args + offsetof(struct ciuki_spawn_args, fd_count), 2);
    CHECK(proc_spawn(args) == -EINVAL && pages_used == baseline_pages && heap_blocks == baseline_heap);
    stop_collect(p);
    for (unsigned stage = 0; stage < 2; stage++) {
        pt = make_process(proc_supervisor(), &p);
        g_current = pt->task; g_current->state = T_RUNNING;
        args = prepare_spawn_args(p); closed = snapshots_closed;
        cancel_during_open = stage == 0; cancel_during_read = stage == 1;
        if (!setjmp(canceled_spawn)) { (void)proc_spawn(args); CHECK(false); }
        cancel_during_open = cancel_during_read = false;
        g_current = &controller; proc_collect();
        CHECK(snapshots_closed == closed + 1 && !pages_used && heap_blocks == 1);
    }
    proc_set_file_ops(0);
    puts("proc syscall: argv/signal snapshots, vector bounds/straddles, fd validation, cancellation during open/read PASS");
}
static void test_limits(void)
{
    struct process *ps[CIUKI_PROCESS_MAX] = { 0 };
    unsigned base_pages = pages_used, base_heap = heap_blocks;
    for (unsigned i = 1; i < CIUKI_PROCESS_MAX; i++) CHECK(proc_prepare(proc_supervisor(), &ps[i]) == 0);
    struct process *extra = 0; CHECK(proc_prepare(proc_supervisor(), &extra) == -EAGAIN);
    for (unsigned i = 1; i < CIUKI_PROCESS_MAX; i++) proc_discard(ps[i]);
    CHECK(pages_used == base_pages && heap_blocks == base_heap);
    unsigned n = 0;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX / CIUKI_PROCESS_THREAD_MAX; i++) {
        CHECK(proc_prepare(proc_supervisor(), &ps[i]) == 0);
        for (unsigned j = 0; j < CIUKI_PROCESS_THREAD_MAX; j++) {
            struct proc_thread *t;
            CHECK(proc_thread_prepare(ps[i], CIUKI_IMAGE_BASE, 0, CIUKI_IMAGE_BASE,
                CIUKI_THREAD_STACK_MIN, 0, 0, false, &t) == 0); n++;
        }
        struct proc_thread *t; CHECK(proc_thread_prepare(ps[i], CIUKI_IMAGE_BASE, 0, CIUKI_IMAGE_BASE,
            CIUKI_THREAD_STACK_MIN, 0, 0, false, &t) == -EAGAIN);
    }
    CHECK(n == CIUKI_THREAD_MAX);
    CHECK(proc_prepare(proc_supervisor(), &extra) == 0);
    struct proc_thread *t; CHECK(proc_thread_prepare(extra, CIUKI_IMAGE_BASE, 0, CIUKI_IMAGE_BASE,
        CIUKI_THREAD_STACK_MIN, 0, 0, false, &t) == -EAGAIN); proc_discard(extra);
    for (unsigned i = 0; i < CIUKI_THREAD_MAX / CIUKI_PROCESS_THREAD_MAX; i++) proc_discard(ps[i]);
    CHECK(pages_used == base_pages && heap_blocks == base_heap);
    /* Exhaustion injection is host-local; production has no ID reset hook. */
    uint32_t saved_pid = next_pid, saved_tid = next_tid;
    next_pid = CIUKI_ID_MAX;
    CHECK(proc_prepare(proc_supervisor(), &extra) == 0 && extra->pid == CIUKI_ID_MAX);
    CHECK(proc_prepare(proc_supervisor(), &ps[0]) == -EAGAIN);
    next_tid = CIUKI_ID_MAX;
    CHECK(proc_thread_prepare(extra, CIUKI_IMAGE_BASE, 0, CIUKI_IMAGE_BASE, CIUKI_THREAD_STACK_MIN,
        0, 0, false, &t) == 0 && t->tid == CIUKI_ID_MAX);
    CHECK(proc_thread_prepare(extra, CIUKI_IMAGE_BASE, 0, CIUKI_IMAGE_BASE, CIUKI_THREAD_STACK_MIN,
        0, 0, false, &t) == -EAGAIN);
    proc_discard(extra); next_pid = saved_pid; next_tid = saved_tid;
    CHECK(pages_used == base_pages && heap_blocks == base_heap);
    puts("proc limits: 64 processes, 256 retained/live threads, 16 per process, INT32_MAX exhaustion PASS");
}
static int real_payload_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    memcpy(dst, (const uint8_t *)cookie + off, bytes);
    return 0;
}
static void test_real_payload(const char *path)
{
    FILE *file = fopen(path, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file); CHECK(size > 0 && size < 4 * PAGE_SIZE);
    rewind(file); uint8_t *bytes = malloc((size_t)size); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t)size, file) == (size_t)size); fclose(file);
    struct ciuki_file input = { .cookie = bytes, .bytes = (uint32_t)size, .read = real_payload_read };
    struct elf_image image; CHECK(elf_validate(&input, &image) == 0);
    CHECK(image.entry == CIUKI_IMAGE_BASE && image.count == 2 && image.bytes == 3 * PAGE_SIZE);
    struct uaddr u; CHECK(ua_init(&u, &u) == 0 && elf_load(&input, &image, &u) == 0);
    CHECK(get_word(&u, CIUKI_IMAGE_BASE + PAGE_SIZE) == 0x71717171);
    CHECK(get_word(&u, CIUKI_IMAGE_BASE + PAGE_SIZE + 4) == 0);
    ua_destroy(&u); free(bytes);
    puts("proc actual NASM ELF: production validation/load PASS (execution not run)");
}
int main(int argc, char **argv)
{
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING;
    test_elf(); test_arena(); test_stack(); test_queue(); test_processes(); test_creation_failure(); test_spawn_syscall(); test_limits();
    if (argc == 2) test_real_payload(argv[1]);
    struct trap_frame tf = { .vector = 14, .cs = 8,
        .eip = (uint32_t)(uintptr_t)copy_user_fault_start };
    proc_trap_dispatch(&tf);
    CHECK(!trap_calls && tf.eip == (uint32_t)(uintptr_t)copy_user_fixup);
    tf.eip = (uint32_t)(uintptr_t)copy_user_fault_end;
    proc_trap_dispatch(&tf); CHECK(trap_calls == 1);
    tf.eip = (uint32_t)(uintptr_t)copy_user_fault_start; tf.cs = 0x1b;
    proc_trap_dispatch(&tf); CHECK(trap_calls == 2);
    CHECK(invlpgs == 0); /* host tests never activate a native CR3 */
    struct proc_ledger ledger; proc_snapshot(&ledger);
    CHECK(ledger.processes == 1 && !ledger.threads && !ledger.zombies && !ledger.handles && !ledger.extents && !ledger.backing && !ledger.tables);
    CHECK(!pages_used && !g_user_mappings && heap_blocks == 1); /* PID 1 scheduler task */
    printf("proc final: pages=0 user_mappings=0 processes=1 threads=0 zombies=0 handles=0 maps=0 checks=%u PASS\n", checks);
    free(ram);
    return 0;
}
