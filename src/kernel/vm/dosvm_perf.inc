/* f3-13/15 counters: one memory snapshot per second; records are dumped after
 * the worker stops, so a polled UART cannot consume the game's frame budget.
 * TSC accounting pauses across task switches; it never includes a sleeping
 * monitor's wall time as VM CPU time. SPDX-License-Identifier: GPL-2.0-only */
uint64_t dosvm_perf_clock(void)
{
    return (g_boot.flags & CBI_F_DIAG_VERBOSE) && g_tsc_per_ms ? read_tsc_serialized() : 0;
}
static void perf_phase(struct dosvm *v, unsigned phase)
{
    uint64_t now = dosvm_perf_clock();
    if (v->perf.phase == 1) v->perf.guest_cycles += now - v->perf.stamp;
    if (v->perf.phase == 2) {
        uint64_t cycles = now - v->perf.stamp;
        v->perf.monitor_cycles += cycles;
        if (v->perf.trap_active) v->perf.trap_cycles += cycles;
    }
    v->perf.stamp = now; v->perf.phase = phase;
}
static struct dosvm *perf_current;
static void perf_switch(struct dosvm *next)
{
    if (perf_current) {
        perf_current->perf.resume_phase = perf_current->perf.phase;
        perf_phase(perf_current, 0);
    }
    perf_current = next;
    if (next) perf_phase(next, next->perf.resume_phase ? next->perf.resume_phase : 1);
}
static uint64_t perf_us(uint64_t cycles)
{ return g_tsc_per_ms ? cycles * 1000u / g_tsc_per_ms : 0; }
static uint64_t perf_monitor_stamp(struct dosvm *v)
{
    if (!(g_boot.flags & CBI_F_VERBOSE) || !g_tsc_per_ms) return 0;
    if (perf_current == v) perf_phase(v, v->perf.phase);
    return v->perf.monitor_cycles;
}
static void perf_vector_end(struct dosvm *v, unsigned path, uint8_t vector, uint16_t ax, uint64_t begin)
{
    if (!(g_boot.flags & CBI_F_VERBOSE)) return;
    uint64_t cycles = perf_monitor_stamp(v) - begin;
    struct dosvm_vector_cost *c = &v->perf.vector[path][vector];
    c->count++; c->cycles += cycles;
    if (cycles >= c->max_cycles) { c->max_cycles = cycles; c->ax = ax; }
}

/* Live startup evidence is emitted at safe boundaries, never per interrupt.
 * Snapshot before printing; the native owner can run while the VM sleeps.
 * 40 raw characters per record leave space for full-width IDs/counters. */
