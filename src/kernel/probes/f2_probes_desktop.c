/* Interim ring-3 object-isolation controller; no simulated fault success.
 * Payload execution and progress require the lead's QEMU evidence.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/supervisor.h>
#include <ciuki/probe.h>

#ifdef CIUKI_DESKTOP_PAYLOAD_BIN
__asm__(".pushsection .rodata.desktop_payload,\"a\"\n"
        ".balign 4\n"
        "desktop_payload_start:\n"
        ".incbin \"" CIUKI_DESKTOP_PAYLOAD_BIN "\"\n"
        "desktop_payload_end:\n"
        ".popsection\n");
extern const uint8_t desktop_payload_start[], desktop_payload_end[];
#define DESKTOP_RESULT (CIUKI_IMAGE_BASE + CIUKI_PAGE_SIZE)
struct desktop_result {
    uint32_t mode, stage, errors, turns, ticks;
    int32_t endpoint, victim_endpoint;
    uint32_t release, surface, mapping;
    int32_t display, input;
    uint32_t unauthorized, ack;
};
_Static_assert(offsetof(struct desktop_result, ack) == 52, "private NASM controller layout");
static int desktop_image_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    (void)cookie;
    uint32_t size = (uint32_t)(desktop_payload_end - desktop_payload_start);
    if (off > size || bytes > size - off) return -EIO;
    memcpy(dst, desktop_payload_start + off, bytes);
    return 0;
}
static int desktop_payload(struct process *parent, unsigned mode, int32_t endpoint, struct process **out)
{
    struct proc_strings *strings = proc_strings_new();
    if (!strings) return -ENOMEM;
    int err = proc_strings_add(strings, "standin", sizeof("standin"), false);
    struct ciuki_file file = { .bytes = (uint32_t)(desktop_payload_end - desktop_payload_start), .read = desktop_image_read };
    struct ciuki_spawn_fd inherit = { endpoint, 4 };
    if (!err) err = proc_spawn_file(parent, &file, strings, endpoint < 0 ? 0 : &inherit,
                                    endpoint < 0 ? 0 : 1, CIUKI_SPAWN_NEW_GROUP, 0, out);
    proc_strings_free(strings);
    if (err < 0) return err;
    struct desktop_result initial = { .mode = mode, .endpoint = endpoint < 0 ? -1 : 4,
        .victim_endpoint = -1, .display = -1, .input = -1 };
    ua_write((*out)->memory, DESKTOP_RESULT, &initial, sizeof(initial));
    return err;
}
static bool desktop_result_read(struct process *p, struct desktop_result *r)
{
    return p && p->state == PROC_LIVE && p->memory &&
        !ua_read(p->memory, r, DESKTOP_RESULT, sizeof(*r));
}
static void desktop_word(struct process *p, unsigned offset, uint32_t value)
{
    if (p && p->state == PROC_LIVE) ua_write(p->memory, DESKTOP_RESULT + offset, &value, sizeof(value));
}
static void desktop_remove(struct process *parent, struct process *p)
{
    if (!p) return;
    if (p->state != PROC_ZOMBIE) proc_stop(p, 0, 0);
    uint64_t deadline = g_ticks + 1000;
    while (p->state != PROC_ZOMBIE && g_ticks < deadline) { proc_collect(); task_sleep_ms(1); }
    if (p->state == PROC_ZOMBIE) proc_reap(parent, p);
}
static void desktop_activity_record(struct process *server, int32_t grant, const char *stage)
{
    struct desktop_activity a;
    desktop_activity_snapshot(&a);
    struct ciuki_display_info info;
    int err = grant_display_info(server, grant, &info);
    uint32_t digest = 0;
    if (!err) {
        const struct fb_device *d = fbdev_get();
        digest = fnv1a32(d->mapped, d->size, 2166136261u);
    }
    rec_emit("crash-isolation", "DATA", "case=display server=standin stage=%s presents=%llu input_events=%llu pixel_digest=%08x display_error=%d",
             stage, a.presents, a.input_events, digest, err);
}
static bool desktop_pause(struct process *survivor)
{
    desktop_word(survivor, offsetof(struct desktop_result, ack), 1);
    struct desktop_result r;
    uint64_t deadline = g_ticks + 1000;
    while (g_ticks < deadline && desktop_result_read(survivor, &r)) {
        if (r.stage == 2) return true;
        task_sleep_ms(1);
    }
    return false;
}
static bool desktop_ledgers_equal(const struct desktop_ledger *a, const struct desktop_ledger *b)
{
    return a->descriptions == b->descriptions && a->surfaces == b->surfaces &&
        a->pages == b->pages && a->channels == b->channels && a->messages == b->messages && a->grants == b->grants;
}
#endif

int supervisor_spawn_standin(struct process *parent, struct process **out)
{
#ifdef CIUKI_DESKTOP_PAYLOAD_BIN
    int result = desktop_payload(parent, 0, -1, out);
    if (result < 0) return result;
    int32_t fds[2];
    int err = grants_install(*out, fds);
    if (err) {
        proc_stop(*out, 1, 0);
        if (parent != proc_supervisor()) desktop_remove(parent, *out);
        else proc_collect();
        *out = 0;
        return err;
    }
    desktop_word(*out, offsetof(struct desktop_result, display), (uint32_t)fds[0]);
    desktop_word(*out, offsetof(struct desktop_result, input), (uint32_t)fds[1]);
    return result;
#else
    (void)parent; (void)out;
    return -ENOENT;
#endif
}

int probe_f2_crash_isolation(void)
{
    const char *name = "crash-isolation";
    rec_emit(name, "BEGIN", 0);
#ifndef CIUKI_DESKTOP_PAYLOAD_BIN
    rec_emit(name, "ERROR", "status=not_run reason=missing_standin_payload");
    rec_emit(name, "END", "status=FAIL reason=not_run");
    return 1;
#else
    if (!ua_map_shared) {
        rec_emit(name, "ERROR", "status=not_run reason=missing_shared_mapping_hooks");
        rec_emit(name, "END", "status=FAIL reason=not_run");
        return 1;
    }
    uint8_t hash[32]; char hex[65];
    sha256(desktop_payload_start, (uint32_t)(desktop_payload_end - desktop_payload_start), hash);
    sha256_hex(hash, hex);
    rec_emit(name, "DATA", "case=payload server=standin sha256=%s", hex);
    struct process *parent = 0, *server = 0, *survivor = 0;
    struct desktop_ledger initial, final;
    desktop_snapshot(&initial);
    struct proc_ledger before, after;
    proc_snapshot(&before);
    bool pass = proc_prepare(proc_supervisor(), &parent) == 0;
    if (!pass) goto finished;
    proc_publish(parent, 0); /* kernel controller's child owner, no runnable user thread */
    int32_t pair[2];
    pass = !channel_pair(parent, pair);
    if (!pass) goto cleanup;
    parent->fds[pair[0]].flags = parent->fds[pair[1]].flags = 0;
    pass = desktop_payload(parent, 0, pair[0], &server) > 0;
    if (!pass) goto cleanup;
    int32_t grants[2];
    pass = !grants_install(server, grants);
    if (!pass) goto cleanup;
    desktop_word(server, offsetof(struct desktop_result, display), (uint32_t)grants[0]);
    desktop_word(server, offsetof(struct desktop_result, input), (uint32_t)grants[1]);
    pass = desktop_payload(parent, 1, pair[1], &survivor) > 0;
    desktop_close(parent, pair[0]); desktop_close(parent, pair[1]);
    if (!pass) goto cleanup;
    uint32_t server_pid = server->pid, survivor_pid = survivor->pid;
    uint32_t server_cr3 = server->memory->as.pd_phys, survivor_cr3 = survivor->memory->as.pd_phys;
    rec_emit(name, "DATA", "case=identity server=standin server_pid=%u survivor_pid=%u server_cr3=%08x survivor_cr3=%08x",
             server_pid, survivor_pid, server_cr3, survivor_cr3);
    struct desktop_result sr = { 0 }, cr = { 0 };
    uint64_t deadline = g_ticks + 2000;
    while (g_ticks < deadline && desktop_result_read(survivor, &cr) && cr.turns < 2) task_sleep_ms(1);
    pass = desktop_result_read(server, &sr) && cr.turns >= 2 && !cr.errors && !sr.errors && desktop_pause(survivor);
    desktop_activity_record(server, grants[0], "before");
    unsigned cycles = 0;
    for (; pass && cycles < 100; cycles++) {
        struct desktop_ledger baseline, restored;
        desktop_snapshot(&baseline);
        struct process *victim = 0;
        int32_t ends[2];
        if (channel_pair(parent, ends)) { pass = false; break; }
        parent->fds[ends[1]].flags = 0;
        int server_fd = desktop_fd_slot(server, 0);
        if (server_fd < 0) { pass = false; break; }
        /* Trusted controller moves one endpoint; client receives only the
         * other through production explicit spawn inheritance. */
        server->fds[server_fd] = parent->fds[ends[0]];
        parent->fds[ends[0]] = (struct proc_fd){ 0 };
        desktop_word(server, offsetof(struct desktop_result, ack), 0);
        desktop_word(server, offsetof(struct desktop_result, victim_endpoint), (uint32_t)server_fd);
        unsigned mode = 2 + cycles % 4;
        int made = desktop_payload(parent, mode, ends[1], &victim);
        desktop_close(parent, ends[1]);
        if (made < 0) { pass = false; desktop_close(server, server_fd); break; }
        uint32_t victim_pid = victim->pid, victim_cr3 = victim->memory->as.pd_phys;
        pass = victim_cr3 != server_cr3 && victim_cr3 != survivor_cr3 && victim->pgid != server->pgid;
        bool armed = false;
        struct desktop_result vr = { 0 };
        desktop_word(survivor, offsetof(struct desktop_result, ack), 0);
        deadline = g_ticks + 2000;
        while (g_ticks < deadline && victim->state != PROC_ZOMBIE) {
            if (!armed && desktop_result_read(server, &sr) && sr.ack)
                desktop_word(victim, offsetof(struct desktop_result, ack), 1);
            if (!armed && desktop_result_read(victim, &vr) && vr.stage == 3) {
                pass = pass && !vr.errors && !vr.unauthorized;
                armed = true;
                desktop_word(victim, offsetof(struct desktop_result, ack), 2);
            }
            task_sleep_ms(1);
        }
        int status = victim->state == PROC_ZOMBIE ? victim->status : -1;
        uint32_t expected = mode == 3 ? SIGPIPE : SIGSEGV;
        pass = pass && armed && status == (int)expected;
        /* Closing the receiver drops the pending message and its surface ref. */
        desktop_word(server, offsetof(struct desktop_result, victim_endpoint), UINT32_MAX);
        desktop_close(server, server_fd);
        desktop_remove(parent, victim);
        desktop_result_read(server, &sr); desktop_result_read(survivor, &cr);
        uint32_t start_server = sr.turns, start_client = cr.turns;
        uint64_t start_tick = g_ticks;
        deadline = g_ticks + 2500;
        do {
            task_sleep_ms(1);
            pass = pass && desktop_result_read(server, &sr) && desktop_result_read(survivor, &cr);
        } while (pass && g_ticks < deadline && (sr.turns - start_server < 100 || cr.turns - start_client < 100 || g_ticks - start_tick < 100));
        pass = pass && desktop_pause(survivor);
        desktop_snapshot(&restored);
        pass = pass && server->pid == server_pid && survivor->pid == survivor_pid &&
            server->memory->as.pd_phys == server_cr3 && survivor->memory->as.pd_phys == survivor_cr3 &&
            sr.turns - start_server >= 100 && cr.turns - start_client >= 100 && g_ticks - start_tick >= 100 &&
            !sr.errors && !cr.errors && !cr.unauthorized && desktop_ledgers_equal(&baseline, &restored);
        rec_emit(name, "DATA", "case=victim server=standin cycle=%u pid=%u cr3=%08x mode=%u expected=%u status=%d",
                 cycles + 1, victim_pid, victim_cr3, mode, expected, status);
        rec_emit(name, "DATA", "case=progress cycle=%u server_replies=%u replies=%u ticks=%llu unauthorized_access=%u desktop_restarts=0",
                 cycles + 1, sr.turns - start_server, cr.turns - start_client, g_ticks - start_tick, cr.unauthorized);
        /* Survivor is paused after consuming its reply: both snapshots have
         * no ordinary request in flight, so messages are compared exactly. */
    }
    rec_emit(name, "DATA", "case=cycles server=standin cycles=%u desktop_restarts=0", cycles);
    if (server->state == PROC_LIVE) desktop_activity_record(server, grants[0], "after");
