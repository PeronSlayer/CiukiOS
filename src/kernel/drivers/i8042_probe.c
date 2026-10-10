/* F1 input evidence through the shared native/firmware queue.
 * Protocol references and production decisions are recorded in i8042.c.
 * Fault fixtures below never read/write physical ports or PIC/PIT state.
 * Mediated-I/O fault evidence is supplied by biosvm_selftest (f1-07);
 * the native fault PASS does not qualify those firmware fault cases.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/probe.h>
#include <ciuki/i8042.h>
#include <ciuki/init.h>
#include <ciuki/fwinput.h>
#include <ciuki/biosvm.h>

static int input_verdict(const char *probe, bool ok, const char *reason)
{
    if (ok) rec_emit(probe, "END", "status=PASS");
    else rec_emit(probe, "END", "status=FAIL reason=%s", reason);
    return ok ? 0 : 1;
}

int probe_input(void)
{
    rec_emit("input", "BEGIN", 0);
    int err = i8042_init();
    bool firmware = g_boot.input_policy == CBI_INPUT_FIRMWARE || (g_boot.flags & CBI_F_INPUT_FORCED);
    const char *backend = firmware ? "firmware" : "native";
    if (firmware && err == -ENOSYS) err = fwinput_adapter_init();
    struct fwinput_backend_state fw = { 0 };
    if (firmware) {
        fwinput_backend_state(&fw);
        if (!err && (!fw.keyboard || !fw.mouse || !fw.key_releases || fw.disabled)) err = -ENOSYS;
    }
    if (err) {
        rec_emit("input", "DATA", "case=setup backend=%s error=%d", backend, err);
        return input_verdict("input", false, "setup");
    }
    struct input_event event;
    while (input_read(&event)) ;       /* boot-time bytes are outside the stimulus */
    struct input_stats base, q;
    struct i8042_stats driver;
    input_snapshot(&base);
    i8042_snapshot(&driver);
    struct input_digest d;
    input_digest_init(&d);
    rec_emit("input", "READY", "backend=%s", backend);
    rec_emit("input", "ARM", "keys=100 moves=100 buttons=10 x=2 y=-1 timeout_ms=120000 generation=%u",
             driver.generation);
    uint64_t end = deadline_after_ms(120000), quiet = deadline_after_ms(0);
    uint64_t diagnostic_at = deadline_after_ms(10000);
    bool complete = false;
    while (!deadline_passed(end)) {
        if (firmware && deadline_passed(diagnostic_at)) {
            fwinput_adapter_log_delivery();
            diagnostic_at = deadline_after_ms(10000);
        }
        for (unsigned n = 0; n < INPUT_CAPACITY && input_read(&event); n++) {
            input_digest_add(&d, &event);
            quiet = deadline_after_ms(50);
        }
        if (d.characters >= 100 && d.key_transitions >= 200 && d.button_transitions >= 20 &&
            d.x >= 200 && d.y <= -100 && deadline_passed(quiet)) {
            complete = true;
            break;
        }
        task_sleep_ms(1);
    }
    input_snapshot(&q);
    /* Expected text digest is exactly 100 unshifted 'a' characters. */
    struct input_digest expected;
    input_digest_init(&expected);
    struct input_event a = { .type = INPUT_TEXT, .code = 'a' };
    for (unsigned i = 0; i < 100; i++) input_digest_add(&expected, &a);
    rec_emit("input", "DATA", "case=counts backend=%s generation=%u characters=%llu key_transitions=%llu button_transitions=%llu",
             backend, driver.generation, d.characters, d.key_transitions, d.button_transitions);
    rec_emit("input", "DATA", "case=motion x=%lld y=%lld digest=%08x text_digest=%08x events=%llu",
             d.x, d.y, d.hash, d.text_hash, d.events);
    rec_emit("input", "DATA", "case=queue overflow=%llu resync=%llu duplicates=%llu repeats=%llu errors=%llu stuck_keys=%u buttons=%u state_lost=%u",
             q.overflow - base.overflow, q.resync - base.resync, q.duplicates - base.duplicates,
             q.repeats - base.repeats, q.errors - base.errors, q.keys_down, q.buttons, q.state_lost);
    rec_emit("input", "DATA", "group=input backend=%s text_count=%llu key_transitions=%llu button_transitions=%llu motion_x=%lld motion_y=%lld",
             backend, d.characters, d.key_transitions, d.button_transitions, d.x, d.y);
    rec_emit("input", "DATA", "group=input loss=%llu duplicates=%llu stuck=%u owner_errors=%u",
             q.overflow - base.overflow, q.duplicates - base.duplicates,
             q.keys_down + !!q.buttons, !driver.active || driver.quarantined);
    rec_emit("input", "DATA", "group=metadata subcase=stimulus owner=%s generation=%u errors=%llu gate=input timing_domain=%s",
             firmware ? "firmware-input" : "i8042", driver.generation, q.errors - base.errors,
             (g_boot.flags & CBI_F_SMBIOS_QEMU) ? "icount" : "hardware");
    bool ok = complete && !base.keys_down && !base.buttons && !base.state_lost &&
              d.characters == 100 && d.text_hash == expected.text_hash && d.key_transitions == 200 &&
              d.button_transitions == 20 && d.x == 200 && d.y == -100 &&
              q.overflow == base.overflow && q.resync == base.resync && q.duplicates == base.duplicates &&
              q.repeats == base.repeats && q.errors == base.errors && !q.keys_down && !q.buttons && !q.pending;
    if (firmware) {
        fwinput_adapter_log_delivery();
        struct fwinput_stats stats;
        fwinput_stats(&stats);
        fwinput_backend_state(&fw);
        rec_emit("input", "DATA", "case=lease backend=firmware persistent=1 key_releases=%u disabled=%u scan_bytes=%llu aux_bytes=%llu",
                 fw.key_releases, fw.disabled, stats.scan_bytes, stats.aux_bytes);
        ok = ok && !fw.disabled && fw.key_releases;
    }
    /* Input remains the boot backend for the following safe probe and
     * ordinary operation. Never tear down the firmware's persistent lease. */
    rec_emit("input", "DATA", "case=lease backend=%s generation=%u retained=1 active=%u quarantined=%u",
             backend, driver.generation, driver.active, driver.quarantined);
    return input_verdict("input", ok, "stimulus_or_lease");
}

