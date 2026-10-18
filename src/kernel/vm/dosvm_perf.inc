/* f0-34: live disk summaries are protected; rec_diag stop-time history is
 * droppable regardless of a shared group=vm_perf_* name.
 * f3-31 counters: one memory snapshot and disk-only RAM summary per five
 * seconds; the verbose UART history is dumped after the worker stops.
 * TSC accounting pauses across task switches; it never includes a sleeping
 * monitor's wall time as VM CPU time. SPDX-License-Identifier: GPL-2.0-only */
uint64_t dosvm_perf_clock(void)
{
    /* Ordinary hardware boots need the same accounting as diagnostics. */
    return g_tsc_per_ms ? read_tsc_serialized() : 0;
}
uint64_t dosvm_perf_task_clock(void)
{
    /* Presenter/port calls can be preempted. The native scheduler clock
     * excludes suspension and physical IRQ time; host seams use fake TSC. */
    return g_tsc_per_ms ? (cpu_path_clock ? cpu_path_clock() : dosvm_perf_clock()) : 0;
}
/* The physical frame's IF stays set at CPL 3. Count only changes to the
 * software virtual flag, at the instruction/host return that restores it. */
void dosvm_vif_restore(struct dosvm *v, bool enabled, enum dosvm_if_path path)
{
    if (enabled && !v->monitor.vif) v->perf.if_enable[path]++;
    v->monitor.vif = enabled;
}
void dosvm_isr_begin(struct dosvm *v, struct dpmi_record *r, unsigned irq)
{
    r->isr_measured = true; r->isr_irq = irq;
    r->isr_tsc = dosvm_perf_clock(); r->isr_tick = g_ticks;
    unsigned depth = 0;
    for (unsigned i = 0; i < v->dpmi.depth; i++)
        if ((v->dpmi.rec[i].flags & DPMI_RF_HWINT) && !v->dpmi.rec[i].isr_expired) depth++;
    if (depth > v->perf.isr_max_depth) v->perf.isr_max_depth = depth;
}
void dosvm_isr_end(struct dosvm *v, const struct dpmi_record *r)
{
    if (!r->isr_measured) return;
    struct dosvm_isr_cost *s = &v->perf.isr[r->isr_irq];
    uint64_t ms = g_ticks - r->isr_tick;
    uint64_t us = g_tsc_per_ms ? (dosvm_perf_clock() - r->isr_tsc) * 1000 / g_tsc_per_ms : ms * 1000;
    static const uint32_t bounds[7] = {10, 100, 500, 1000, 2000, 5000, 10000};
    unsigned bin = 0;
    while (bin < ARRAY_SIZE(bounds) && us > bounds[bin]) bin++;
    s->vector = r->vector; s->count++; s->us += us; s->ms += ms; s->bins[bin]++;
    if (us > s->max_us) s->max_us = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    if (ms > s->max_ms) s->max_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}
static void perf_phase(struct dosvm *v, unsigned phase)
{
    uint32_t flags = irq_save();
    uint64_t now = dosvm_perf_clock();
    if (v->perf.phase == 1) v->perf.guest_cycles += now - v->perf.stamp;
    if (v->perf.phase == 2) {
        uint64_t cycles = now - v->perf.stamp;
        v->perf.monitor_cycles += cycles;
        if (v->perf.trap_active) v->perf.trap_cycles += cycles;
    }
    v->perf.stamp = now; v->perf.phase = phase;
    irq_restore(flags);
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
    if (!g_tsc_per_ms) return 0;
    if (perf_current == v) perf_phase(v, v->perf.phase);
    return v->perf.monitor_cycles;
}
static void perf_vector_end(struct dosvm *v, unsigned path, uint8_t vector, uint16_t ax, uint64_t begin)
{
    uint32_t flags = irq_save();
    uint64_t cycles = perf_monitor_stamp(v) - begin;
    struct dosvm_vector_cost *c = &v->perf.vector[path][vector];
    c->count++; c->cycles += cycles;
    struct dosvm_hot_cost *h = &v->perf.interval[v->perf.active_interval].vectors[path][vector];
    h->id = (uint16_t)(path * 256 + vector); h->count++; h->cycles += cycles;
    if (cycles >= c->max_cycles) { c->max_cycles = cycles; c->ax = ax; }
    irq_restore(flags);
}

