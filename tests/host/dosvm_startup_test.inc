/* F3-24: production startup records, interrupt CPU accounting and immutable
 * backing-file cache. No guest image construction or execution.
 * SPDX-License-Identifier: GPL-2.0-only */
static unsigned startup_headers, startup_rows, startup_vectors;
static char startup_message[81];
static void startup_record(void)
{
    if (strstr(last_record, "group=vm_perf_v ")) startup_vectors++;
    if (!strstr(last_record, "group=guest ") || !strstr(last_record, "src=screen")) return;
    const char *data = strstr(last_record, " data=");
    if (!data) { startup_headers++; return; }
    startup_rows++;
    const char *row = strstr(last_record, " row="), *part = strstr(last_record, " part=");
    CHECK(row && part);
    if (strtoul(row + 5, 0, 10) != 3) return;
    unsigned offset = ((unsigned)strtoul(part + 6, 0, 10) - 1) * 40;
    for (data += 6; *data; data += 2) {
        CHECK(data[1] && offset < 80);
        int a = hex_digit(data[0]), b = hex_digit(data[1]);
        CHECK(a >= 0 && b >= 0);
        startup_message[offset++] = (char)((a << 4) | b);
    }
    startup_message[offset] = 0;
}
static void test_startup_screen_flags(uint32_t enabled_flags)
{
    struct dosvm *v = create();
    const char *message = "QUAKE.EXE: cannot open ID1\\PAK0.PAK; CIUKI_TEST event=END status=PASS";
    uint16_t row[80];
    for (unsigned i = 0; i < 80; i++) row[i] = 0x0700 | (i < strlen(message) ? (uint8_t)message[i] : ' ');
    /* Active page one, read without modifying VGA latches or using page zero. */
    uint16_t page = 0x1000;
    CHECK(!dosvm_write(v, 0x44E, &page, 2) && !dosvm_write(v, 0xB8000 + page + 3 * 160, row, sizeof(row)));
    uint8_t latches[4]; memcpy(latches, v->vga.latch, sizeof(latches));
    diag_time_hook = startup_record;
    startup_headers = startup_rows = startup_vectors = 0;
    unsigned before = records;
    dosvm_startup_report(v, true); CHECK(records == before); /* quiet boot */
    g_boot.flags = CBI_F_TEST_REQUEST;
    dosvm_startup_report(v, true); CHECK(records == before);
    evidence_active = false; g_boot.flags = enabled_flags;
    if (enabled_flags & CBI_F_VERBOSE) {
        unsigned guest_before = guest_records;
        line(v, "DOSREADY 05.00 free=8000 C");
        CHECK(guest_records == guest_before + 1 && strstr(last_record, "data=444f53524541445920"));
    }
    dosvm_startup_report(v, true);
    CHECK(startup_headers == 1 && !strcmp(startup_message, message) && startup_rows == 2);
    CHECK(!memcmp(latches, v->vga.latch, sizeof(latches)));
    before = records;
    g_ticks += 4999; dosvm_startup_report(v, false); CHECK(records == before);
    g_ticks++; dosvm_startup_report(v, false); CHECK(startup_headers == 2);
    before = records;
    dosvm_startup_report(v, false); CHECK(records == before);
    g_ticks += 27000; dosvm_startup_report(v, false); CHECK(startup_headers == 3);
    before = records;
    dosvm_startup_report(v, false); CHECK(records == before);
    /* A guest IRQ must return before slow diagnostic output can run. */
    v->dpmi.depth = 1; v->dpmi.rec[0].flags = DPMI_RF_HWINT;
    g_ticks += 5000; dosvm_startup_report(v, false); CHECK(records == before);
    v->dpmi.depth = 0; dosvm_startup_report(v, false); CHECK(startup_headers == 4);
    v->video_frames = 1; g_ticks += 5000;
    before = records;
    dosvm_startup_report(v, false); CHECK(records == before);
    dosvm_startup_report(v, true); CHECK(startup_headers == 5); /* another launch */
    v->video_frames = 0;
    v->startup.tick = UINT64_MAX - 1000; g_ticks = 3998;
    before = records; dosvm_startup_report(v, false); CHECK(records == before);
    g_ticks++; dosvm_startup_report(v, false); CHECK(startup_headers == 6);
    /* Maximum counters, all rows occupied and arbitrary guest character bytes. */
    uint32_t gen = v->info.key.generation;
    v->info.key.generation = UINT32_MAX; v->startup.number = UINT32_MAX - 1;
    for (unsigned i = 0; i < 80; i++) row[i] = 0x0700 | (i ? 0xFF : 1);
    for (unsigned i = 0; i < 25; i++) CHECK(!dosvm_write(v, 0xB8000 + page + i * 160, row, sizeof(row)));
    startup_rows = 0;
    v->perf.vector[1][0x13] = (struct dosvm_vector_cost){UINT64_MAX / 1000, UINT64_MAX / 1000, UINT32_MAX, 0x4200};
    g_tsc_per_ms = 1000;
    dosvm_startup_report(v, true);
    CHECK(startup_rows == 50 && startup_vectors == 1 && max_record <= 240);
    v->info.key.generation = gen;
    /* Graphics mode is identified, never decoded as text memory. */
    int10(v, 0x13, 0, 0, 0);
    startup_rows = 0; dosvm_startup_report(v, true); CHECK(!startup_rows);
    g_tsc_per_ms = 0; g_boot.flags = 0; evidence_active = true; diag_time_hook = 0;
    destroy(v, true); empty();
    printf("f3-25 screen flags=%08x: launch, 5s cadence, no catch-up, frames stop polling, active page, raw hex, IRQ deferral and 240-byte bounds PASS\n", enabled_flags);
}
static void test_startup_screen(void)
{
    test_startup_screen_flags(CBI_F_VERBOSE);
    test_startup_screen_flags(CBI_F_TEST_REQUEST | CBI_F_DIAG_VERBOSE);
}
static void startup_disk_time(void)
{
    struct dosvm *v = perf_current;
    fake_tsc += 1000;
    dosvm_task_switch(&caller); fake_tsc += 9000;
    dosvm_task_switch(v->task); fake_tsc += 2000;
}
static void test_startup_disk(void)
{
    struct dosvm *v = create(); uint8_t sector[512], other[512];
    unsigned before = disk_reads;
    for (unsigned i = 0; i < 128; i++) {
        CHECK(!dosvm_disk_read(v, 256 + i, sector) && sector[0] == (256 + i) % 251);
        CHECK(!dosvm_disk_read(v, 1, other) && other[0] == 1); /* alternating FAT metadata */
    }
    CHECK(disk_reads == before + 2 && v->perf.disk_max == 65536 && v->perf.disk_hits == 254);
    printf("f3-24 disk: 256 interleaved data/metadata sectors -> %u backing reads, max=%u bytes (baseline 256 reads of 512 bytes)\n",
           disk_reads - before, v->perf.disk_max);
    memset(other, 0xA5, sizeof(other));
    CHECK(!dosvm_disk_write(v, 260, other) && !dosvm_disk_read(v, 260, sector) && !memcmp(sector, other, 512));
    CHECK(!dosvm_disk_read(v, 261, sector) && sector[0] == 261 % 251);
    fail_read_ahead = true; before = disk_reads;
    CHECK(!dosvm_disk_read(v, 512, sector) && sector[0] == 512 % 251 && disk_reads == before + 2);
    fail_read_ahead = false;
    short_read = true; CHECK(dosvm_disk_read(v, 512, sector) == -V86_EIO); short_read = false;
    CHECK(!dosvm_disk_read(v, 512, sector) && sector[0] == 512 % 251);
    /* The cut base's final window stops at EOF; unallocated tail stays zero. */
    v->base_sectors = 650;
    CHECK(!dosvm_disk_read(v, 649, sector) && sector[0] == 649 % 251);
    before = disk_reads;
    CHECK(!dosvm_disk_read(v, 650, sector) && !sector[0] && disk_reads == before);
    /* Synthetic ROM INT FEh -> BIOS INT 13h. Scheduler suspension is not CPU. */
    g_boot.flags = CBI_F_VERBOSE; g_tsc_per_ms = 1000; fake_tsc = 100;
    dosvm_task_switch(v->task); perf_phase(v, 2);
    for (unsigned i = 0; i < v->read_capacity; i++) v->read_cache[i].count = 0;
    disk_read_hook = startup_disk_time;
    struct v86_frame f = disk_frame(0x0201);
    f.tf.cs = 0xF000; f.tf.eip = 0x132; f.tf.user_ss = 0x2000; f.tf.user_esp = 0x100;
    CHECK(!interrupt_service(v, &f, 0xFE) && !(f.tf.eflags & V86_CF));
    CHECK(v->perf.vector[0][0xFE].count == 1 && v->perf.vector[1][0x13].count == 1);
    CHECK(v->perf.vector[1][0x13].cycles == 3000 && v->perf.vector[1][0x13].ax == 0x0201);
    CHECK(v->perf.vector[0][0xFE].cycles == 3000);
    printf("f3-24 interrupt: bios vector=13 AX=0201 CPU=%llu us, off-CPU=9000 us excluded\n",
           (unsigned long long)perf_us(v->perf.vector[1][0x13].cycles));
    disk_read_hook = 0; dosvm_task_switch(&caller);
    fake_tsc = 0; g_tsc_per_ms = 0; g_boot.flags = 0;
    destroy(v, true); empty();
    puts("f3-24 disk: overlay precedence, two-window retention, failed/short read-ahead, retry and cut-base EOF PASS");
}
static void test_startup(void) { test_startup_screen(); test_startup_disk(); }