/* A bounded controller-boundary script. ACKs are published only after
 * writes; status carries AUX even when keyboard traffic is interleaved. */
enum fault_mode { FAULT_ACK, FAULT_MISSING, FAULT_RESEND, FAULT_EXHAUST, FAULT_MIXED };
struct fault_bus {
    struct { uint8_t status, byte; } bytes[16];
    unsigned head, count, sends, resets;
    enum fault_mode mode;
    bool aux;
    uint64_t tick;
};
static void fault_push(struct fault_bus *b, bool aux, uint8_t byte)
{
    if (b->count < ARRAY_SIZE(b->bytes)) {
        unsigned at = (b->head + b->count++) % ARRAY_SIZE(b->bytes);
        b->bytes[at].status = aux ? 0x21 : 0x01;
        b->bytes[at].byte = byte;
    }
}
static uint8_t fault_read(void *arg, uint16_t port)
{
    struct fault_bus *b = arg;
    if (port == 0x64) return b->count ? b->bytes[b->head].status : 0;
    if (!b->count) return 0;
    uint8_t byte = b->bytes[b->head].byte;
    b->head = (b->head + 1) % ARRAY_SIZE(b->bytes);
    b->count--;
    return byte;
}
static void fault_write(void *arg, uint16_t port, uint8_t byte)
{
    struct fault_bus *b = arg;
    if (port == 0x64) {
        if (byte == 0xD1 || byte >= 0xF0) b->resets++;
        b->aux = byte == 0xD4;
        return;
    }
    if (byte == 0xFF) b->resets++;
    b->sends++;
    if (b->mode == FAULT_MISSING) return;
    if (b->mode == FAULT_MIXED) {
        fault_push(b, false, 0x1C);
        fault_push(b, true, 0x08);
        fault_push(b, true, 0x02);
        fault_push(b, true, 0x01);
        fault_push(b, false, 0xF0);
        fault_push(b, false, 0x1C);
    }
    bool resend = b->mode == FAULT_EXHAUST || (b->mode == FAULT_RESEND && b->sends <= 2);
    fault_push(b, b->aux, resend ? 0xFE : 0xFA);
}
static uint64_t fault_now(void *arg) { return ((struct fault_bus *)arg)->tick; }
static void fault_pause(void *arg) { ((struct fault_bus *)arg)->tick++; }
static const struct i8042_test_io fault_io = { fault_read, fault_write, fault_now, fault_pause };

/* Reuse the F0 ring-3 survivor payload (id 7) and its sentinel/error words.
 * Each successful mapping transfers page ownership to the address space;
 * reap unwinds all mapped pages and the stack, including partial failure. */