/* Live startup evidence is emitted at safe boundaries, never per interrupt.
 * Snapshot before printing; the native owner can run while the VM sleeps.
 * 40 raw characters per record leave space for full-width IDs/counters. */
void dosvm_startup_report(struct dosvm *v, bool force)
{
    if (!(g_boot.flags & (CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE)) || v->startup.busy ||
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
        rec_diag(dosvm_probe(v), "group=vm_perf_v vm=%u gen=%u n=%u path=%s vector=%02x calls=%u us=%llu max_us=%llu ax=%04x",
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
    uint32_t flags = irq_save();
    perf_phase(v, 2);
    {
        v->perf.cost_count[v->perf.cause]++;
        v->perf.cost_cycles[v->perf.cause] += v->perf.trap_cycles;
        if (v->perf.cause == DOSVM_COST_VGA_IO) {
            unsigned r = v->perf.vga_reg;
            v->perf.port_exits[r]++;
            v->perf.port_exit_cycles[r] += v->perf.trap_cycles;
        }
    }
    if (v->perf.io_valid) {
        unsigned slot = v->perf.io_port % DOSVM_PERF_PORTS, n;
        for (n = 0; n < DOSVM_PERF_PORTS; n++, slot = (slot + 1) % DOSVM_PERF_PORTS) {
            struct dosvm_hot_cost *h = &v->perf.interval[v->perf.active_interval].ports[slot];
            if (!h->count || h->id == v->perf.io_port) {
                h->id = v->perf.io_port; h->count++; h->cycles += v->perf.trap_cycles;
                break;
            }
        }
        if (n == DOSVM_PERF_PORTS) v->perf.port_overflow++;
    }
    v->perf.trap_active = false;
    irq_restore(flags);
}

#define DOSVM_PERF_FIRST 4u
static unsigned perf_sample_slot(unsigned n)
{
    return n < DOSVM_PERF_FIRST ? n : DOSVM_PERF_FIRST +
        (n - DOSVM_PERF_FIRST) % (16u - DOSVM_PERF_FIRST);
}

/* Fixed-size selection: ties use the numeric key, never allocation/order.
 * Stored cycles are converted only when emitted, after the atomic snapshot. */
static void perf_top(struct dosvm_hot_cost top[DOSVM_PERF_TOP], struct dosvm_hot_cost h)
{
    if (!h.count) return;
    for (unsigned i = 0; i < DOSVM_PERF_TOP; i++) {
        if (!top[i].count || h.cycles > top[i].cycles ||
            (h.cycles == top[i].cycles && h.id < top[i].id)) {
            for (unsigned j = DOSVM_PERF_TOP - 1; j > i; j--) top[j] = top[j-1];
            top[i] = h; break;
        }
    }
}
static const char *const perf_names[DOSVM_COST_COUNT] = {
    "rm_int", "rm_iret", "rm_flags", "pm_int", "pm_flags", "switch",
    "vga_io", "vga_pf", "pit_pic", "kbd", "io", "db", "timer", "other",
    "rm_pushf", "rm_popf", "rm_cli", "rm_sti", "rm_hlt", "pm_hlt", "dpmi", "host_irq"
};
/* f3-44 (lead): cumulative virtual-IF and ISR counters on the disk record,
 * so a hardware trial shows which path re-enables IF and how long ISRs run.
 * Copied under IF-off because producers update them in trap context. */
static void perf_if_live(struct dosvm *v, unsigned n)
{
    uint32_t enable[DOSVM_IF_PATHS], depth, retired, expired, c0, m0, c7, m7, c5, m5;
    uint64_t samples, clear, outside;
    uint32_t flags = irq_save();
    memcpy(enable, v->perf.if_enable, sizeof(enable));
    samples = v->perf.if_samples_ms; clear = v->perf.if_clear_ms; outside = v->perf.if_outside_ms;
    depth = v->perf.isr_max_depth; retired = v->perf.isr_retired; expired = v->perf.isr_expired;
    c0 = v->perf.isr[0].count; m0 = v->perf.isr[0].max_us;
    c7 = v->perf.isr[7].count; m7 = v->perf.isr[7].max_us;
    c5 = v->perf.isr[5].count; m5 = v->perf.isr[5].max_us;
    irq_restore(flags);
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_if vm=%u gen=%u n=%u enable=%u,%u,%u,%u,%u,%u samples_ms=%llu clear_ms=%llu outside_ms=%llu isr_depth=%u isr_retired=%u isr_expired=%u isr0=%u/%u isr5=%u/%u isr7=%u/%u",
                  v->info.key.id, v->info.key.generation, n, enable[0], enable[1], enable[2], enable[3], enable[4], enable[5],
                  samples, clear, outside, depth, retired, expired, c0, m0, c5, m5, c7, m7);
}
static void perf_live(struct dosvm *v, const struct dosvm_perf_sample *s, unsigned n)
{
    unsigned id = v->info.key.id, gen = v->info.key.generation;
    uint64_t wall = (uint64_t)s->ms * 1000, cpu = s->guest_us + s->monitor_us;
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_cpu vm=%u gen=%u n=%u ms=%u guest_us=%llu monitor_us=%llu",
                  id, gen, n, s->ms, s->guest_us, s->monitor_us);
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_share vm=%u gen=%u n=%u guest_permille=%llu monitor_permille=%llu off_cpu_us=%llu",
                  id, gen, n, wall ? s->guest_us * 1000 / wall : 0,
                  wall ? s->monitor_us * 1000 / wall : 0, wall > cpu ? wall - cpu : 0);
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_video vm=%u gen=%u n=%u frames=%u flips=%u harvests=%u pages=%u present_rate=%u",
                  id, gen, n, s->frames, s->flips, s->harvests, s->pages, s->present_rate);
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_present vm=%u gen=%u n=%u present_us=%llu harvest_us=%llu rows=%u",
                  id, gen, n, s->present_us, s->harvest_us, s->rows);
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_remap vm=%u gen=%u n=%u remaps=%u remap_us=%llu batched=%u",
                  id, gen, n, s->port_remaps, s->remap_us, s->batched);
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_exits vm=%u gen=%u n=%u gp=%u pf=%u db=%u timer=%u other=%u pm=%u",
                  id, gen, n, s->exits[0], s->exits[1], s->exits[2], s->exits[3], s->exits[4], s->exits[5]);
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_service vm=%u gen=%u n=%u ticks=%u timer_us=%llu audio_us=%llu port_overflow=%u",
                  id, gen, n, s->ticks, s->timer_us, s->audio_us, s->port_overflow);
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_checkpoint vm=%u gen=%u n=%u hits=%u segmented=%u fallback=%u",
                         id, gen, n, s->return_hits, s->return_segmented, s->return_fallbacks);
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_tracker vm=%u gen=%u n=%u armed=%u hits=%u expired=%u fallback=%u reacquired=%u segmented=%u",
                         id, gen, n, s->return_armed, s->return_hits, s->return_expired,
                         s->return_fallbacks, s->return_reacquired, s->return_segmented);
    perf_if_live(v, n);
    for (unsigned i = 0; i < DOSVM_COST_COUNT; i++) if (s->cost_count[i])
        rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_cost vm=%u gen=%u n=%u cause=%s exits=%u us=%llu",
                      id, gen, n, perf_names[i], s->cost_count[i], s->cost_us[i]);
    for (unsigned i = 0; i < DOSVM_PERF_TOP; i++) {
        const struct dosvm_hot_cost *p = &s->top_port[i], *q = &s->top_vector[i];
        if (p->count) rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_port vm=%u gen=%u n=%u rank=%u port=%04x exits=%u us=%llu",
                      id, gen, n, i + 1, p->id, p->count, perf_us(p->cycles));
        if (q->count) rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_perf_vector vm=%u gen=%u n=%u rank=%u path=%s vector=%02x calls=%u us=%llu",
                      id, gen, n, i + 1, q->id < 256 ? "int" : q->id < 512 ? "bios" : "pm",
                      q->id & 255, q->count, perf_us(q->cycles));
    }
}

