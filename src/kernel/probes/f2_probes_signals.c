/* Interim signal probe: production ELF loader, syscall/fault/return paths.
 * The runner owns selector dispatch and profiles; no alternate kernel path.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/process.h>
#include <ciuki/signal.h>
#include <ciuki/probe.h>
#include <ciuki/sha256.h>

#ifdef CIUKI_SIGNAL_PAYLOAD_BIN
__asm__(".pushsection .rodata.signal_payload,\"a\"\n"
        ".balign 4\n"
        "signal_payload_start:\n"
        ".incbin \"" CIUKI_SIGNAL_PAYLOAD_BIN "\"\n"
        "signal_payload_end:\n"
        ".popsection\n");
extern const uint8_t signal_payload_start[], signal_payload_end[];
#define SIGNAL_RESULT (CIUKI_IMAGE_BASE + 4 * CIUKI_PAGE_SIZE)
struct signal_result {
    uint32_t mode, stage, errors, entries, returns, depth, max_depth;
    uint32_t signo, vector, code, error, address, eip, sender, tid, pid;
    uint32_t peer, other_group, resume, expect_eip, expect_vector, expect_error;
    uint32_t expect_code, expect_address, expect_signo;
    int32_t sleep_result;
    uint32_t first_tick, last_tick, order, saved_eax, resumed_eax, progress;
    uint32_t saved_mask, fp_digest, tls, release, peer_tid;
    uint32_t release_count, completion, handler_completion;
    int32_t handler_remaining;
    uint32_t ac_fallback;
};
_Static_assert(offsetof(struct signal_result, peer_tid) == 144, "private NASM result layout");
_Static_assert(offsetof(struct signal_result, handler_completion) == 156, "private release result layout");
_Static_assert(offsetof(struct signal_result, handler_remaining) == 160, "private remainder result layout");
_Static_assert(offsetof(struct signal_result, ac_fallback) == 164, "private alignment result layout");

/* Fresh supervisor-owned descriptors; the final-release boundary posts a
 * catcher while close/dup2 is executing, then yields before completing. No
 * fault/recovery hook is installed on a real device or in the public ABI. */
static struct signal_release {
    struct proc_object object;
    unsigned references;
    bool inject;
} release_objects[2];
static void signal_release_retain(struct proc_object *object)
{
    ((struct signal_release *)object)->references++;
}
static void signal_release_drop(struct proc_object *object)
{
    struct signal_release *r = (struct signal_release *)object;
    if (--r->references || !r->inject) return;
    struct proc_thread *t = proc_thread_for(g_current);
    if (!t) return;
    proc_signal_thread_kill(t->process, t->tid, SIGUSR1);
    task_sleep_ms(1);
    uint32_t one = 1;
    ua_write(t->process->memory, SIGNAL_RESULT + offsetof(struct signal_result, release_count), &one, sizeof(one));
    ua_write(t->process->memory, SIGNAL_RESULT + offsetof(struct signal_result, completion), &one, sizeof(one));
}

static int signal_image_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    (void)cookie;
    uint32_t size = (uint32_t)(signal_payload_end - signal_payload_start);
    if (off > size || bytes > size - off)
        return -EIO;
    memcpy(dst, signal_payload_start + off, bytes);
    return 0;
}

static struct process *signal_payload(struct process *parent, unsigned mode, uint32_t flags)
{
    struct proc_strings *args = proc_strings_new();
    if (!args)
        return 0;
    int err = proc_strings_add(args, "signals", sizeof("signals"), false);
    struct ciuki_file file = { .bytes = (uint32_t)(signal_payload_end - signal_payload_start), .read = signal_image_read };
    struct process *p = 0;
    if (!err)
        err = proc_spawn_file(parent, &file, args, 0, 0, flags, 0, &p);
    proc_strings_free(args);
    if (err < 0)
        return 0;
    /* The UP kernel has not yielded since publication. */
    ua_write(p->memory, SIGNAL_RESULT, &mode, sizeof(uint32_t));
    if (mode == 16 || mode == 17) {
        for (unsigned i = 0; i < (mode == 17 ? 2u : 1u); i++) {
            release_objects[i] = (struct signal_release){
                .object = { signal_release_retain, signal_release_drop, true },
                .references = 1, .inject = i == 0 };
            p->fds[8 + i] = (struct proc_fd){ &release_objects[i].object, 0 };
        }
    }
    return p;
}

static bool signal_result_read(struct process *p, struct signal_result *r)
{
    return p->state == PROC_LIVE && p->memory && !ua_read(p->memory, r, SIGNAL_RESULT, sizeof(*r));
}

static void signal_result_word(struct process *p, uint32_t offset, uint32_t value)
{
    if (p->state == PROC_LIVE)
        ua_write(p->memory, SIGNAL_RESULT + offset, &value, sizeof(value));
}

