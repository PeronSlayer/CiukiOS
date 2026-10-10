/* Interim F2 controllers use the production loader, mappings and syscalls.
 * The embedded NASM fixture is superseded by SDK payloads in the lead's
 * runner/SDK directives. No selector dispatch or storage mount lives here.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>
#include <ciuki/probe.h>
#include <ciuki/sha256.h>

__asm__(".pushsection .rodata.proc_payload,\"a\"\n"
        ".balign 4\n"
        ".global f2_payload_start, f2_payload_end\n"
        "f2_payload_start:\n"
        ".incbin \"" CIUKI_PROC_PAYLOAD_BIN "\"\n"
        "f2_payload_end:\n"
        ".popsection\n");
extern const uint8_t f2_payload_start[], f2_payload_end[];

#define RESULT (CIUKI_IMAGE_BASE + CIUKI_PAGE_SIZE)
struct payload_result {
    uint32_t sentinel, done, release, errors, esp, flags, gs, tls, argc;
    uint32_t thread_gate, increments, mutex, completed, reserved[3], tids[8];
    uint32_t reserved2[8], token_state, token_value, tokens;
};

static int image_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    (void)cookie;
    uint32_t size = (uint32_t)(f2_payload_end - f2_payload_start);
    if (off > size || bytes > size - off)
        return -EIO;
    memcpy(dst, f2_payload_start + off, bytes);
    return 0;
}

static struct ciuki_file image_file(void)
{
    return (struct ciuki_file){ .bytes = (uint32_t)(f2_payload_end - f2_payload_start), .read = image_read };
}

static int image_open(void *cwd, const char *path, struct ciuki_file *out)
{
    (void)cwd;
    if (strncmp(path, "/f2-fixture", sizeof("/f2-fixture")))
        return -ENOENT;
    *out = image_file();
    return 0;
}

static const struct ciuki_file_ops probe_files = { .open = image_open };

static int make_payload(struct process *parent, const char *mode, struct process **out)
{
    struct proc_strings *strings = proc_strings_new();
    if (!strings)
        return -ENOMEM;
    int err = proc_strings_add(strings, "fixture", sizeof("fixture"), false);
    if (!err)
        err = proc_strings_add(strings, mode, 2, false);
    if (!err)
        err = proc_strings_add(strings, "A=B", sizeof("A=B"), true);
    struct ciuki_file file = image_file();
    if (!err)
        err = proc_spawn_file(parent, &file, strings, 0, 0, 0, 0, out);
    proc_strings_free(strings);
    return err;
}

static int finish(const char *name, bool pass)
{
    rec_emit(name, "END", pass ? "status=PASS" : "status=FAIL reason=process_contract");
    return pass ? 0 : 1;
}

static bool result_read(struct process *p, struct payload_result *out)
{
    return p->state == PROC_LIVE && p->memory && !ua_read(p->memory, out, RESULT, sizeof(*out));
}

static void release_payload(struct process *p)
{
    uint32_t address = RESULT + offsetof(struct payload_result, release), value = 1;
    struct ww_key key;
    ua_write(p->memory, address, &value, sizeof(value));
    if (!ww_bind(p->memory, address, &key))
        ww_wake(&key, UINT32_MAX);
}

static void record_fault(const char *name, const struct process *p)
{
    rec_emit(name, "DATA", "case=fault pid=%u vector=%u error=%08x address=%08x eip=%08x",
             p->pid, p->fault_vector, p->fault_error, p->fault_address, p->fault_eip);
}

static void record_thread_ids(const char *name, const struct payload_result *r)
{
    for (unsigned i = 0; i < ARRAY_SIZE(r->tids); i++)
        rec_emit(name, "DATA", "case=thread index=%u tid=%u", i, r->tids[i]);
}

static bool run_payload(const char *name, struct process *parent, const char *mode,
                         uint32_t *switches_out)
{
    struct process *p;
    int result = make_payload(parent, mode, &p);
    if (result < 0) {
        rec_emit(name, "DATA", "case=spawn expected=0 observed=%d", result);
        return false;
    }
    uint32_t pid = p->pid, preemptions = 0;
    struct payload_result r = { 0 };
    uint64_t deadline = g_ticks + 30000;
    while (g_ticks < deadline && p->state != PROC_ZOMBIE) {
        if (mode[0] == 's') {
            /* The sole child is created through the ring-3 spawn syscall. */
            struct process *child = proc_find(pid + 1);
            struct payload_result cr;
            struct payload_result pr;
            if (child && child->ppid == pid && result_read(p, &pr) && pr.reserved[0] &&
                result_read(child, &cr) && cr.done)
                release_payload(child);
        }
        if (result_read(p, &r)) {
            if (mode[0] == 't' && r.completed == 8 && !r.thread_gate) {
                preemptions = 0;
                for (unsigned i = 0; i < ARRAY_SIZE(r.tids); i++) {
                    struct proc_thread *t = proc_thread_find(p, r.tids[i]);
                    if (t && t->task) {
                        t->task->measured = true;
                        preemptions += t->task->preempt_dispatches;
                    }
                }
                /* These are all genuine timer dispatches; task_yield is not
                 * used by workers. F0's measured-counter policy stays frozen. */
                if (preemptions >= 1000) {
                    uint32_t one = 1;
                    ua_write(p->memory, RESULT + offsetof(struct payload_result, thread_gate), &one, sizeof(one));
                }
            }
            if (r.done)
                release_payload(p);
        }
        /* Leave timer quanta between controller polls: an interactive
         * controller waking on every tick would interpose a kernel task
         * between all user dispatches and invalidate the switch evidence. */
        task_sleep_ms(mode[0] == 't' ? 20 : 1);
    }
    bool fault = mode[0] == 'f' || mode[0] == 'n' || mode[0] == 'r';
    bool pass = p->state == PROC_ZOMBIE && p->status == (fault ? SIGSEGV : 9472) &&
                (fault ? p->fault_vector == 14 : r.done && !r.errors);
    if (mode[0] == 't')
        pass = pass && preemptions >= 1000 && r.increments == 80000 && r.completed == 8 && r.tokens == 10000;
    rec_emit(name, "DATA", "case=%s pid=%u ppid=%u pgid=%u expected=%u observed=%d raw=%d vector=%u errors=%u",
             mode, pid, p->ppid, p->pgid, fault ? SIGSEGV : 9472, p->status, p->raw_exit, p->fault_vector, r.errors);
    if (fault)
        record_fault(name, p);
    if (!fault)
        rec_emit(name, "DATA", "case=entry pid=%u esp=%08x flags=%08x gs=%08x tls=%08x argc=%u bss_register_errors=%u",
                 pid, r.esp, r.flags, r.gs, r.tls, r.argc, r.errors);
    if (mode[0] == 't') {
        rec_emit(name, "DATA", "case=threads pid=%u threads=%u preemptions=%u increments=%u expected_increments=80000 tokens=%u expected_tokens=10000",
                 pid, r.completed, preemptions, r.increments, r.tokens);
        record_thread_ids(name, &r);
    }
    if (switches_out)
        *switches_out = preemptions;
    if (p->state != PROC_ZOMBIE) {
        proc_stop(p, 99, 0);
        proc_collect();
    }
    if (proc_reap(parent, p))
        pass = false;
    return pass;
}