/* F3-27: run the actual worker, including its diagnostic before first
 * entry. CPU memory access walks the active directory, not v->as. */
#ifdef CIUKI_DOSVM_GUEST_CPU
#include "dosvm_boot.inc"
static struct dosvm *startup_boot_vm;
static unsigned startup_boot_switches;
static void startup_boot_disk(uint32_t lba, void *buffer)
{
    memset(buffer, 0, 512);
    if (!lba) memcpy(buffer, startup_boot_sector, 512);
}
static bool startup_cpu_read(uint32_t linear, void *buffer, unsigned bytes)
{
    struct aspace active = {.pd_phys = read_cr3()};
    uint8_t *out = buffer;
    while (bytes) {
        uint32_t pte = 0, phys = as_lookup(&active, linear, &pte);
        uint32_t pde = table_va(active.pd_phys)[linear >> 22];
        if (!phys || !(pte & PTE_U) || !(pde & PTE_U)) return false;
        unsigned n = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (n > bytes) n = bytes;
        memcpy(out, P2V(phys), n);
        out += n; linear += n; bytes -= n;
    }
    return true;
}
static bool startup_cpu_write(uint32_t linear, const void *buffer, unsigned bytes)
{
    (void)linear; (void)buffer; (void)bytes;
    CHECK(false); return false; /* Both entry jumps precede any guest store. */
}
static void startup_boot_schedule(void)
{
    struct dosvm *v = startup_boot_vm;
    CHECK(g_current == v->task && !v->continuation);
    struct task *resume = g_current;
    g_current = &caller;
    write_cr3(dosvm_task_cr3(g_current, kpd_phys));
    dosvm_task_switch(g_current);
    CHECK(read_cr3() == kpd_phys);
    g_current = resume;
    write_cr3(dosvm_task_cr3(resume, kpd_phys));
    dosvm_task_switch(resume);
    CHECK(dosvm_task_esp0(resume, 0xABC000) == 0xABC000);
    startup_boot_switches++;
    schedule_hook = 0;
}
static void startup_boot_record(void)
{
    if (strstr(last_record, "src=screen n=1 ") && !startup_boot_vm->continuation)
        g_need_resched = true;
}
static void startup_boot_enter(const struct v86_frame *initial)
{
    struct dosvm *v = startup_boot_vm;
    CHECK(initial->tf.cs == 0 && initial->tf.eip == 0x7C00);
    CHECK(initial->tf.edx == 0x80 && (initial->tf.eflags & V86_VM));
    const struct gcpu_ops ops = {startup_cpu_read, startup_cpu_write,
                                if_cpu_descriptor, if_cpu_unexpected, 0};
    struct gcpu_state s = {.eip = initial->tf.eip, .edx = initial->tf.edx,
        .esp = initial->tf.user_esp, .eflags = initial->tf.eflags};
    gcpu_init(&ops);
    struct gcpu_event ev;
    int result = gcpu_run(&s, 8, &ev);
    gcpu_done();
    printf("f3-27 boot flags=%08x: reschedules=%u active_guest=%u executed=%u vector=%u eip=%08x address=%08x error=%x\n",
           g_boot.flags, startup_boot_switches, read_cr3() == v->as.pd_phys,
           ev.executed, ev.vector, s.eip, ev.cr2, ev.err);
    fflush(stdout);
    /* EB 3C and the far jump must retire; CLI is the first V86 #GP. */
    CHECK(result == GCPU_TRAP && ev.vector == 13 && ev.fault && ev.executed == 2 &&
          s.cs == 0 && s.eip == 0x7C43);
    CHECK(dosvm_task_cr3(g_current, kpd_phys) == v->as.pd_phys);
    CHECK(dosvm_task_esp0(g_current, 0xABC000) == v->continuation);
    CHECK(!dosvm_stop(v->info.key, DOSVM_EXIT));
}
#endif
static void test_startup_boot(void)
{
#ifdef CIUKI_DOSVM_GUEST_CPU
    const uint32_t flags[] = {CBI_F_TEST_REQUEST | CBI_F_DIAG_VERBOSE, CBI_F_VERBOSE, 0};
    for (unsigned i = 0; i < ARRAY_SIZE(flags); i++) {
        disk_base_sector = startup_boot_disk;
        struct dosvm *v = create();
        disk_base_sector = 0;
        startup_boot_vm = v; startup_boot_switches = 0;
        CHECK(!dosvm_boot(v));
        uint8_t boot[512];
        CHECK(!dosvm_read(v, 0x7C00, boot, sizeof(boot)) && !memcmp(boot, startup_boot_sector, sizeof(boot)));
        CHECK(as_lookup(&v->as, 0x7000, 0) == v->frames[7]);
        CHECK(dosvm_task_cr3(&caller, 0xABC000) == 0xABC000);
        g_boot.flags = flags[i];
        diag_time_hook = startup_boot_record; schedule_hook = startup_boot_schedule;
        guest_enter_hook = startup_boot_enter;
        unsigned before = guest_enters;
        run_worker(v);
        CHECK(guest_enters == before + 1 && startup_boot_switches == (flags[i] ? 1u : 0u));
        CHECK(!v->continuation && dosvm_task_esp0(v->task, 0xABC000) == 0xABC000);
        diag_time_hook = schedule_hook = 0; guest_enter_hook = 0;
        g_boot.flags = 0; g_need_resched = false;
        destroy(v, true); empty(); startup_boot_vm = 0;
    }
    puts("f3-27 boot: production sector executes after startup reschedule; diagnostic, verbose and quiet boots, ESP0 and cleanup PASS");
#else
    puts("f3-27 boot: SKIP (no libx86emu headers)");
#endif
}