void dosvm_startup_report(struct dosvm *v, bool force)
{
    if (!(g_boot.flags & CBI_F_VERBOSE) || v->startup.busy ||
        (dosvm_in_irq(v) && v->info.state < DOSVM_STOPPING) ||
        (!force && (!v->startup.started || v->video_frames || g_ticks - v->startup.tick < 5000))) return;
    v->startup.busy = v->startup.started = true;
    v->startup.tick = g_ticks;
    uint32_t number = ++v->startup.number;
    uint8_t bda[64] = {0};
    int error = dosvm_read(v, 0x449, bda, sizeof(bda));
    unsigned mode = bda[0], cols = bda[1] | (bda[2] << 8), rows = bda[0x3B] + 1u;
    unsigned page = bda[5] | (bda[6] << 8);
    bool text = !error && (mode <= 3 || mode == 7) && cols && cols <= 80 && rows <= 25;
    if (text) error = dosvm_read(v, (mode == 7 ? 0xB0000 : 0xB8000) + page,
                                 v->startup.text, cols * rows * 2u);
    uint64_t begin = dosvm_perf_clock();
    rec_diag(dosvm_probe(v), "group=guest vm=%u gen=%u src=screen n=%u mode=%u cols=%u rows=%u error=%d",
             v->info.key.id, v->info.key.generation, number, mode, cols, rows, error);
    if (text && !error) for (unsigned row = 0; row < rows; row++) {
        unsigned length = cols;
        while (length && ((v->startup.text[row * cols + length - 1] & 255) == ' ' ||
                         !(v->startup.text[row * cols + length - 1] & 255))) length--;
        unsigned parts = (length + 39) / 40;
        for (unsigned part = 0; part < parts; part++) {
            char data[81];
            unsigned bytes = length - part * 40;
            if (bytes > 40) bytes = 40;
            for (unsigned i = 0; i < bytes; i++) {
                uint8_t ch = v->startup.text[row * cols + part * 40 + i];
                data[i * 2] = "0123456789abcdef"[ch >> 4];
                data[i * 2 + 1] = "0123456789abcdef"[ch & 15];
            }
            data[bytes * 2] = 0;
            rec_diag(dosvm_probe(v), "group=guest vm=%u gen=%u src=screen n=%u row=%u part=%u parts=%u data=%s",
                     v->info.key.id, v->info.key.generation, number, row, part + 1, parts, data);
        }
        if (g_need_resched) schedule();
    }
    for (unsigned path = 0; path < 2; path++) for (unsigned vector = 0; vector < 256; vector++) {
        /* Copy each counter while the native owner may preempt the worker. */
        uint32_t flags = irq_save();
        struct dosvm_vector_cost c = v->perf.vector[path][vector];
        irq_restore(flags);
        if (!c.count) continue;
        rec_diag(dosvm_probe(v), "group=vm_perf_v vm=%u gen=%u n=%u path=%s v=%02x calls=%u us=%llu max_us=%llu ax=%04x",
                 v->info.key.id, v->info.key.generation, number, path ? "bios" : "int", vector,
                 c.count, perf_us(c.cycles), perf_us(c.max_cycles), c.ax);
        if (g_need_resched) schedule();
    }
    rec_diag(dosvm_probe(v), "group=vm_perf_disk vm=%u gen=%u n=%u reads=%u bytes=%llu max=%u hits=%u",
             v->info.key.id, v->info.key.generation, number, v->perf.disk_reads, v->perf.disk_bytes, v->perf.disk_max, v->perf.disk_hits);
    v->perf.log_cycles += dosvm_perf_clock() - begin;
    v->startup.tick = g_ticks; /* slow output never creates a catch-up burst */
    v->startup.busy = false;
}
static void perf_exit_end(struct dosvm *v)
{
    perf_phase(v, 2);
    if (g_boot.flags & CBI_F_DIAG_VERBOSE) {
        v->perf.cost_count[v->perf.cause]++;
        v->perf.cost_cycles[v->perf.cause] += v->perf.trap_cycles;
        if (v->perf.cause == DOSVM_COST_VGA_IO) {
            unsigned r = v->perf.vga_reg;
            v->perf.port_exits[r]++;
            v->perf.port_exit_cycles[r] += v->perf.trap_cycles;
        }
    }
    v->perf.trap_active = false;
}

#define DOSVM_PERF_FIRST 4u
static unsigned perf_sample_slot(unsigned n)
{
    return n < DOSVM_PERF_FIRST ? n : DOSVM_PERF_FIRST +
        (n - DOSVM_PERF_FIRST) % (16u - DOSVM_PERF_FIRST);
}