static void ledger(const char *name, const char *stage, const struct proc_ledger *l,
                    uint32_t pages, uint32_t heap)
{
    rec_emit(name, "DATA", "case=memory_ledger stage=%s pages_free=%u heap_bytes=%u maps=%u backing=%u tables=%u",
             stage, pages, heap, l->extents, l->backing, l->tables);
    rec_emit(name, "DATA", "case=object_ledger stage=%s processes=%u threads=%u zombies=%u handles=%u",
             stage, l->processes, l->threads, l->zombies, l->handles);
}

/* A bounded overlay reader mutates one field of the immutable embedded file.
 * The parser and every mapping still come from the production loader. */
struct bad_field { uint32_t offset, value, bytes; };
static int bad_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    const struct bad_field *bad = cookie;
    int err = image_read(0, off, dst, bytes);
    if (err)
        return err;
    for (unsigned i = 0; i < bad->bytes; i++)
        if (bad->offset + i >= off && bad->offset + i - off < bytes)
            ((uint8_t *)dst)[bad->offset + i - off] = (uint8_t)(bad->value >> (8 * i));
    return 0;
}

int probe_f2_elf_load(void)
{
    const char *name = "elf-load";
    rec_emit(name, "BEGIN", 0);
    struct ciuki_file file = image_file();
    struct elf_image image;
    bool pass = elf_validate(&file, &image) == 0;
    uint8_t digest[32];
    char hex[65];
    sha256(f2_payload_start, file.bytes, digest);
    sha256_hex(digest, hex);
    rec_emit(name, "DATA", "case=image sha256=%s file_bytes=%u load_bytes=%u entry=%08x", hex, file.bytes, image.bytes, image.entry);
    const struct bad_field invalid[] = {
        { 0, 0, 1 }, { 4, 2, 1 }, { 5, 2, 1 }, { 6, 0, 1 }, { 7, 3, 1 }, { 8, 1, 1 },
        { 16, 3, 2 }, { 18, 62, 2 }, { 20, 0, 4 }, { 24, CIUKI_IMAGE_BASE - 1, 4 },
        { 28, UINT32_MAX, 4 }, { 36, 1, 4 }, { 40, 0, 2 }, { 42, 0, 2 }, { 44, 0, 2 },
        { 44, CIUKI_ELF_PHDR_MAX + 1, 2 }, { 52, 2, 4 }, { 52, 3, 4 }, { 52, 7, 4 },
        { 56, UINT32_MAX, 4 }, { 60, CIUKI_IMAGE_BASE + 1, 4 }, { 68, UINT32_MAX, 4 },
        { 72, UINT32_MAX, 4 }, { 76, 7, 4 }, { 80, 1, 4 }, { 92, CIUKI_IMAGE_BASE, 4 }
    };
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        struct ciuki_file bad = file;
        bad.cookie = (void *)&invalid[i];
        bad.read = bad_read;
        int err = elf_validate(&bad, &image);
        rec_emit(name, "DATA", "case=reject index=%u offset=%u expected=%d observed=%d", i, invalid[i].offset, -ENOEXEC, err);
        pass = pass && err == -ENOEXEC;
    }
    for (unsigned i = 0; i < 2; i++) {
        struct ciuki_file bad = file;
        bad.bytes = i ? CIUKI_ELF_BYTES_MAX + 1 : 51;
        int err = elf_validate(&bad, &image);
        rec_emit(name, "DATA", "case=size bytes=%u expected=%d observed=%d", bad.bytes, -ENOEXEC, err);
        pass = pass && err == -ENOEXEC;
    }
    struct process *parent = 0;
    if (make_payload(proc_supervisor(), "p", &parent) < 0)
        pass = false;
    if (parent) {
        pass = run_payload(name, parent, "e", 0) && pass;
        proc_stop(parent, 0, 0);
        proc_collect();
    }
    return finish(name, pass);
}