void dosvm_perf_report(struct dosvm *v)
{
    if (!g_tsc_per_ms || g_ticks - v->perf.report_tick < DOSVM_PERF_INTERVAL) return;
    uint32_t flags = irq_save();
    /* Report only from preemptible task context, including native STATUS. */
    if (!(flags & 0x200) || !g_tsc_per_ms || v->perf.reporting ||
        g_ticks - v->perf.report_tick < DOSVM_PERF_INTERVAL) {
        irq_restore(flags); return;
    }
    if (crit_label) crit_label("dosvm_perf_snapshot");
    v->perf.reporting = true;
    /* STATUS must not restart a suspended VM clock. Only close the current
     * phase here; ranking, clearing, deltas and all divisions follow IF restore.
     * The critical work is bounded by sizeof(raw), independent of table use. */
    if (perf_current == v) perf_phase(v, v->perf.phase);
    struct dosvm_perf_interval *bank = &v->perf.interval[v->perf.active_interval];
    struct dosvm_perf_raw *r = &bank->raw;
    uint64_t tick = g_ticks;
    r->ms = (uint32_t)(tick - v->perf.report_tick);
    r->frames = v->video_frames; r->starts = v->video_starts;
    r->present_rate = v->video_present_rate;
    r->edges = v->pit.edges; r->idle_blocks = v->idle.blocks;
    memcpy(r->idle_count, v->idle.count, sizeof(r->idle_count));
    memcpy(r->irqs, v->monitor.stats.delivered, sizeof(r->irqs));
    memcpy(r->insn, v->monitor.stats.insn, sizeof(r->insn));
#define CAPTURE(field) r->field = v->perf.field
    CAPTURE(harvests); CAPTURE(direct_pages); CAPTURE(trapped_bytes);
    CAPTURE(converted_bytes); CAPTURE(rows); CAPTURE(port_remaps);
    CAPTURE(burst_insn); CAPTURE(latch_read_windows); CAPTURE(batched);
    CAPTURE(port_overflow); CAPTURE(ticks);
    CAPTURE(return_hits); CAPTURE(return_segmented); CAPTURE(return_fallbacks);
    CAPTURE(return_armed); CAPTURE(return_expired); CAPTURE(return_reacquired);
    CAPTURE(guest_cycles); CAPTURE(monitor_cycles); CAPTURE(present_cycles);
    CAPTURE(harvest_cycles); CAPTURE(log_cycles); CAPTURE(remap_cycles);
    CAPTURE(timer_cycles); CAPTURE(audio_cycles);
#undef CAPTURE
#define CAPTURE_ARRAY(field) memcpy(r->field, v->perf.field, sizeof(r->field))
    CAPTURE_ARRAY(exits); CAPTURE_ARRAY(cost_count); CAPTURE_ARRAY(cost_cycles);
    CAPTURE_ARRAY(port_count); CAPTURE_ARRAY(port_exits);
    CAPTURE_ARRAY(port_cycles); CAPTURE_ARRAY(port_exit_cycles);
#undef CAPTURE_ARRAY
    v->perf.port_overflow = 0;
    v->perf.report_tick = tick;
    /* The spare bank was cleared by the previous reporter (or VM init).
     * Producers can run as soon as the bank index is published. reporting
     * excludes a second reporter until this detached bank is cleared. */
    v->perf.active_interval ^= 1u;
    irq_restore(flags);
#define DELTA(field, last) (r->field - v->perf.last)
    struct dosvm_perf_sample *s = &v->perf.sample[perf_sample_slot(v->perf.sample_count)];
    *s = (struct dosvm_perf_sample){
        .ms = r->ms,
        .present_rate = r->present_rate,
        .frames = r->frames - v->perf.last_frames, .flips = r->starts - v->perf.last_starts,
        .harvests = DELTA(harvests, last_harvests), .pages = DELTA(direct_pages, last_pages),
        .converted = DELTA(converted_bytes, last_converted), .rows = DELTA(rows, last_rows),
        .trapped = DELTA(trapped_bytes, last_trapped), .guest_us = perf_us(DELTA(guest_cycles, last_guest)),
        .monitor_us = perf_us(DELTA(monitor_cycles, last_monitor)), .log_us = perf_us(DELTA(log_cycles, last_log)),
        .present_us = perf_us(DELTA(present_cycles, last_present)),
        .harvest_us = perf_us(DELTA(harvest_cycles, last_harvest)), .edges = r->edges - v->perf.last_edges
    };
    s->return_hits = DELTA(return_hits, last_return_hits);
    s->return_segmented = DELTA(return_segmented, last_return_segmented);
    s->return_fallbacks = DELTA(return_fallbacks, last_return_fallbacks);
    v->perf.last_return_hits = r->return_hits;
    v->perf.last_return_segmented = r->return_segmented;
    v->perf.last_return_fallbacks = r->return_fallbacks;
    s->return_armed = DELTA(return_armed, last_return_armed);
    s->return_expired = DELTA(return_expired, last_return_expired);
    s->return_reacquired = DELTA(return_reacquired, last_return_reacquired);
    v->perf.last_return_armed = r->return_armed;
    v->perf.last_return_expired = r->return_expired;
    v->perf.last_return_reacquired = r->return_reacquired;
    s->ticks = DELTA(ticks, last_ticks); s->timer_us = perf_us(DELTA(timer_cycles, last_timer));
    s->audio_us = perf_us(DELTA(audio_cycles, last_audio));
    v->perf.last_ticks = r->ticks; v->perf.last_timer = r->timer_cycles;
    v->perf.last_audio = r->audio_cycles;
    for (unsigned i = 0; i < DOSVM_PERF_PORTS; i++) perf_top(s->top_port, bank->ports[i]);
    for (unsigned path = 0; path < 3; path++) for (unsigned i = 0; i < 256; i++)
        perf_top(s->top_vector, bank->vectors[path][i]);
    s->port_overflow = r->port_overflow;
    s->port_remaps = DELTA(port_remaps, last_port_remaps);
    s->remap_us = perf_us(DELTA(remap_cycles, last_remap));
    s->burst_insn = DELTA(burst_insn, last_burst_insn);
    s->latch_read_windows = DELTA(latch_read_windows, last_latch_read_windows);
    for (unsigned i = 0; i < DOSVM_VGA_REGS; i++) {
        s->port_count[i] = r->port_count[i] - v->perf.last_port_count[i];
        s->port_exits[i] = r->port_exits[i] - v->perf.last_port_exits[i];
        s->port_us[i] = perf_us(r->port_cycles[i] - v->perf.last_port_cycles[i]);
        s->port_exit_us[i] = perf_us(r->port_exit_cycles[i] - v->perf.last_port_exit_cycles[i]);
        v->perf.last_port_count[i] = r->port_count[i];
        v->perf.last_port_exits[i] = r->port_exits[i];
        v->perf.last_port_cycles[i] = r->port_cycles[i];
        v->perf.last_port_exit_cycles[i] = r->port_exit_cycles[i];
    }
    v->perf.last_port_remaps = r->port_remaps; v->perf.last_remap = r->remap_cycles;
    v->perf.last_burst_insn = r->burst_insn; v->perf.last_latch_read_windows = r->latch_read_windows;
    s->batched = DELTA(batched, last_batched);
    s->idle_blocks = r->idle_blocks - v->perf.last_idle_blocks;
    v->perf.last_idle_blocks = r->idle_blocks;
    s->idle_reasons = 0;
    for (unsigned i = 0; i < DOSVM_IDLE_REASONS; i++) {
        s->idle_count[i] = r->idle_count[i] - v->perf.last_idle_count[i];
        v->perf.last_idle_count[i] = r->idle_count[i];
        if (s->idle_count[i]) s->idle_reasons |= 1u << i;
    }
    uint64_t wall_us = (uint64_t)s->ms * 1000u;
    uint64_t busy = wall_us ? (s->guest_us + s->monitor_us) * 1000u / wall_us : 0;
    s->busy_permille = v->idle.busy_permille = busy < 1000 ? (uint32_t)busy : 1000;
    v->perf.last_batched = r->batched;
    for (unsigned i = 0; i < DOSVM_COST_COUNT; i++) {
        s->cost_count[i] = r->cost_count[i] - v->perf.last_cost_count[i];
        s->cost_us[i] = perf_us(r->cost_cycles[i] - v->perf.last_cost_cycles[i]);
        v->perf.last_cost_count[i] = r->cost_count[i];
        v->perf.last_cost_cycles[i] = r->cost_cycles[i];
    }
    for (unsigned i = 0; i < 6; i++) s->exits[i] = r->exits[i] - v->perf.last_exits[i];
    for (unsigned i = 0; i < 16; i++) s->irqs[i] = r->irqs[i] - v->perf.last_irqs[i];
    for (unsigned i = 0; i < V86_INSNS; i++) {
        uint64_t count = r->insn[i];
        s->insn[i] = count - v->perf.last_insn[i];
        v->perf.last_insn[i] = count;
    }
    v->perf.last_frames = r->frames; v->perf.last_starts = r->starts;
    v->perf.last_harvests = r->harvests; v->perf.last_pages = r->direct_pages;
    v->perf.last_converted = r->converted_bytes; v->perf.last_rows = r->rows;
    v->perf.last_trapped = r->trapped_bytes;
    memcpy(v->perf.last_exits, r->exits, sizeof(r->exits));
    memcpy(v->perf.last_irqs, r->irqs, sizeof(r->irqs)); v->perf.last_edges = r->edges;
    v->perf.last_guest = r->guest_cycles; v->perf.last_monitor = r->monitor_cycles;
    v->perf.last_present = r->present_cycles; v->perf.last_log = r->log_cycles;
    v->perf.last_harvest = r->harvest_cycles;
    memset(bank->ports, 0, sizeof(bank->ports));
    memset(bank->vectors, 0, sizeof(bank->vectors));
    unsigned n = ++v->perf.sample_count;
    /* No UART, console, VFS, waits or allocation. reporting prevents a
     * preempting STATUS call from overwriting this sample during emission. */
    uint64_t begin = dosvm_perf_clock();
    perf_live(v, s, n);
    v->perf.log_cycles += dosvm_perf_clock() - begin;
    v->perf.reporting = false;
#undef DELTA
}

void dosvm_perf_dump(struct dosvm *v)
{
    if (!(g_boot.flags & CBI_F_DIAG_VERBOSE)) return;
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
        rec_diag(dosvm_probe(v), "group=vm_perf_video vm=%u n=%u ms=%u frames=%u flips=%u harvests=%u pages=%u present_rate=%u",
            id, n+1, s->ms, s->frames, s->flips, s->harvests, s->pages, s->present_rate);
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
                id, n+1, perf_names[i], s->cost_count[i], s->cost_us[i],
                s->cost_us[i] * 1000u / s->cost_count[i]);
        rec_diag(dosvm_probe(v), "group=vm_perf_checkpoint vm=%u n=%u hits=%u segmented=%u fallback=%u",
                 id, n+1, s->return_hits, s->return_segmented, s->return_fallbacks);
        rec_diag(dosvm_probe(v), "group=vm_perf_tracker vm=%u n=%u armed=%u hits=%u expired=%u fallback=%u reacquired=%u segmented=%u",
                 id, n+1, s->return_armed, s->return_hits, s->return_expired,
                 s->return_fallbacks, s->return_reacquired, s->return_segmented);
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