void dosvm_perf_report(struct dosvm *v)
{
    if (!(g_boot.flags & CBI_F_DIAG_VERBOSE) || !g_tsc_per_ms ||
        g_ticks - v->perf.report_tick < 1000) return;
    unsigned old_phase = v->perf.phase;
    /* STATUS runs on the native owner: never restart a suspended VM clock. */
    if (perf_current == v) perf_phase(v, old_phase);
    uint32_t flags = irq_save();
    uint64_t tick = g_ticks, edges = v->pit.edges;
    uint32_t irqs[16]; memcpy(irqs, v->monitor.stats.delivered, sizeof(irqs));
    irq_restore(flags);
#define DELTA(field, last) (v->perf.field - v->perf.last)
    struct dosvm_perf_sample *s = &v->perf.sample[perf_sample_slot(v->perf.sample_count++)];
    *s = (struct dosvm_perf_sample){
        .ms = (uint32_t)(tick - v->perf.report_tick),
        .frames = v->video_frames - v->perf.last_frames, .flips = v->video_starts - v->perf.last_starts,
        .harvests = DELTA(harvests, last_harvests), .pages = DELTA(direct_pages, last_pages),
        .converted = DELTA(converted_bytes, last_converted), .rows = DELTA(rows, last_rows),
        .trapped = DELTA(trapped_bytes, last_trapped), .guest_us = perf_us(DELTA(guest_cycles, last_guest)),
        .monitor_us = perf_us(DELTA(monitor_cycles, last_monitor)), .log_us = perf_us(DELTA(log_cycles, last_log)),
        .present_us = perf_us(DELTA(present_cycles, last_present)),
        .harvest_us = perf_us(DELTA(harvest_cycles, last_harvest)), .edges = edges - v->perf.last_edges
    };
    s->port_remaps = DELTA(port_remaps, last_port_remaps);
    s->remap_us = perf_us(DELTA(remap_cycles, last_remap));
    s->burst_insn = DELTA(burst_insn, last_burst_insn);
    s->latch_read_windows = DELTA(latch_read_windows, last_latch_read_windows);
    for (unsigned i = 0; i < DOSVM_VGA_REGS; i++) {
        s->port_count[i] = v->perf.port_count[i] - v->perf.last_port_count[i];
        s->port_exits[i] = v->perf.port_exits[i] - v->perf.last_port_exits[i];
        s->port_us[i] = perf_us(v->perf.port_cycles[i] - v->perf.last_port_cycles[i]);
        s->port_exit_us[i] = perf_us(v->perf.port_exit_cycles[i] - v->perf.last_port_exit_cycles[i]);
        v->perf.last_port_count[i] = v->perf.port_count[i];
        v->perf.last_port_exits[i] = v->perf.port_exits[i];
        v->perf.last_port_cycles[i] = v->perf.port_cycles[i];
        v->perf.last_port_exit_cycles[i] = v->perf.port_exit_cycles[i];
    }
    v->perf.last_port_remaps = v->perf.port_remaps; v->perf.last_remap = v->perf.remap_cycles;
    v->perf.last_burst_insn = v->perf.burst_insn; v->perf.last_latch_read_windows = v->perf.latch_read_windows;
    s->batched = DELTA(batched, last_batched);
    s->idle_blocks = v->idle.blocks - v->perf.last_idle_blocks;
    v->perf.last_idle_blocks = v->idle.blocks;
    s->idle_reasons = 0;
    for (unsigned i = 0; i < DOSVM_IDLE_REASONS; i++) {
        s->idle_count[i] = v->idle.count[i] - v->perf.last_idle_count[i];
        v->perf.last_idle_count[i] = v->idle.count[i];
        if (s->idle_count[i]) s->idle_reasons |= 1u << i;
    }
    uint64_t wall_us = (uint64_t)s->ms * 1000u;
    uint64_t busy = wall_us ? (s->guest_us + s->monitor_us) * 1000u / wall_us : 0;
    s->busy_permille = v->idle.busy_permille = busy < 1000 ? (uint32_t)busy : 1000;
    v->perf.last_batched = v->perf.batched;
    for (unsigned i = 0; i < DOSVM_COST_COUNT; i++) {
        s->cost_count[i] = v->perf.cost_count[i] - v->perf.last_cost_count[i];
        s->cost_us[i] = perf_us(v->perf.cost_cycles[i] - v->perf.last_cost_cycles[i]);
        v->perf.last_cost_count[i] = v->perf.cost_count[i];
        v->perf.last_cost_cycles[i] = v->perf.cost_cycles[i];
    }
    for (unsigned i = 0; i < 6; i++) s->exits[i] = v->perf.exits[i] - v->perf.last_exits[i];
    for (unsigned i = 0; i < 16; i++) s->irqs[i] = irqs[i] - v->perf.last_irqs[i];
    for (unsigned i = 0; i < V86_INSNS; i++) {
        uint64_t count = v->monitor.stats.insn[i];
        s->insn[i] = count - v->perf.last_insn[i];
        v->perf.last_insn[i] = count;
    }
    v->perf.report_tick = tick;
    v->perf.last_frames = v->video_frames; v->perf.last_starts = v->video_starts;
    v->perf.last_harvests = v->perf.harvests; v->perf.last_pages = v->perf.direct_pages;
    v->perf.last_converted = v->perf.converted_bytes; v->perf.last_rows = v->perf.rows;
    v->perf.last_trapped = v->perf.trapped_bytes;
    memcpy(v->perf.last_exits, v->perf.exits, sizeof(v->perf.exits));
    memcpy(v->perf.last_irqs, irqs, sizeof(irqs)); v->perf.last_edges = edges;
    v->perf.last_guest = v->perf.guest_cycles; v->perf.last_monitor = v->perf.monitor_cycles;
    v->perf.last_present = v->perf.present_cycles; v->perf.last_log = v->perf.log_cycles;
    v->perf.last_harvest = v->perf.harvest_cycles;
#undef DELTA
}

