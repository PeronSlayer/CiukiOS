/* Reuse f2-02's fake RAM/scheduler fixtures, not its signal adapter. All
 * signal decisions, copies, waits, validators and FPU translations are real.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main proc_fixture_main
#define fpu_task_release fixture_fpu_release
#include "proc_test.c"
#undef main
#undef fpu_task_release

/* Only architectural hardware operations are replaced. */
#define SEL_UCODE 0x1b
#define SEL_UDATA 0x23
#define CR0_MP (1u << 1)
#define CR0_EM (1u << 2)
#define CR0_TS (1u << 3)
#define CR0_NE (1u << 5)
#define CR4_OSFXSR (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)
static uint32_t fake_cr0, fake_cr4;
bool g_cpu_fxsr;
static uint8_t hardware_fp[512];
static unsigned hardware_saves, hardware_restores;
static uint32_t read_cr0(void) { return fake_cr0; }
static uint32_t read_cr4(void) { return fake_cr4; }
static void write_cr0(uint32_t v) { fake_cr0 = v; }
static void write_cr4(uint32_t v) { fake_cr4 = v; }
static void clts(void) { fake_cr0 &= ~CR0_TS; }
void fpu_fxsave(void *p) { CHECK(!(fake_cr0 & CR0_TS)); memcpy(p, hardware_fp, 512); hardware_saves++; }
void fpu_fxrstor(const void *p) { CHECK(!(fake_cr0 & CR0_TS)); memcpy(hardware_fp, p, 512); hardware_restores++; }
void fpu_fnsave(void *p) { CHECK(!(fake_cr0 & CR0_TS)); memcpy(p, hardware_fp, 108); hardware_saves++; memset(hardware_fp, 0, 108); }
void fpu_frstor(const void *p) { CHECK(!(fake_cr0 & CR0_TS)); memcpy(hardware_fp, p, 108); hardware_restores++; }
void fpu_reset_state(int sse) { CHECK(!sse); memset(hardware_fp, 0, sizeof(hardware_fp)); }
#include "../../../src/kernel/proc/sigframe.c"
#include "../../../src/kernel/core/fpu.c"
#include "../../../src/kernel/proc/signal.c"
#include "../../../src/kernel/proc/syscalls_signal.c"