int probe_f2_spawn_wait(void)
{
    const char *name = "spawn-wait";
    rec_emit(name, "BEGIN", 0);
    const struct ciuki_file_ops *old = proc_get_file_ops();
    proc_set_file_ops(&probe_files);
    struct process *parent = 0;
    bool pass = make_payload(proc_supervisor(), "p", &parent) > 0;
    if (parent) {
        /* Warm allocator classes before measuring physical and heap ledgers. */
        pass = run_payload(name, parent, "s", 0) && pass;
        struct proc_ledger before, after;
        proc_snapshot(&before);
        uint32_t free_before = pmm_free_count(), heap_before = (uint32_t)kheap_in_use();
        ledger(name, "baseline", &before, free_before, heap_before);
        unsigned bad_cycles = 0;
        for (unsigned i = 0; i < 100; i++) {
            bool cycle = run_payload(name, parent, (i & 1) ? "f" : "e", 0);
            proc_snapshot(&after);
            if (!cycle || memcmp(&before, &after, sizeof(before)) ||
                free_before != pmm_free_count() || heap_before != kheap_in_use())
                bad_cycles++;
        }
        ledger(name, "final", &after, pmm_free_count(), (uint32_t)kheap_in_use());
        rec_emit(name, "DATA", "case=cycles cycles=100 bad_cycles=%u leaked=%u", bad_cycles, !!bad_cycles);
        pass = pass && !bad_cycles;
        proc_stop(parent, 0, 0);
        proc_collect();
    }
    proc_set_file_ops(old);
    return finish(name, pass);
}