static bool signal_wait_zombie(struct process *p)
{
    uint64_t deadline = g_ticks + 1000;
    while (p->state != PROC_ZOMBIE && g_ticks < deadline)
        task_sleep_ms(1);
    return p->state == PROC_ZOMBIE;
}

static void signal_fault_record(const struct signal_result *r, uint32_t pid)
{
    unsigned index = r->stage - 50;
    rec_emit("signals-fault", "DATA", "case=fault-repair part=observed index=%u signal=%u vector=%u code=%u error=%08x",
             index, r->signo, r->vector, r->code, r->error);
    rec_emit("signals-fault", "DATA", "case=fault-repair part=expected index=%u signal=%u vector=%u code=%u error=%08x",
             index, r->expect_signo, r->expect_vector, r->expect_code, r->expect_error);
    rec_emit("signals-fault", "DATA", "case=fault-repair part=addresses index=%u address=%08x expected_addr=%08x eip=%08x expected_eip=%08x",
             index, r->address, r->expect_address, r->eip, r->expect_eip);
    rec_emit("signals-fault", "DATA", "case=fault-repair part=context index=%u pid=%u tid=%u mask=%08x x87_digest=%08x",
             index, pid, r->tid, r->saved_mask, r->fp_digest);
}

static bool signal_case(struct process *parent, struct process *survivor, struct process *other,
                         const char *name, unsigned mode, uint32_t expected_status)
{
    struct process *p = signal_payload(parent, mode, 0);
    if (!p) {
        rec_emit("signals-fault", "DATA", "case=%s expected=spawn observed=failed", name);
        return false;
    }
    signal_result_word(p, offsetof(struct signal_result, peer), survivor->pid);
    signal_result_word(p, offsetof(struct signal_result, other_group), other->pid);
    struct signal_result r = { 0 }, before = { 0 }, after = { 0 };
    struct signal_result faults[7];
    unsigned fault_count = 0;
    bool fault_order = true;
    struct ciuki_timespec remaining = { 0 };
    bool remaining_read = false;
    signal_result_read(survivor, &before);
    uint64_t deadline = g_ticks + 2000;
    bool injected = false, completed = false;
    uint32_t inject_tid = 0, inject_mask = 0, inject_signal = 0;
    int inject_error = 0;
    while (g_ticks < deadline && p->state != PROC_ZOMBIE) {
        if (signal_result_read(p, &r)) {
            if (mode == 0 && r.stage > 50 && r.stage <= 57) {
                /* The handler waits for this acknowledgement. UART polling
                 * and full LFB console redraws must not spend its deadline:
                 * snapshot now, print after completion (f2-22 hardware). */
                fault_order = fault_order && r.stage - 50 == fault_count + 1;
                if (fault_count < ARRAY_SIZE(faults))
                    faults[fault_count++] = r;
                else
                    fault_order = false;
                signal_result_word(p, offsetof(struct signal_result, stage), 0);
            }
            if (!injected && r.tid && (mode == 9 || (mode >= 10 && mode <= 13)) &&
                r.stage == (mode == 9 ? 1u : 2u) &&
                (mode != 13 || proc_thread_find(p, r.tid)->task->state == T_BLOCKED) &&
                (mode == 9 || g_ticks >= (uint64_t)r.first_tick + 10)) {
                uint32_t sig = mode == 9 || mode == 12 ? SIGKILL : mode == 11 ? SIGTERM : mode == 13 ? SIGUSR1 : SIGUSR2;
                inject_tid = r.tid;
                inject_mask = (uint32_t)proc_thread_find(p, r.tid)->mask;
                inject_signal = sig;
                int err = proc_signal_thread_kill(p, r.tid, sig);
                inject_error = err;
                /* Record after completion. The payload's background peers
                 * sleep between progress updates: our 1 ms polling sleep
                 * alone cannot dispatch an awakened P_NORMAL waiter before
                 * its deadline when three peers consume 10 ms quanta. */
                injected = !err;
            }
            if (!injected && mode == 14 && r.stage == 3 && r.peer_tid) {
                injected = !proc_signal_thread_kill(p, r.peer_tid, SIGUSR2);
                rec_emit("signals-fault", "DATA", "case=%s part=inject pid=%u tid=%u main_tid=%u signal=%u expected=0 observed=%d",
                         name, p->pid, r.peer_tid, r.tid, SIGUSR2, injected ? 0 : -ESRCH);
            }
            if (!injected && mode == 15 && r.stage == 2 && r.tid &&
                proc_thread_find(p, r.tid)->task->state == T_BLOCKED)
                injected = !proc_signal_thread_kill(p, r.tid, SIGUSR1);
            if (r.stage == 100) {
                if (mode == 13)
                    remaining_read = !ua_read(p->memory, &remaining, SIGNAL_RESULT + 320, sizeof(remaining));
                completed = true;
                signal_result_word(p, offsetof(struct signal_result, release), 1);
                break;
            }
        }
        task_sleep_ms(1);
    }
    bool zombie = signal_wait_zombie(p);
    if (inject_tid)
        rec_emit("signals-fault", "DATA", "case=%s part=inject pid=%u tid=%u signal=%u mask=%08x expected=0 observed=%d",
                 name, p->pid, inject_tid, inject_signal, inject_mask, inject_error);
    bool pass = zombie && (uint32_t)p->status == expected_status && !r.errors &&
                (expected_status || completed);
    if (mode == 0) {
        pass = pass && fault_order && fault_count == ARRAY_SIZE(faults) &&
               r.entries == 7 && r.returns == 7;
        for (unsigned i = 0; i < fault_count; i++)
            signal_fault_record(&faults[i], p->pid);
        rec_emit("signals-fault", "DATA", "case=fault-repair part=completion completed=%u zombie=%u records=%u state=%u",
                 completed, zombie, fault_count, p->state);
        rec_emit("signals-fault", "DATA", "case=fault-repair part=alignment tcg_fallback=%u hardware_required_vector=17",
                 r.ac_fallback);
    }
    if (mode == 1)
        pass = pass && r.entries == 2 && r.returns == 2;
    if (mode == 10)
        pass = pass && injected && r.entries == 2 && r.returns == 2 && r.max_depth == 1 &&
               !r.sleep_result && r.last_tick - r.first_tick >= 20 && r.order == 3;
    if (mode == 13)
        pass = pass && injected && r.sleep_result == -EINTR && r.saved_eax == (uint32_t)-EINTR &&
               r.resumed_eax == (uint32_t)-EINTR;
    if (mode == 14)
        pass = pass && injected && r.entries == 1 && r.order == 1;
    if (mode == 15)
        pass = pass && injected && r.sleep_result == -EINTR && r.entries == 1 && r.returns == 1 &&
               r.saved_eax == (uint32_t)-EINTR && r.resumed_eax == (uint32_t)-EINTR;
    if (mode == 16 || mode == 17)
        pass = pass && r.entries == 1 && r.returns == 1 && r.release_count == 1 && r.handler_completion == 1 &&
               r.sleep_result == (mode == 16 ? 0 : 8) && r.saved_eax == r.resumed_eax;
    rec_emit("signals-fault", "DATA", "case=%s part=identity pid=%u tid=%u sender=%u",
             name, p->pid, r.tid, r.sender);
    rec_emit("signals-fault", "DATA", "case=%s part=fault signal=%u vector=%u code=%u trap_error=%08x address=%08x eip=%08x",
             name, r.signo, r.vector, r.code, r.error, r.address, r.eip);
    rec_emit("signals-fault", "DATA", "case=%s part=status expected=%u observed=%d raw_vector=%u corruption=%u",
             name, expected_status, p->status, p->fault_vector, r.errors);
    rec_emit("signals-fault", "DATA", "case=%s part=handlers entries=%u returns=%u max_depth=%u order=%u",
             name, r.entries, r.returns, r.max_depth, r.order);
    rec_emit("signals-fault", "DATA", "case=%s part=sleep result=%d elapsed_ms=%u saved_eax=%08x resumed_eax=%08x",
             name, r.sleep_result, r.last_tick - r.first_tick, r.saved_eax, r.resumed_eax);
    rec_emit("signals-fault", "DATA", "case=%s part=context mask=%08x x87_digest=%08x",
             name, r.saved_mask, r.fp_digest);
    if (mode == 13) {
        pass = pass && remaining_read && !remaining.tv_sec && remaining.tv_nsec >= 0 && remaining.tv_nsec <= 20000000 &&
               !remaining.reserved && r.handler_remaining == remaining.tv_nsec;
        rec_emit("signals-fault", "DATA", "case=syscall-interruption syscall=nanosleep remainder_ns=%d handler_remainder_ns=%d result=%d saved_eax=%08x resumed_eax=%08x",
                 remaining.tv_nsec, r.handler_remaining, r.sleep_result, r.saved_eax, r.resumed_eax);
    }
    if (mode >= 15 && mode <= 17)
        rec_emit("signals-fault", "DATA", "case=syscall-interruption syscall=%s result=%d saved_eax=%08x resumed_eax=%08x releases=%u handler_completion=%u",
                 mode == 15 ? "channel_recv" : mode == 16 ? "close" : "dup2",
                 r.sleep_result, r.saved_eax, r.resumed_eax, r.release_count, r.handler_completion);
    if (!zombie) {
        proc_stop(p, 99, 0);
        zombie = signal_wait_zombie(p);
    }
    if (zombie)
        pass = !proc_reap(parent, p) && pass;
    task_sleep_ms(20);
    bool alive = signal_result_read(survivor, &after) && after.progress != before.progress;
    if (mode == 1)
        pass = pass && after.entries == before.entries + 1 && after.sender == r.pid;
    rec_emit("signals-fault", "DATA", "case=%s survivor_pid=%u progress_before=%u progress_after=%u alive=%u",
             name, survivor->pid, before.progress, after.progress, alive);
    return pass && alive;
}
#endif