static uint8_t task_fp[CIUKI_THREAD_MAX][512];
static struct proc_thread *signal_process(struct process *parent, struct process **p)
{
    struct proc_thread *t = make_process(parent, p);
    t->task->fpu_area = task_fp[t->slot];
    return t;
}
static struct trap_frame user_frame(struct proc_thread *t)
{
    return (struct trap_frame){ .gs = CIUKI_TLS_SELECTOR, .fs = SEL_UDATA, .es = SEL_UDATA,
        .ds = SEL_UDATA, .edi = 0x12345678, .esi = 0x87654321, .ebp = 0xfeedface,
        .ebx = 17, .edx = 19, .ecx = 23, .eax = (uint32_t)-EINTR, .vector = 0x80,
        .eip = CIUKI_IMAGE_BASE + 8, .cs = SEL_UCODE, .eflags = CIUKI_INITIAL_EFLAGS,
        .user_esp = t->stack_base + t->stack_bytes - 128, .user_ss = SEL_UDATA };
}
static void catch_signal(struct process *p, uint32_t signo)
{
    p->actions[signo] = (struct ciuki_sigaction){ .handler = CIUKI_IMAGE_BASE + 16,
        .flags = SA_SIGINFO, .restorer = CIUKI_IMAGE_BASE + 32 };
}
static void select_task(struct proc_thread *t)
{
    g_current = t->task;
    g_current->state = T_RUNNING;
}
static void destroy_signal_process(struct process *p)
{
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == p && t->task) fpu_task_release(t->task);
    }
    stop_collect(p);
}
static void test_pending(void)
{
    struct process *p, *other;
    struct proc_thread *t = signal_process(proc_supervisor(), &p), *u;
    CHECK(proc_thread_prepare(p, CIUKI_IMAGE_BASE, 0, CIUKI_IMAGE_BASE, CIUKI_THREAD_STACK_MIN,
                              0, 0, false, &u) == 0);
    task_start(u->task); u->task->fpu_area = task_fp[u->slot];
    CHECK(t->tid < u->tid);
    catch_signal(p, SIGUSR1); catch_signal(p, SIGUSR2);
    bool from_process;
    t->mask = u->mask = CIUKI_SIGBIT(SIGUSR1);
    proc_signal_post(p, 0, SIGUSR1, 71); proc_signal_post(p, 0, SIGUSR1, 72);
    CHECK(p->pending == CIUKI_SIGBIT(SIGUSR1) && !proc_signal_caught(t));
    CHECK(process_senders(p)[SIGUSR1] == 71);
    u->mask = 0;
    CHECK(proc_signal_next(u, &from_process) == SIGUSR1 && from_process);
    CHECK(!proc_signal_caught(t));
    t->mask = 0;
    CHECK(proc_signal_next(t, &from_process) == SIGUSR1 && from_process);
    CHECK(!proc_signal_caught(u));
    t->in_handler = true;
    CHECK(!proc_signal_caught(t) && proc_signal_caught(u));
    p->pending = 0;
    proc_signal_post(p, t, SIGUSR2, p->pid); proc_signal_post(p, t, SIGUSR2, p->pid);
    CHECK(t->pending == CIUKI_SIGBIT(SIGUSR2) && !proc_signal_caught(t) && !proc_signal_caught(u));
    t->in_handler = false;
    proc_signal_post(p, t, SIGUSR1, p->pid);
    CHECK(proc_signal_next(t, &from_process) == SIGUSR1 && !from_process);
    t->mask = CIUKI_SIGBIT(SIGUSR1);
    CHECK(proc_signal_next(t, &from_process) == SIGUSR2);
    p->actions[SIGUSR2].handler = SIG_IGN; proc_signal_refresh(p);
    CHECK(!(t->pending & CIUKI_SIGBIT(SIGUSR2)) && !t->interrupted);
    proc_signal_post(p, 0, SIGCHLD, 12); CHECK(!p->pending);
    catch_signal(p, SIGCHLD); proc_signal_child(p, 41);
    CHECK(process_senders(p)[SIGCHLD] == 41 && p->pending == CIUKI_SIGBIT(SIGCHLD));
    p->pending = t->pending = 0;
    signal_process(proc_supervisor(), &other); other->pgid = other->pid;
    CHECK(proc_signal_kill(p, 1, 0) == -EPERM);
    CHECK(proc_signal_kill(p, -1, 0) == -EINVAL);
    CHECK(proc_signal_kill(p, INT32_MIN, 0) == -EPERM);
    CHECK(proc_signal_kill(p, (int32_t)other->pid, 0) == -EPERM);
    CHECK(proc_signal_kill(p, -(int32_t)other->pgid, 0) == -EPERM);
    CHECK(proc_signal_kill(p, CIUKI_ID_MAX, 0) == -ESRCH);
    CHECK(proc_signal_kill(p, 0, 0) == 0 && !p->pending);
    CHECK(proc_signal_thread_kill(p, u->tid, SIGUSR1) == 0 && (u->pending & CIUKI_SIGBIT(SIGUSR1)));
    CHECK(proc_signal_thread_kill(other, u->tid, 0) == -ESRCH);
    CHECK(proc_signal_thread_kill(p, u->tid, 1) == -EINVAL);
    CHECK(proc_signal_kill(p, (int32_t)p->pid, 64) == -EINVAL);
    t->mask = PROC_SIGNAL_SET; t->in_handler = true;
    t->pending |= CIUKI_SIGBIT(SIGKILL);
    CHECK(proc_signal_next(t, &from_process) == SIGKILL);
    proc_signal_refresh(p); CHECK(p->state == PROC_STOPPING && p->status == SIGKILL);
    destroy_signal_process(p); destroy_signal_process(other);
    puts("signal pending: coalescing/senders, lowest-TID/number, masks, handler-active, SIGKILL, authority PASS");
}
static void test_frames(void)
{
    struct process *p;
    struct proc_thread *t = signal_process(proc_supervisor(), &p);
    select_task(t); catch_signal(p, SIGUSR1);
    struct trap_frame tf = user_frame(t), restored;
    tf.eflags |= (1u << 16) | (1u << 18) | (1u << 10); /* RF/AC/DF */
    struct ciuki_siginfo info = { .signo = SIGUSR1, .sender_pid = (int32_t)p->pid };
    struct ciuki_signal_frame frame, edit;
    uint32_t address;
    CHECK(!sigframe_build(t, &tf, &p->actions[SIGUSR1], &info, 0x1234567887654321ull, &frame, &address));
    CHECK(sizeof(frame) == 320 && sizeof(frame.context) == 256 && !(address & 3));
    CHECK(address == tf.user_esp - sizeof(frame));
    CHECK(frame.siginfo_ptr == address + offsetof(struct ciuki_signal_frame, info));
    CHECK(frame.context_ptr == address + offsetof(struct ciuki_signal_frame, context));
    CHECK(frame.context.gregs[CIUKI_REG_ESP] == tf.user_esp && frame.context.gregs[CIUKI_REG_USER_ESP] == tf.user_esp);
    CHECK(frame.context.gregs[CIUKI_REG_EAX] == (uint32_t)-EINTR && frame.token == 0x1234567887654321ull);
    struct sig_active active = { .tid = t->tid, .address = address, .tls = t->tls, .original = frame };
    t->in_handler = true;
    edit = frame; CHECK(!sigframe_validate(t, &active, address, &edit, &restored));
    CHECK(restored.eflags == tf.eflags && restored.eax == tf.eax);
#define BAD(field, value) do { edit = frame; edit.field = (value); CHECK(sigframe_validate(t, &active, address, &edit, &restored) == -EINVAL); } while (0)
    BAD(token, frame.token + 1); BAD(size, frame.size - 1); BAD(version, frame.version + 1);
    BAD(restorer, frame.restorer + 1); BAD(signo, SIGUSR2); BAD(siginfo_ptr, 0); BAD(context_ptr, 0);
    BAD(info.sender_pid, 0); BAD(info.reserved, 1); BAD(context.size, 0); BAD(context.flags, 1);
    BAD(context.link, 1); BAD(context.stack_base, 0); BAD(context.stack_bytes, 0); BAD(context.stack_flags, 1);
    BAD(context.cr2, 1); BAD(context.fp_format, 0);
    const unsigned fixed[] = { CIUKI_REG_GS, CIUKI_REG_FS, CIUKI_REG_ES, CIUKI_REG_DS,
        CIUKI_REG_CS, CIUKI_REG_SS, CIUKI_REG_VECTOR, CIUKI_REG_ERROR };
    for (unsigned i = 0; i < ARRAY_SIZE(fixed); i++) BAD(context.gregs[fixed[i]], 8);
    const unsigned flag_bits[] = { 1, 3, 5, 9, 12, 13, 14, 15, 16, 17, 19, 20, 21, 22, 31 };
    for (unsigned i = 0; i < ARRAY_SIZE(flag_bits); i++)
        BAD(context.gregs[CIUKI_REG_EFLAGS], tf.eflags ^ (1u << flag_bits[i]));
    BAD(context.gregs[CIUKI_REG_EIP], CIUKI_MAIN_STACK_LIMIT);
    BAD(context.gregs[CIUKI_REG_EIP], CIUKI_IMAGE_BASE + PAGE_SIZE);
    BAD(context.gregs[CIUKI_REG_ESP], tf.user_esp - 4);
    edit = frame; edit.context.gregs[CIUKI_REG_ESP] = edit.context.gregs[CIUKI_REG_USER_ESP] = CIUKI_MAIN_STACK_LIMIT;
    CHECK(sigframe_validate(t, &active, address, &edit, &restored) == -EINVAL);
    for (unsigned i = 0; i < sizeof(edit.context.reserved); i++) BAD(context.reserved[i], 1);
    for (unsigned i = 0; i < 64; i++) if (!(PROC_SIGNAL_SET & (UINT64_C(1) << i))) BAD(context.mask, UINT64_C(1) << i);
    const unsigned fp_reserved[] = { 2, 3, 6, 7, 10, 11, 26, 27 };
    for (unsigned i = 0; i < ARRAY_SIZE(fp_reserved); i++) BAD(context.fp_state[fp_reserved[i]], 1);
    BAD(context.fp_state[19], 0x80); BAD(context.fp_state[0], 0xff); BAD(context.fp_state[1], 0x80);
    edit = frame; CHECK(sigframe_validate(t, &active, address + 4, &edit, &restored) == -EINVAL);
    active.tid++; CHECK(sigframe_validate(t, &active, address, &edit, &restored) == -EINVAL); active.tid--;
    active.tls++; CHECK(sigframe_validate(t, &active, address, &edit, &restored) == -EINVAL); active.tls--;
    t->in_handler = false; CHECK(sigframe_validate(t, &active, address, &edit, &restored) == -EINVAL); t->in_handler = true;
    edit = frame;
    edit.context.gregs[CIUKI_REG_EAX] = 17;
    edit.context.gregs[CIUKI_REG_EIP] += 4;
    edit.context.gregs[CIUKI_REG_EFLAGS] ^= USER_FLAGS_EDITABLE;
    edit.context.mask = PROC_SIGNAL_SET;
    memset(edit.context.fp_state + 12, 0xff, 6); memset(edit.context.fp_state + 20, 0xff, 6);
    CHECK(!sigframe_validate(t, &active, address, &edit, &restored));
    CHECK(restored.eax == 17 && !(edit.context.mask & CIUKI_SIGBIT(SIGKILL)));
    CHECK(!sf_word(edit.context.fp_state + 12) && !sf_word(edit.context.fp_state + 20));
    t->in_handler = false;
    tf.user_esp = t->stack_base + sizeof(frame) - 1;
    CHECK(sigframe_build(t, &tf, &p->actions[SIGUSR1], &info, 1, &frame, &address) == -EFAULT);
    tf.user_esp = t->stack_base + sizeof(frame);
    CHECK(!sigframe_build(t, &tf, &p->actions[SIGUSR1], &info, 1, &frame, &address) && address == t->stack_base);
    tf.user_esp += 3;
    CHECK(!sigframe_build(t, &tf, &p->actions[SIGUSR1], &info, 1, &frame, &address) && address == t->stack_base);
    tf.user_esp = 0;
    CHECK(sigframe_build(t, &tf, &p->actions[SIGUSR1], &info, 1, &frame, &address) == -EFAULT);
    destroy_signal_process(p);
    puts("signal frames: ABI layout/token/alignment/exhaustion, every immutable/privileged edit, FPU sanitization PASS");
#undef BAD
}
static void test_fp(void)
{
    uint8_t fx[512], out[108], back[512];
    for (unsigned top = 0; top < 8; top++) {
        memset(fx, 0, sizeof(fx)); fp_put16(fx, 0x037f); fp_put16(fx + 2, (uint16_t)(top << 11));
        /* logical slots: zero, normal, infinity, denormal, unnormal, empty,
         * negative zero, NaN. Physical FTW must follow TOP. */
        for (unsigned i = 0; i < 8; i++) if (i != 5) fx[4] |= (uint8_t)(1u << ((top + i) & 7));
        fx[32 + 16 + 7] = 0x80; fp_put16(fx + 32 + 16 + 8, 0x3fff);
        fx[32 + 32 + 7] = 0x80; fp_put16(fx + 32 + 32 + 8, 0x7fff);
        fx[32 + 48] = 1;
        fx[32 + 64] = 1; fp_put16(fx + 32 + 64 + 8, 0x4000);
        fp_put16(fx + 32 + 96 + 8, 0x8000);
        fx[32 + 112 + 7] = 0xc0; fp_put16(fx + 32 + 112 + 8, 0x7fff);
        const unsigned tags[] = { 1, 0, 2, 2, 2, 3, 1, 2 };
        fpu_signal_to_fnsave(out, fx, true);
        for (unsigned i = 0; i < 8; i++) {
            CHECK(((fp_u16(out + 8) >> (((top + i) & 7) * 2)) & 3) == tags[i]);
            CHECK(!memcmp(out + 28 + i * 10, fx + 32 + i * 16, 10));
        }
        fpu_signal_from_fnsave(back, out, true);
        CHECK(back[4] == fx[4] && fp_u16(back + 2) == fp_u16(fx + 2));
        for (unsigned i = 0; i < 8; i++) CHECK(!memcmp(back + 32 + i * 16, fx + 32 + i * 16, 10));
    }
    for (unsigned mode = 0; mode < 2; mode++) {
        g_cpu_fxsr = mode; fpu_init();
        CHECK((fake_cr0 & (1u << 18)) && !(fake_cr4 & (CR4_OSFXSR | CR4_OSXMMEXCPT)));
        struct task a = { .fpu_area = task_fp[0] }, b = { .fpu_area = task_fp[1] };
        g_current = &a; fpu_handle_nm(); CHECK(fpu_is_owner(&a));
        hardware_fp[mode ? 32 : 28] = 0x5a;
        unsigned saves = hardware_saves;
        g_current = &b; fpu_signal_export(&b, out);
        CHECK(hardware_saves == saves + 1 && !owner && a.fpu_valid);
        CHECK(!out[28] && fp_u16(out) == 0x037f && fp_u16(out + 8) == 0xffff);
        fpu_signal_export(&a, out); CHECK(out[28] == 0x5a);
        g_current = &a; fpu_signal_reset(&a); fpu_handle_nm();
        CHECK(!hardware_fp[mode ? 32 : 28]);
        unsigned restores = hardware_restores;
        fpu_signal_import(&a, out);
        CHECK(!owner && a.fpu_valid && hardware_restores == restores && (fake_cr0 & CR0_TS));
        fpu_handle_nm(); CHECK(hardware_restores == restores + 1 && hardware_fp[mode ? 32 : 28] == 0x5a);
        fpu_task_release(&a);
    }
    g_current = &controller;
    puts("signal x87: all TOP/tag classes, FXSR/FNSAVE, actual lazy owner, first-use/reset/import isolation PASS");
}
static void test_fault_mapping(void)
{
    const struct { uint32_t vector, error, signal, code, address; } cases[] = {
        { 0, 0, SIGFPE, FPE_INTDIV, CIUKI_IMAGE_BASE },
        { 6, 0, SIGILL, ILL_ILLOPC, CIUKI_IMAGE_BASE },
        { 13, 0x28, SIGSEGV, SEGV_ACCERR, CIUKI_IMAGE_BASE },
        { 14, 4, SIGSEGV, SEGV_MAPERR, CIUKI_MMAP_BASE },
        { 14, 7, SIGSEGV, SEGV_ACCERR, CIUKI_MMAP_BASE },
        { 16, 0, SIGFPE, CIUKI_SI_X87, CIUKI_IMAGE_BASE },
        { 17, 0, SIGBUS, BUS_ADRALN, 0 },
        { 19, 0, SIGSEGV, SEGV_ACCERR, CIUKI_IMAGE_BASE },
        { 3, 0, SIGSEGV, SEGV_ACCERR, CIUKI_IMAGE_BASE }
    };
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        struct trap_frame tf = { .vector = cases[i].vector, .err = cases[i].error, .eip = CIUKI_IMAGE_BASE };
        struct ciuki_siginfo info;
        proc_signal_fault_info(&tf, CIUKI_MMAP_BASE, &info);
        CHECK(info.signo == (int32_t)cases[i].signal && info.code == (int32_t)cases[i].code);
        CHECK(info.vector == cases[i].vector && info.trap_error == cases[i].error && info.fault_addr == cases[i].address);
        CHECK(!info.sender_pid && !info.error && !info.reserved);
    }
    puts("signal faults: DE/UD/GP/PF absent+protection/MF/AC/other exact siginfo mapping PASS");
}
static void test_decisions(void)
{
    struct process *p;
    struct proc_thread *t = signal_process(proc_supervisor(), &p);
    select_task(t); catch_signal(p, SIGUSR1);
    unsigned cases = 0;
    for (unsigned caught = 0; caught < 2; caught++) {
        t->pending = 0; t->task->state = T_BLOCKED;
        if (caught) {
            proc_signal_post(p, t, SIGUSR1, p->pid);
            CHECK(t->task->state == T_READY);
        }
        for (unsigned kind = SIGNAL_WAIT_I; kind <= SIGNAL_WAIT_DEFER; kind++)
            for (unsigned commit = 0; commit < 2; commit++)
                for (unsigned issued = 0; issued < 2; issued++)
                    for (unsigned progress = 0; progress < 2; progress++) {
                        enum signal_wait_decision expect = SIGNAL_WAIT_CONTINUE;
                        if (caught && !progress) {
                            if (kind == SIGNAL_WAIT_I) expect = issued ? SIGNAL_WAIT_DRAIN : SIGNAL_WAIT_EINTR;
                            if (kind == SIGNAL_WAIT_D && !commit && !issued) expect = SIGNAL_WAIT_EINTR;
                        }
                        CHECK(proc_signal_decide(caught, kind, commit, issued, progress) == expect);
                        CHECK(proc_signal_wait(t, kind, commit, issued, progress) == expect);
                        cases++;
                    }
    }
    for (unsigned kind = SIGNAL_WAIT_I; kind <= SIGNAL_WAIT_DEFER; kind++) {
        t->mask = CIUKI_SIGBIT(SIGUSR1);
        CHECK(proc_signal_wait(t, kind, false, false, 0) == SIGNAL_WAIT_CONTINUE);
        t->mask = 0; t->in_handler = true;
        CHECK(proc_signal_wait(t, kind, false, false, 0) == SIGNAL_WAIT_CONTINUE);
        t->in_handler = false;
    }
    CHECK(proc_signal_read_result(17, true, false, -EIO) == 17);
    CHECK(proc_signal_read_result(0, true, true, 4096) == -EINTR);
    CHECK(proc_signal_read_result(0, true, false, 4096) == -EIO);
    CHECK(proc_signal_read_result(0, false, true, 4096) == 4096);
    destroy_signal_process(p);
    printf("signal interruption: %u decision rows and pending-signal helper rows, fake scheduler wake, masked/handler suppression, close/dup2 deferral, issued-read drain/result priority PASS\n", cases);
}
static struct proc_thread *wait_target;
static unsigned wait_steps;
static void send_while_waiting(void)
{
    if (++wait_steps == 1) proc_signal_post(wait_target->process, wait_target, SIGUSR2, wait_target->process->pid);
    if (wait_steps == 4) {
        struct ww_key key = wait_target->word_wait.key;
        ww_wake(&key, 1);
    }
}
static void test_delivery(void)
{
    struct process *p;
    struct proc_thread *t = signal_process(proc_supervisor(), &p);
    select_task(t); catch_signal(p, SIGUSR1); catch_signal(p, SIGUSR2);
    struct trap_frame tf = user_frame(t), original = tf;
    t->mask = CIUKI_SIGBIT(SIGINT);
    proc_signal_post(p, t, SIGUSR1, p->pid);
    CHECK(proc_signal_caught(t));
    proc_signal_return_to_user(&tf);
    CHECK(t->in_handler && tf.eip == p->actions[SIGUSR1].handler);
    CHECK((t->mask & (CIUKI_SIGBIT(SIGUSR1) | CIUKI_SIGBIT(SIGINT))) == t->mask);
    uint32_t active_address = tf.user_esp;
    struct ciuki_signal_frame frame;
    CHECK(!ua_read(p->memory, &frame, active_address, sizeof(frame)));
    CHECK(frame.context.gregs[CIUKI_REG_EAX] == original.eax && frame.context.mask == CIUKI_SIGBIT(SIGINT));
    uint32_t word = CIUKI_IMAGE_BASE + PAGE_SIZE + 64;
    put_word(p->memory, word, 0);
    wait_target = t; wait_steps = 0; on_schedule = send_while_waiting;
    CHECK(proc_wait_word(word, 0, 0, CLOCK_MONOTONIC) == 0 && wait_steps == 4);
    on_schedule = 0;
    CHECK(t->pending == CIUKI_SIGBIT(SIGUSR2) && !proc_signal_caught(t));
    proc_signal_return_to_user(&tf); CHECK(tf.user_esp == active_address);
    proc_signal_sigreturn(&tf, active_address);
    CHECK(!t->in_handler && tf.eax == original.eax && tf.eip == original.eip && t->mask == CIUKI_SIGBIT(SIGINT));
    CHECK(proc_signal_caught(t));
    proc_signal_return_to_user(&tf); CHECK(t->in_handler);
    CHECK(!ua_read(p->memory, &frame, tf.user_esp, sizeof(frame)) && frame.signo == SIGUSR2);
    uint64_t last_token = frame.token;
    proc_signal_sigreturn(&tf, tf.user_esp);
    wait_steps = 0; on_schedule = send_while_waiting;
    CHECK(proc_wait_word(word, 0, 0, CLOCK_MONOTONIC) == -EINTR && wait_steps == 1);
    on_schedule = 0;
    tf.eax = (uint32_t)-EINTR;
    proc_signal_return_to_user(&tf);
    CHECK(!ua_read(p->memory, &frame, tf.user_esp, sizeof(frame)) && frame.token != last_token);
    CHECK(frame.context.gregs[CIUKI_REG_EAX] == (uint32_t)-EINTR);
    struct trap_frame sr = tf;
    sr.eax = CIUKI_SYS_SIGRETURN; sr.ebx = tf.user_esp;
    CHECK(proc_signal_syscall(&sr) && sr.eax == (uint32_t)-EINTR && !t->in_handler);
    destroy_signal_process(p);
    puts("signal delivery: frame copy, handler wait no EINTR/nesting, deferred catcher, outside wait EINTR, sigreturn EAX PASS");
}
static void test_syscalls(void)
{
    struct process *p;
    struct proc_thread *t = signal_process(proc_supervisor(), &p);
    select_task(t);
    uint32_t va = CIUKI_IMAGE_BASE + PAGE_SIZE + 64, old = va + 64;
    struct ciuki_sigaction action = { .handler = CIUKI_IMAGE_BASE + 16, .restorer = CIUKI_IMAGE_BASE + 32,
        .flags = SA_SIGINFO, .mask = CIUKI_SIGBIT(SIGKILL) | CIUKI_SIGBIT(SIGUSR2) };
    CHECK(!ua_write(p->memory, va, &action, sizeof(action)));
    CHECK(proc_sigaction(SIGUSR1, va, CIUKI_IMAGE_BASE) == -EFAULT && !p->actions[SIGUSR1].handler);
    CHECK(proc_sigaction(SIGKILL, va, 0) == -EINVAL);
    CHECK(proc_sigaction(SIGKILL, 0, old) == 0);
    CHECK(proc_sigaction(1, va, 0) == -EINVAL);
    CHECK(!proc_sigaction(SIGUSR1, va, old));
    CHECK(p->actions[SIGUSR1].mask == CIUKI_SIGBIT(SIGUSR2));
    struct ciuki_sigaction saved = p->actions[SIGUSR1];
    for (unsigned variant = 0; variant < 5; variant++) {
        action = saved;
        if (variant == 0) action.reserved = 1;
        if (variant == 1) action.flags = 1;
        if (variant == 2) action.mask = 1;
        if (variant == 3) action.handler = CIUKI_IMAGE_BASE + PAGE_SIZE;
        if (variant == 4) action.restorer = CIUKI_MAIN_STACK_LIMIT;
        CHECK(!ua_write(p->memory, va, &action, sizeof(action)));
        CHECK(proc_sigaction(SIGUSR1, va, 0) == -EINVAL && !memcmp(&saved, &p->actions[SIGUSR1], sizeof(saved)));
    }
    uint64_t mask = PROC_SIGNAL_SET;
    CHECK(!ua_write(p->memory, va, &mask, sizeof(mask)));
    CHECK(proc_sigprocmask(SIG_SETMASK, va, CIUKI_IMAGE_BASE) == -EFAULT && !t->mask);
    CHECK(!proc_sigprocmask(SIG_SETMASK, va, old) && t->mask == (PROC_SIGNAL_SET & ~CIUKI_SIGBIT(SIGKILL)));
    CHECK(!proc_sigprocmask(UINT32_MAX, 0, old));
    CHECK(proc_sigprocmask(UINT32_MAX, va, 0) == -EINVAL);
    mask = 1; CHECK(!ua_write(p->memory, va, &mask, sizeof(mask)));
    CHECK(proc_sigprocmask(SIG_BLOCK, va, 0) == -EINVAL);
    mask = CIUKI_SIGBIT(SIGUSR1); CHECK(!ua_write(p->memory, va, &mask, sizeof(mask)));
    proc_signal_post(p, t, SIGUSR1, p->pid); CHECK(!proc_signal_caught(t));
    CHECK(!proc_sigprocmask(SIG_UNBLOCK, va, 0) && proc_signal_caught(t));
    p->actions[SIGPIPE].handler = SIG_IGN; proc_signal_pipe(t); CHECK(!(t->pending & CIUKI_SIGBIT(SIGPIPE)));
    p->actions[SIGPIPE].handler = SIG_DFL; proc_signal_pipe(t); CHECK(t->pending & CIUKI_SIGBIT(SIGPIPE));
    destroy_signal_process(p);
    puts("signal syscalls: single-snapshot validation/no mutation on failure, mask operations, SIGPIPE PASS");
}
static void test_fatal(void)
{
    for (unsigned variant = 0; variant < 8; variant++) {
        struct process *p;
        struct proc_thread *t = signal_process(proc_supervisor(), &p);
        select_task(t); catch_signal(p, SIGSEGV); catch_signal(p, SIGUSR1);
        struct trap_frame tf = user_frame(t);
        tf.vector = 14; tf.err = 4;
        if (variant == 0) p->actions[SIGSEGV].handler = SIG_DFL;
        if (variant == 1) t->mask = CIUKI_SIGBIT(SIGSEGV);
        if (variant == 2) p->actions[SIGSEGV].handler = SIG_IGN;
        if (variant == 3) t->in_handler = true;
        if (variant == 4) tf.user_esp = t->stack_base;
        expect_exit = true;
        if (!setjmp(exited)) {
            if (variant < 5) proc_signal_fault(&tf, CIUKI_MMAP_BASE);
            else if (variant == 5) proc_signal_sigreturn(&tf, tf.user_esp);
            else {
                t->in_handler = true; t->in_syscall = true; t->task->state = T_BLOCKED;
                proc_signal_post(p, t, variant == 6 ? SIGTERM : SIGKILL, p->pid);
                proc_signal_return_to_user(&tf);
            }
            CHECK(false);
        }
        expect_exit = false;
        CHECK(p->state == PROC_STOPPING && p->status == (variant == 6 ? SIGTERM : variant == 7 ? SIGKILL : SIGSEGV));
        t->in_syscall = false;
        destroy_signal_process(p);
    }
    puts("signal fatal: default/blocked/ignored/handler fault, bad stack, forged return, handler TERM/KILL PASS");
}
static void test_signal_payload(const char *path)
{
    FILE *f = fopen(path, "rb"); CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END)); long size = ftell(f);
    CHECK(size > 0 && size < 8 * PAGE_SIZE); rewind(f);
    uint8_t *data = malloc((size_t)size); CHECK(data);
    CHECK(fread(data, 1, (size_t)size, f) == (size_t)size); fclose(f);
    struct ciuki_file file = { .cookie = data, .bytes = (uint32_t)size, .read = real_payload_read };
    struct elf_image image;
    CHECK(!elf_validate(&file, &image) && image.entry == CIUKI_IMAGE_BASE && image.count == 2);
    struct uaddr u; CHECK(!ua_init(&u, &u) && !elf_load(&file, &image, &u));
    CHECK(ua_range(&u, image.entry, 1, PROT_EXEC));
    CHECK(ua_range(&u, CIUKI_IMAGE_BASE + 4 * PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE));
    CHECK(!get_word(&u, CIUKI_IMAGE_BASE + 4 * PAGE_SIZE));
    ua_destroy(&u); free(data);
    printf("signal NASM ELF: bytes=%ld production parser/load PASS (execution not run)\n", size);
}
int main(int argc, char **argv)
{
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller; proc_init();
    g_cpu_fxsr = true; fpu_init();
    test_pending(); test_frames(); test_fp(); test_fault_mapping(); test_decisions();
    test_delivery(); test_syscalls(); test_fatal();
    if (argc == 2) test_signal_payload(argv[1]);
    g_current = &controller;
    struct proc_ledger l; proc_snapshot(&l);
    CHECK(l.processes == 1 && !l.threads && !l.zombies && !pages_used && heap_blocks == 1);
    printf("signal final: checks=%u pages=0 threads=0 zombies=0 PASS\n", checks);
    free(ram);
    return 0;
}
