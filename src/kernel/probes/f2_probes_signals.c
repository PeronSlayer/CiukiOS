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
};
_Static_assert(offsetof(struct signal_result, peer_tid) == 144, "private NASM result layout");

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
    signal_result_read(survivor, &before);
    uint64_t deadline = g_ticks + 2000;
    bool injected = false, completed = false;
    while (g_ticks < deadline && p->state != PROC_ZOMBIE) {
        if (signal_result_read(p, &r)) {
            if (mode == 0 && r.stage > 50 && r.stage <= 57) {
                rec_emit("signals-fault", "DATA", "case=fault-repair part=observed index=%u signal=%u vector=%u code=%u error=%08x",
                         r.stage - 50, r.signo, r.vector, r.code, r.error);
                rec_emit("signals-fault", "DATA", "case=fault-repair part=expected index=%u signal=%u vector=%u code=%u error=%08x",
                         r.stage - 50, r.expect_signo, r.expect_vector, r.expect_code, r.expect_error);
                rec_emit("signals-fault", "DATA", "case=fault-repair part=addresses index=%u address=%08x expected_addr=%08x eip=%08x expected_eip=%08x",
                         r.stage - 50, r.address, r.expect_address, r.eip, r.expect_eip);
                rec_emit("signals-fault", "DATA", "case=fault-repair part=context index=%u pid=%u tid=%u mask=%08x x87_digest=%08x",
                         r.stage - 50, p->pid, r.tid, r.saved_mask, r.fp_digest);
                signal_result_word(p, offsetof(struct signal_result, stage), 0);
            }
            if (!injected && r.tid && (mode == 9 || (mode >= 10 && mode <= 13)) &&
                r.stage == (mode == 9 ? 1u : 2u) &&
                (mode == 9 || g_ticks >= (uint64_t)r.first_tick + 10)) {
                uint32_t sig = mode == 9 || mode == 12 ? SIGKILL : mode == 11 ? SIGTERM : mode == 13 ? SIGUSR1 : SIGUSR2;
                int err = proc_signal_thread_kill(p, r.tid, sig);
                rec_emit("signals-fault", "DATA", "case=%s part=inject pid=%u tid=%u signal=%u mask=%08x expected=0 observed=%d",
                         name, p->pid, r.tid, sig, (uint32_t)proc_thread_find(p, r.tid)->mask, err);
                injected = !err;
            }
            if (!injected && mode == 14 && r.stage == 3 && r.peer_tid) {
                injected = !proc_signal_thread_kill(p, r.peer_tid, SIGUSR2);
                rec_emit("signals-fault", "DATA", "case=%s part=inject pid=%u tid=%u main_tid=%u signal=%u expected=0 observed=%d",
                         name, p->pid, r.peer_tid, r.tid, SIGUSR2, injected ? 0 : -ESRCH);
            }
            if (r.stage == 100) {
                completed = true;
                signal_result_word(p, offsetof(struct signal_result, release), 1);
                break;
            }
        }
        task_sleep_ms(1);
    }
    bool zombie = signal_wait_zombie(p);
    bool pass = zombie && (uint32_t)p->status == expected_status && !r.errors &&
                (expected_status || completed);
    if (mode == 0)
        pass = pass && r.entries == 7 && r.returns == 7;
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
            { "thread-kill-target", 14, 0 }
        };
        for (unsigned i = 0; i < ARRAY_SIZE(cases); i++)
            pass = signal_case(parent, survivor, other, cases[i].name, cases[i].mode, cases[i].status) && pass;
    }
    if (survivor) proc_stop(survivor, 0, 0);
    if (other) proc_stop(other, 0, 0);
    proc_stop(parent, 0, 0);
    /* PID 1 owns parent; the reaper adopts and reaps its children. */
    task_sleep_ms(20);
    rec_emit("signals-fault", "DATA", "case=syscall-interruption status=not_run reason=f2_03_backend_required");
    rec_emit("signals-fault", "END", "status=FAIL reason=%s", pass ? "f2_03_interruption_evidence_pending" : "signal_contract");
    return 1;
#endif
}
CIUKI_F2_PROBE("signals-fault", probe_f2_signals_fault);