static int probe_f2_signals_fault(void)
{
    rec_emit("signals-fault", "BEGIN", 0);
#ifndef CIUKI_SIGNAL_PAYLOAD_BIN
    /* A missing canonical build hook is a failure, never a fabricated pass. */
    rec_emit("signals-fault", "END", "status=FAIL reason=signal_payload_not_built");
    return 1;
#else
    uint8_t digest[32];
    char hex[65];
    sha256(signal_payload_start, (uint32_t)(signal_payload_end - signal_payload_start), digest);
    sha256_hex(digest, hex);
    rec_emit("signals-fault", "DATA", "case=image sha256=%s payload=interim_nasm", hex);
    struct process *parent = signal_payload(proc_supervisor(), 20, CIUKI_SPAWN_NEW_GROUP);
    if (!parent) {
        rec_emit("signals-fault", "END", "status=FAIL reason=parent_spawn");
        return 1;
    }
    struct process *survivor = signal_payload(parent, 20, 0);
    struct process *other = signal_payload(parent, 20, CIUKI_SPAWN_NEW_GROUP);
    bool pass = survivor && other;
    if (pass) {
        const struct { const char *name; unsigned mode; uint32_t status; } cases[] = {
            { "fault-repair", 0, 0 }, { "mask-coalesce-group", 1, 0 },
            { "default-fault", 2, SIGSEGV }, { "blocked-fault", 3, SIGSEGV },
            { "ignored-fault", 4, SIGSEGV }, { "bad-stack", 5, SIGSEGV },
            { "handler-fault", 6, SIGILL }, { "forged-token", 7, SIGSEGV },
            { "inactive-sigreturn", 8, SIGSEGV }, { "sigkill", 9, SIGKILL },
            { "handler-blocking", 10, 0 }, { "handler-blocking-term", 11, SIGTERM },
            { "handler-blocking-kill", 12, SIGKILL }, { "nanosleep-eintr", 13, 0 },
            { "thread-kill-target", 14, 0 }, { "channel-eintr", 15, 0 },
            { "close-deferred", 16, 0 }, { "dup2-deferred", 17, 0 }
        };
        for (unsigned i = 0; i < ARRAY_SIZE(cases); i++)
            pass = signal_case(parent, survivor, other, cases[i].name, cases[i].mode, cases[i].status) && pass;
    }
    if (survivor) proc_stop(survivor, 0, 0);
    if (other) proc_stop(other, 0, 0);
    proc_stop(parent, 0, 0);
    /* PID 1 owns parent; the reaper adopts and reaps its children. */
    task_sleep_ms(20);
    unsigned decisions = 0;
    for (unsigned caught = 0; caught < 2; caught++)
        for (unsigned kind = SIGNAL_WAIT_I; kind <= SIGNAL_WAIT_DEFER; kind++)
            for (unsigned committed = 0; committed < 2; committed++)
                for (unsigned issued = 0; issued < 2; issued++)
                    for (unsigned progress = 0; progress < 2; progress++) {
                        enum signal_wait_decision expected = SIGNAL_WAIT_CONTINUE;
                        if (caught && !progress && kind != SIGNAL_WAIT_DEFER) {
                            if (kind == SIGNAL_WAIT_I) expected = issued ? SIGNAL_WAIT_DRAIN : SIGNAL_WAIT_EINTR;
                            else if (!committed && !issued) expected = SIGNAL_WAIT_EINTR;
                        }
                        enum signal_wait_decision actual = proc_signal_decide(caught, kind, committed, issued, progress);
                        rec_emit("signals-fault", "DATA", "case=syscall-interruption layer=decision class=%u caught=%u committed=%u issued=%u progress=%u expected=%u observed=%u",
                                 kind, caught, committed, issued, progress, expected, actual);
                        pass = pass && actual == expected;
                        decisions++;
                    }
    rec_emit("signals-fault", "DATA", "case=syscall-interruption layer=decision rows=%u close=defer dup2=defer issued_read=drain partial=positive", decisions);
    rec_emit("signals-fault", "END", pass ? "status=PASS" : "status=FAIL reason=signal_contract");
    return pass ? 0 : 1;
#endif
}
CIUKI_F2_PROBE("signals-fault", probe_f2_signals_fault);