cleanup:
    if (parent) {
        desktop_remove(parent, survivor);
        desktop_remove(parent, server);
        proc_stop(parent, 0, 0);
        proc_collect(); /* PID 1 reaps the kernel controller owner */
    }
finished:
    desktop_snapshot(&final); proc_snapshot(&after);
    pass = pass && desktop_ledgers_equal(&initial, &final) &&
        before.processes == after.processes && before.threads == after.threads &&
        before.handles == after.handles && before.extents == after.extents && before.backing == after.backing;
    rec_emit(name, "DATA", "case=ledger descriptions=%u surfaces=%u pages=%u channels=%u messages=%u grants=%u",
             final.descriptions, final.surfaces, final.pages, final.channels, final.messages, final.grants);
    rec_emit(name, "END", pass ? "status=PASS" : "status=FAIL reason=desktop_contract");
    return pass ? 0 : 1;
#endif
}

int probe_f2_libc_smoke(void)
{
    const char *name = "libc-smoke";
    rec_emit(name, "BEGIN", 0);
    const char *argv[] = { "libc_smoke", 0 };
    struct process *app = 0;
    int result = supervisor_spawn("/bin/libc_smoke", argv, 0, false, &app);
    if (result < 0) {
        rec_emit(name, "ERROR", "status=not_run reason=%s error=%d",
                 result == -ENOENT ? "missing_payload" : "application_setup", result);
        rec_emit(name, "END", "status=FAIL reason=not_run");
        return 1;
    }
    uint32_t pid = app->pid;
    /* Retain exit status using a private child owner until the controller
     * consumes it; orphans still go through PID 1 on controller teardown. */
    struct process *owner = 0;
    int err = proc_prepare(proc_supervisor(), &owner);
    if (err) { proc_stop(app, 1, 0); proc_collect(); rec_emit(name, "END", "status=FAIL reason=controller_memory"); return 1; }
    proc_publish(owner, 0);
    app->ppid = owner->pid;
    supervisor_observe(name, pid);
    rec_emit(name, "DATA", "case=launch pid=%u pgid=%u abi_version=%u", pid, app->pgid, CIUKI_ABI_VERSION);
    uint64_t deadline = g_ticks + 180000;
    while (app->state != PROC_ZOMBIE && g_ticks < deadline) task_sleep_ms(1);
    bool pass = app->state == PROC_ZOMBIE && app->status == 0;
    rec_emit(name, "DATA", "case=wait pid=%u status=%d timeout=%u", pid, app->state == PROC_ZOMBIE ? app->status : -1, g_ticks >= deadline);
    supervisor_observe_end();
    if (app->state != PROC_ZOMBIE) {
        proc_stop(app, 1, 0);
        uint64_t cleanup_deadline = g_ticks + 1000;
        while (app->state != PROC_ZOMBIE && g_ticks < cleanup_deadline) { task_sleep_ms(1); proc_collect(); }
    }
    if (app->state == PROC_ZOMBIE) proc_reap(owner, app);
    proc_stop(owner, 0, 0); proc_collect();
    rec_emit(name, "END", pass ? "status=PASS" : "status=FAIL reason=application_status");
    return pass ? 0 : 1;
}
CIUKI_F2_PROBE("crash-isolation", probe_f2_crash_isolation);
CIUKI_F2_PROBE("libc-smoke", probe_f2_libc_smoke);
