/* F3-42: real DOOM instructions and production SB16, with the host CPU and
 * instruction clock used by dosvm_guest. No guest code or disk changes. */
static void isr_stats(const char *phase)
{
    static const char *const paths[DOSVM_IF_PATHS] = {"trampoline", "sti", "v86_iret", "popf", "api", "forced"};
    printf("isr_if phase=%s sampled_ms=%llu clear_ms=%llu outside_ms=%llu max_depth=%u retired=%u expired=%u\n", phase,
           (unsigned long long)gv->perf.if_samples_ms, (unsigned long long)gv->perf.if_clear_ms,
           (unsigned long long)gv->perf.if_outside_ms, gv->perf.isr_max_depth,
           gv->perf.isr_retired, gv->perf.isr_expired);
    for (unsigned i = 0; i < DOSVM_IF_PATHS; i++)
        printf("isr_enable phase=%s path=%s count=%u\n", phase, paths[i], gv->perf.if_enable[i]);
    for (unsigned irq = 0; irq < 16; irq++) {
        const struct dosvm_isr_cost *s = &gv->perf.isr[irq];
        if (!s->count && irq != 0 && irq != 7) continue;
        printf("isr_duration phase=%s irq=%u vector=%02x count=%u avg_us=%llu max_us=%u instruction_ms=%llu max_instruction_ms=%u bins=%u,%u,%u,%u,%u,%u,%u,%u\n",
               phase, irq, s->vector, s->count, (unsigned long long)(s->count ? s->us / s->count : 0), s->max_us,
               (unsigned long long)s->ms, s->max_ms, s->bins[0], s->bins[1], s->bins[2], s->bins[3],
               s->bins[4], s->bins[5], s->bins[6], s->bins[7]);
    }
}
static bool isr_replay(void)
{
    bool sound = getenv("GUEST_MENU_SOUND") != 0;
    CHECK(guest_perf && dosvm_input_focus(dosvm_key(gv), true));
    if (!menu_wait(1000)) return false;
    isr_stats("startup");
    memset(gv->perf.isr, 0, sizeof(gv->perf.isr));
    memset(gv->perf.if_enable, 0, sizeof(gv->perf.if_enable));
    gv->perf.if_samples_ms = gv->perf.if_clear_ms = gv->perf.if_outside_ms = 0;
    gv->perf.isr_max_depth = 0;
    gv->perf.isr_retired = gv->perf.isr_expired = 0;
    memset(&gv->audio.sb.cadence, 0, sizeof(gv->audio.sb.cadence));
    /* Keep the complete measurement bank until the end of this window.
     * This changes only diagnostic snapshot time, never device progress. */
    gv->audio.diag.next_cadence = g_ticks + 6000;
    uint64_t begin = g_ticks;
    uint32_t timer = gv->monitor.stats.delivered[0], reads = gv->kbd.reads;
    uint32_t sb_irqs = gv->monitor.stats.delivered[7];
    uint32_t units = gv->audio.sb.units, edges = gv->audio.sb.irq_edges;
    uint32_t flips = gv->video_starts;
    window_key(0x1D, 1); /* hold fire; actual game, not title/menu */
    window_key(0x148, 1); /* walk towards the first door */
    for (unsigned i = 0; i < 10; i++) {
        window_key(0x39, 1); /* use/open door */
        if (!menu_wait(100)) return false;
        window_key(0x39, 0);
        if (!menu_wait(400)) return false;
    }
    window_key(0x148, 0); window_key(0x1D, 0);
    const struct dosvm_sb_cadence *c = &gv->audio.sb.cadence;
    uint64_t avg = c->irq_entries ? c->irq_latency_sum_us / c->irq_entries : 0;
    bool progress = gv->monitor.stats.delivered[0] > timer && gv->kbd.reads > reads && gv->video_starts > flips;
    bool audio = !sound || (gv->audio.sb.units > units && gv->audio.sb.irq_edges > edges && c->blocks && c->irq_entries);
    uint64_t ms = g_ticks - begin;
    bool target = ms && sound && c->irq_entries && avg <= 2000 && c->if_clear * 20ull <= ms &&
        c->merged * 20ull <= c->blocks && gv->perf.if_clear_ms * 20 <= gv->perf.if_samples_ms &&
        (gv->monitor.stats.delivered[0] - timer) * 1000ull >= 135 * ms &&
        (gv->monitor.stats.delivered[7] - sb_irqs) * 1000ull >= 80 * ms;
    printf("isr_replay sound=%u progress=%u audio=%u ms=%llu irq0=%u irq7=%u keys=%u flips=%u dma_units=%u sb_edges=%u blocks=%u merged=%u entries=%u irq_latency_avg_us=%llu irq_latency_max_us=%u if_clear=%u descheduled=%u parked=%u target=%u\n",
           sound, progress, audio, (unsigned long long)(g_ticks - begin), gv->monitor.stats.delivered[0] - timer,
           gv->monitor.stats.delivered[7] - sb_irqs, gv->kbd.reads - reads, gv->video_starts - flips, gv->audio.sb.units - units, gv->audio.sb.irq_edges - edges,
           c->blocks, c->merged, c->irq_entries, (unsigned long long)avg, c->irq_latency_max_us,
           c->if_clear, c->descheduled, c->parked,
           target);
    isr_stats("shooting");
    return progress && audio && (!sound || target) && !unexpected && !stopped();
}