int probe_f2_mmap(void)
{
    const char *name = "mmap";
    rec_emit(name, "BEGIN", 0);
    struct process *parent = 0;
    bool pass = make_payload(proc_supervisor(), "p", &parent) > 0;
    if (parent) {
        struct uaddr *u = parent->memory;
        struct ciuki_mmap_args args = { .size = sizeof(args), .length = PAGE_SIZE,
            .prot = PROT_NONE, .flags = MAP_PRIVATE | MAP_ANONYMOUS, .fd = -1 };
        const uint32_t rights[] = { PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC };
        for (unsigned i = 0; i < ARRAY_SIZE(rights); i++) {
            args.prot = rights[i];
            int32_t base = ua_mmap(u, &args);
            if ((uint32_t)base >= (uint32_t)-CIUKI_SYSCALL_ERROR_MAX) { pass = false; break; }
            for (unsigned j = 0; j < ARRAY_SIZE(rights); j++) {
                int err = ua_mprotect(u, (uint32_t)base, PAGE_SIZE, rights[j]);
                rec_emit(name, "DATA", "case=transition initial=%u current=%u maximum=%u base=%08x expected=0 observed=%d nx=0",
                         rights[i], rights[j], PROT_READ | PROT_WRITE | PROT_EXEC, (uint32_t)base, err);
                pass = pass && !err;
            }
            pass = !ua_munmap(u, (uint32_t)base, PAGE_SIZE) && pass;
        }
        pass = run_payload(name, parent, "m", 0) && pass;
        pass = run_payload(name, parent, "n", 0) && pass;
        pass = run_payload(name, parent, "r", 0) && pass;
        proc_stop(parent, 0, 0);
        proc_collect();
    }
    return finish(name, pass);
}

int probe_f2_threads_wait(void)
{
    const char *name = "threads-wait";
    rec_emit(name, "BEGIN", 0);
    struct process *parent = 0;
    bool pass = make_payload(proc_supervisor(), "p", &parent) > 0;
    if (parent) {
        uint32_t quantum = g_quantum_ticks;
        uint64_t target = g_switch_target;
        bool stopped = g_measure_stop;
        g_switch_target = 0;
        g_measure_stop = false;
        g_quantum_ticks = 1;
        pass = run_payload(name, parent, "t", 0) && pass;
        g_quantum_ticks = quantum;
        g_switch_target = target;
        g_measure_stop = stopped;
        proc_stop(parent, 0, 0);
        proc_collect();
    }
    return finish(name, pass);
}

CIUKI_F2_PROBE("elf-load", probe_f2_elf_load);
CIUKI_F2_PROBE("spawn-wait", probe_f2_spawn_wait);
CIUKI_F2_PROBE("mmap", probe_f2_mmap);
CIUKI_F2_PROBE("threads-wait", probe_f2_threads_wait);