void dosvm_perf_dump(struct dosvm *v)
{
    if (!(g_boot.flags & CBI_F_DIAG_VERBOSE)) return;
    static const char *const names[DOSVM_COST_COUNT] = {
        "rm_int", "rm_iret", "rm_flags", "pm_int", "pm_flags", "switch",
        "vga_io", "vga_pf", "pit_pic", "kbd", "io", "db", "timer", "other",
        "rm_pushf", "rm_popf", "rm_cli", "rm_sti", "rm_hlt", "pm_hlt"
    };
    if (g_boot.flags & CBI_F_DIAG_VERBOSE) {
        static const char hex[] = "0123456789abcdef";
        for (unsigned offset = 0; offset < v->boot_log_count; offset += 48) {
            char data[97];
            unsigned bytes = v->boot_log_count - offset;
            if (bytes > 48) bytes = 48;
            for (unsigned i = 0; i < bytes; i++) {
                uint8_t byte = (uint8_t)v->boot_log[offset + i];
                data[i * 2] = hex[byte >> 4]; data[i * 2 + 1] = hex[byte & 15];
            }
            data[bytes * 2] = 0;
            rec_diag(dosvm_probe(v), "group=vm_boot_log vm=%u gen=%u offset=%u data=%s",
                     v->info.key.id, v->info.key.generation, offset, data);
            if (g_need_resched) schedule();
        }
    }
    v->boot_log_count = 0;
    uint32_t count = v->perf.sample_count;
    unsigned retained = ARRAY_SIZE(v->perf.sample);
    if (!count) return;
    rec_diag(dosvm_probe(v), "group=vm_perf_samples vm=%u gen=%u total=%u retained=%u", v->info.key.id,
             v->info.key.generation, count, count < retained ? count : retained);
    for (uint32_t row = 0, rows = count < retained ? count : retained; row < rows; row++) {
        uint32_t n = count > retained && row >= DOSVM_PERF_FIRST ?
            count - (retained - DOSVM_PERF_FIRST) + row - DOSVM_PERF_FIRST : row;
        const struct dosvm_perf_sample *s = &v->perf.sample[perf_sample_slot(n)];
        unsigned id = v->info.key.id;
        rec_diag(dosvm_probe(v), "group=vm_perf_video vm=%u n=%u ms=%u frames=%u flips=%u harvests=%u pages=%u",
            id, n+1, s->ms, s->frames, s->flips, s->harvests, s->pages);
        /* Companion row keeps maximum-width counters within 240 bytes. */
        rec_diag(dosvm_probe(v), "group=vm_perf_harvest vm=%u n=%u dirty_pages=%u harvest_us=%llu",
            id, n+1, s->pages, s->harvest_us);
        rec_diag(dosvm_probe(v), "group=vm_perf_present vm=%u n=%u converted=%u rows=%u present_us=%llu",
            id, n+1, s->converted, s->rows, s->present_us);
        rec_diag(dosvm_probe(v), "group=vm_perf_remap vm=%u n=%u port_remaps=%u remap_us=%llu burst_insn=%u latch_read_windows=%u",
            id, n+1, s->port_remaps, s->remap_us, s->burst_insn, s->latch_read_windows);
        static const char *const regs[DOSVM_VGA_REGS] = {
            "sr_index", "sr02", "sr_other", "gc_index", "gc04", "gc05", "gc_other", "crtc", "dac", "other"
        };
        for (unsigned i = 0; i < DOSVM_VGA_REGS; i++) if (s->port_count[i] || s->port_exits[i])
            rec_diag(dosvm_probe(v), "group=vm_perf_vga_port vm=%u n=%u reg=%s writes=%u handler_us=%llu exits=%u exit_us=%llu",
                id, n+1, regs[i], s->port_count[i], s->port_us[i], s->port_exits[i], s->port_exit_us[i]);
        rec_diag(dosvm_probe(v), "group=vm_perf_cpu vm=%u n=%u guest_us=%llu monitor_us=%llu log_us=%llu trapped=%u",
            id, n+1, s->guest_us, s->monitor_us, s->log_us, s->trapped);
        uint64_t wall_us = (uint64_t)s->ms * 1000u;
        uint64_t cpu_us = s->guest_us + s->monitor_us;
        rec_diag(dosvm_probe(v), "group=vm_perf_wall vm=%u n=%u monitor_permille=%llu off_cpu_us=%llu",
            id, n+1, wall_us ? s->monitor_us * 1000u / wall_us : 0, wall_us > cpu_us ? wall_us - cpu_us : 0);
        rec_diag(dosvm_probe(v), "group=vm_perf_share vm=%u n=%u wall_us=%llu vm_permille=%llu guest_permille=%llu idle_blocks=%u idle_reasons=%x busy_permille=%u",
            id, n+1, wall_us, wall_us ? (s->guest_us + s->monitor_us) * 1000u / wall_us : 0,
            wall_us ? s->guest_us * 1000u / wall_us : 0, s->idle_blocks, s->idle_reasons, s->busy_permille);
        rec_diag(dosvm_probe(v), "group=vm_idle_counts vm=%u n=%u int2f1680=%u int28=%u int16=%u hlt=%u",
            id, n+1, s->idle_count[DOSVM_IDLE_2F], s->idle_count[DOSVM_IDLE_28],
            s->idle_count[DOSVM_IDLE_16], s->idle_count[DOSVM_IDLE_HLT]);
        rec_diag(dosvm_probe(v), "group=vm_perf_rate vm=%u n=%u frames_mfps=%llu flips_mfps=%llu harvests_ps=%llu batched=%u",
            id, n+1, s->ms ? (uint64_t)s->frames * 1000000u / s->ms : 0,
            s->ms ? (uint64_t)s->flips * 1000000u / s->ms : 0,
            s->ms ? (uint64_t)s->harvests * 1000u / s->ms : 0, s->batched);
        for (unsigned i = 0; i < DOSVM_COST_COUNT; i++) if (s->cost_count[i])
            rec_diag(dosvm_probe(v), "group=vm_perf_cost vm=%u n=%u cause=%s exits=%u us=%llu avg_ns=%llu",
                id, n+1, names[i], s->cost_count[i], s->cost_us[i],
                s->cost_us[i] * 1000u / s->cost_count[i]);
        rec_diag(dosvm_probe(v), "group=vm_perf_exits vm=%u n=%u gp=%u pf=%u db=%u timer=%u other=%u pm=%u",
            id, n+1, s->exits[0], s->exits[1], s->exits[2], s->exits[3], s->exits[4], s->exits[5]);
        rec_diag(dosvm_probe(v), "group=vm_perf_pit vm=%u n=%u edges=%llu", id, n+1, s->edges);
        for (unsigned i = 0; i < 16; i += 8)
            rec_diag(dosvm_probe(v), "group=vm_perf_irq vm=%u n=%u first=%u counts=%u,%u,%u,%u,%u,%u,%u,%u", id, n+1, i,
                s->irqs[i], s->irqs[i+1], s->irqs[i+2], s->irqs[i+3], s->irqs[i+4], s->irqs[i+5], s->irqs[i+6], s->irqs[i+7]);
        for (unsigned i = 0; i < V86_INSNS; i++) if (s->insn[i])
            rec_diag(dosvm_probe(v), "group=vm_perf_v86 vm=%u n=%u cause=%u exits=%llu", id, n+1, i, s->insn[i]);
        if (g_need_resched) schedule(); /* slow polled UART must permit native work */
    }
    v->perf.sample_count = 0;
}