extern const uint8_t payload_start[], payload_end[];
struct input_survivor { struct task *task; uint32_t data; };
static void survivor_finish(struct input_survivor *s)
{
    if (!s->task) return;
    if (task_alive(s->task)) task_kill(s->task, -1);
    task_reap(s->task);
    s->task = 0;
}
static bool survivor_start(struct input_survivor *s)
{
    s->task = task_create_user("input-survivor", P_NORMAL, 0x00401000u, 0xBFFFFFF0u, 7, 0x5EED5EEDu, 0);
    if (!s->task) return false;
    const uint32_t va[] = { 0x00400000u, 0x00401000u, 0xBFFFF000u };
    for (unsigned i = 0; i < ARRAY_SIZE(va); i++) {
        uint32_t p = pmm_alloc();
        if (!p) { survivor_finish(s); return false; }
        memset(P2V(p), 0, PAGE_SIZE);
        if (i == 1) {
            if ((size_t)(payload_end - payload_start) > PAGE_SIZE) {
                pmm_free(p); survivor_finish(s); return false;
            }
            memcpy(P2V(p), payload_start, (size_t)(payload_end - payload_start));
        }
        if (as_map(&s->task->as, va[i], p, PTE_U | (i == 1 ? 0 : PTE_W)) < 0) {
            pmm_free(p); survivor_finish(s); return false;
        }
        if (!i) s->data = p;
    }
    task_start(s->task);
    task_sleep_ms(10);
    return true;
}
static bool survivor_progress(struct input_survivor *s, const char *name)
{
    volatile uint32_t *data = P2V(s->data);
    uint32_t before = data[0];
    uint64_t start = deadline_after_ms(0);
    task_sleep_ms(110);
    uint64_t ticks = deadline_after_ms(0) - start;
    bool ok = task_alive(s->task) && data[0] != before && !data[1] && data[2] == 0x5EED5EEDu && ticks >= 100;
    rec_emit("input-fault", "DATA", "case=%s survivor_ticks=%llu survivor_progress=%u survivor_ok=%u",
             name, ticks, data[0] - before, ok);
    return ok;
}

static int fault_begin_failed(struct input_survivor *s, int err)
{
    survivor_finish(s);
    rec_emit("input-fault", "END", "status=FAIL reason=fault_begin:%d", err);
    return 1;
}

