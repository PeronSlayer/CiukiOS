/* f3-13 counters: one memory snapshot per second; records are dumped after
 * the worker stops, so a polled UART cannot consume the game's frame budget.
 * TSC accounting pauses across task switches; it never includes a sleeping
 * monitor's wall time as VM CPU time. SPDX-License-Identifier: GPL-2.0-only */
uint64_t dosvm_perf_clock(void)
{
    return (g_boot.flags & CBI_F_VERBOSE) && g_tsc_per_ms ? read_tsc_serialized() : 0;
}
static void perf_phase(struct dosvm *v, unsigned phase)
{
    uint64_t now = dosvm_perf_clock();
    if (v->perf.phase == 1) v->perf.guest_cycles += now - v->perf.stamp;
    if (v->perf.phase == 2) v->perf.monitor_cycles += now - v->perf.stamp;
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

void dosvm_perf_report(struct dosvm *v)
{
    if (!(g_boot.flags & CBI_F_VERBOSE) || !g_tsc_per_ms || dosvm_in_irq(v) ||
        !v->info.dos_ready || g_ticks - v->perf.report_tick < 1000) return;
    unsigned old_phase = v->perf.phase;
    perf_phase(v, old_phase);
    uint32_t flags = irq_save();
    uint64_t tick = g_ticks, edges = v->pit.edges;
    uint32_t irqs[16]; memcpy(irqs, v->monitor.stats.delivered, sizeof(irqs));
    irq_restore(flags);
#define DELTA(field, last) (v->perf.field - v->perf.last)
    struct dosvm_perf_sample *s = &v->perf.sample[v->perf.sample_count++ % ARRAY_SIZE(v->perf.sample)];
    *s = (struct dosvm_perf_sample){
        .ms = (uint32_t)(tick - v->perf.report_tick),
        .frames = v->video_frames - v->perf.last_frames, .flips = v->video_starts - v->perf.last_starts,
        .harvests = DELTA(harvests, last_harvests), .pages = DELTA(direct_pages, last_pages),
        .converted = DELTA(converted_bytes, last_converted), .rows = DELTA(rows, last_rows),
        .trapped = DELTA(trapped_bytes, last_trapped), .guest_us = perf_us(DELTA(guest_cycles, last_guest)),
        .monitor_us = perf_us(DELTA(monitor_cycles, last_monitor)), .log_us = perf_us(DELTA(log_cycles, last_log)),
        .present_us = perf_us(DELTA(present_cycles, last_present)), .edges = edges - v->perf.last_edges
    };
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
#undef DELTA
}

void dosvm_perf_dump(struct dosvm *v)
{
    if (g_boot.flags & CBI_F_VERBOSE) {
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
    for (uint32_t n = count > retained ? count - retained : 0; n < count; n++) {
        const struct dosvm_perf_sample *s = &v->perf.sample[n % retained];
        unsigned id = v->info.key.id;
        rec_diag(dosvm_probe(v), "group=vm_perf_video vm=%u n=%u ms=%u frames=%u flips=%u harvests=%u pages=%u",
            id, n+1, s->ms, s->frames, s->flips, s->harvests, s->pages);
        rec_diag(dosvm_probe(v), "group=vm_perf_present vm=%u n=%u converted=%u rows=%u present_us=%llu",
            id, n+1, s->converted, s->rows, s->present_us);
        rec_diag(dosvm_probe(v), "group=vm_perf_cpu vm=%u n=%u guest_us=%llu monitor_us=%llu log_us=%llu trapped=%u",
            id, n+1, s->guest_us, s->monitor_us, s->log_us, s->trapped);
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