int probe_input_fault(void)
{
    rec_emit("input-fault", "BEGIN", 0);
    struct input_survivor survivor = { 0 };
    if (!survivor_start(&survivor)) return input_verdict("input-fault", false, "survivor_spawn");
    static const struct { const char *name; enum fault_mode mode; int error; unsigned sends, resends; } cases[] = {
        { "missing_ack", FAULT_MISSING, -I8042_ETIMEDOUT, 1, 0 },
        { "bounded_resend", FAULT_RESEND, 0, 3, 2 },
        { "resend_exhausted", FAULT_EXHAUST, -I8042_EPROTO, 3, 2 },
        { "mixed_aux_key", FAULT_MIXED, 0, 1, 0 },
    };
    bool ok = true;
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        struct fault_bus bus = { .mode = cases[i].mode };
        int err = i8042_fault_begin(&fault_io, &bus);
        if (err) return fault_begin_failed(&survivor, err);
        rec_emit("input-fault", "ARM", "case=%s boundary=scripted deadline_ms=%u", cases[i].name, I8042_REPLY_MS);
        /* Mixed stream targets the keyboard: AUX packet bytes are never
         * guessed to be its ACK, while unsolicited key transitions survive. */
        err = i8042_fault_command(false, 0xF4);
        struct i8042_stats driver;
        struct input_stats q;
        i8042_fault_snapshot(&driver, &q);
        struct input_digest d;
        struct input_event e;
        input_digest_init(&d);
        while (i8042_fault_read(&e)) input_digest_add(&d, &e);
        bool c_ok = err == cases[i].error && bus.sends == cases[i].sends &&
                    driver.resends == cases[i].resends && driver.last_elapsed_ms <= I8042_REPLY_MS &&
                    !driver.pending_command && !bus.resets && driver.quarantined == (err != 0);
        if (cases[i].mode == FAULT_MISSING) c_ok = c_ok && driver.last_elapsed_ms == I8042_REPLY_MS && driver.timeouts == 1;
        if (cases[i].mode == FAULT_MIXED)
            c_ok = c_ok && d.characters == 1 && d.key_transitions == 2 && d.x == 2 && d.y == -1 &&
                   !q.keys_down && !q.resync && !q.overflow;
        if (err) {
            uint64_t writes = driver.writes, reads = driver.reads;
            c_ok = c_ok && i8042_fault_command(false, 0xF4) == -I8042_EIO;
            i8042_fault_snapshot(&driver, 0);
            c_ok = c_ok && driver.writes == writes && driver.reads == reads;
        }
        rec_emit("input-fault", "DATA", "case=%s owner=fixture generation=%u error=%d elapsed_ms=%u resends=%llu quarantined=%u pending=%u resets=%u ok=%u",
                 cases[i].name, driver.generation, err, driver.last_elapsed_ms, driver.resends,
                 driver.quarantined, driver.pending_command, bus.resets, c_ok);
        rec_emit("input-fault", "DATA", "case=%s timing=scripted_ms controller_reads=%llu controller_writes=%llu physical_claims=0 keys=%llu x=%lld y=%lld",
                 cases[i].name, driver.reads, driver.writes, d.key_transitions, d.x, d.y);
        i8042_fault_end();
        bool alive = survivor_progress(&survivor, cases[i].name);
        ok = ok && c_ok && alive;
    }
    struct fault_bus bus = { 0 };
    int err = i8042_fault_begin(&fault_io, &bus);
    if (err) return fault_begin_failed(&survivor, err);
    else {
        rec_emit("input-fault", "ARM", "case=malformed_packet boundary=scripted");
        const uint8_t bytes[] = { 0x00, 0xC8, 0x00, 0x08, 0x02, 0x01 };
        for (unsigned i = 0; i < ARRAY_SIZE(bytes); i++) i8042_fault_capture(0x21, bytes[i]);
        struct input_stats q;
        i8042_fault_snapshot(0, &q);
        struct input_digest d;
        struct input_event e;
        input_digest_init(&d);
        while (i8042_fault_read(&e)) input_digest_add(&d, &e);
        bool c_ok = q.resync == 3 && !q.overflow && d.x == 2 && d.y == -1;
        rec_emit("input-fault", "DATA", "case=malformed_packet resync=%llu x=%lld y=%lld ok=%u", q.resync, d.x, d.y, c_ok);
        i8042_fault_end();
        bool alive = survivor_progress(&survivor, "malformed_packet");
        ok = ok && c_ok && alive;
    }
    err = i8042_fault_begin(&fault_io, &bus);
    if (err) return fault_begin_failed(&survivor, err);
    else {
        rec_emit("input-fault", "ARM", "case=queue_overflow boundary=scripted capacity=256");
        for (unsigned i = 0; i < 130; i++) {
            i8042_fault_capture(0x01, 0x1C);
            i8042_fault_capture(0x01, 0xF0);
            i8042_fault_capture(0x01, 0x1C);
        }
        struct input_stats q;
        i8042_fault_snapshot(0, &q);
        struct input_event e;
        bool resync = i8042_fault_read(&e) && e.type == INPUT_RESYNC &&
                      e.flags == INPUT_F_RESYNC && e.lost_count == INPUT_CAPACITY &&
                      !e.value && e.sequence == 257;
        unsigned drained = resync ? 1 : 0;
        uint64_t sequence = 257;
        bool fresh = true;
        while (i8042_fault_read(&e)) {
            drained++;
            if (e.type == INPUT_RESYNC || e.flags || e.lost_count || e.sequence != ++sequence) fresh = false;
        }
        bool c_ok = q.overflow == INPUT_CAPACITY && q.pending == 135 && q.resync == 1 &&
                    q.state_lost && !q.keys_down && drained == 135 && resync && fresh;
        i8042_fault_capture(0x01, 0x1C);
        fresh = fresh && i8042_fault_read(&e) && e.type == INPUT_KEY && e.value == 1 &&
                !e.flags && e.sequence == 392;
        c_ok = c_ok && fresh;
        rec_emit("input-fault", "DATA", "case=queue_overflow overflow=%llu drained=%u state_lost=%u resync_marked=%u fresh=%u ok=%u",
                 q.overflow, drained, q.state_lost, resync, fresh, c_ok);
        i8042_fault_end();
        bool alive = survivor_progress(&survivor, "queue_overflow");
        ok = ok && c_ok && alive;
    }
    survivor_finish(&survivor);
    return input_verdict("input-fault", ok, "fault_or_survivor");
}

CIUKI_F1_PROBE("input", probe_input);
CIUKI_F1_PROBE("input-fault", probe_input_fault);
