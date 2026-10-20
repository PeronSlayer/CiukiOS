/* f3-46 (contract D1) combined replay: a paged DOS VM's host work, in the
 * bounded spans its production paths have between safe points, interleaved
 * with the production audio service. Span costs reuse the f0-41 calibrated
 * operation costs (allocate 4 us, zero 8 us, map 6 us, safe point 0.2 us)
 * and the f0-49 measured pager maxima; waits for disk or page-in I/O leave
 * the CPU to the audio worker (modelled per millisecond). The guest's own
 * IF-off periods and ISRs cost host CPU only in their trap spans. This is a
 * host model of the composition, not a hardware timing claim. */
static void paging_wait(uint64_t ns)
{
    /* The VM worker sleeps; the refill worker runs at every timer tick. */
    while (ns) { uint64_t step = ns > 1000000 ? 1000000 : ns; creation_audio_service(step); ns -= step; }
}
static void test_vm_paging_replay(void)
{
    late_setup();
    modeled_cycles = 0;
    vm_attached_hook = vm_active_hook = vm_nonzero_hook = true;
    audio_service();
    CHECK(audio.running && audio.lead_periods == 8 && !audio.adaptive_lead);
    unsigned grows = late_grow_logs;
    creation_audio_ns = 0;
    uint64_t longest = 0;
#define SPAN(ns) do { uint64_t span_ = (ns); if (span_ > longest) longest = span_; creation_audio_service(span_); } while (0)
    /* 1. Paged creation: 24 record pages and 7 pool tables, one safe point
     * each (allocate, zero, publish); no eager pool loop. */
    for (unsigned i = 0; i < 31; i++) SPAN(4000 + 8000 + 1000 + 200);
    /* 2. DOS/4GW start: 0501h of 16 MiB returns 4096 untouched pages to
     * demand-zero, 16 per safe point; then 2048 first-touch faults (one
     * trap each: allocate, zero, publish pool and alias PTEs). */
    for (unsigned i = 0; i < 4096 / 16; i++) SPAN(16 * 500 + 200);
    for (unsigned i = 0; i < 2048; i++) SPAN(2000 + 4000 + 8000 + 2 * 1000 + 500 + 200);
    /* 3. Five seconds of play: DOOM's 140 Hz flat-stack ISR deliveries,
     * the f3-47 trace's 400 INT 13h requests/s (one in eight misses the
     * cache and waits 8 ms for the disk), guest IF-off periods (no host
     * work), and a pressure episode: 32 page-out batches of 8 at the
     * f0-49 maximum 226 us step plus 8 ms writes, and 256 page-ins (CPU
     * 10 us, 8 ms read wait each, worker blocked at the trap entry). */
    for (unsigned ms_index = 0; ms_index < 5000; ms_index++) {
        if (ms_index % 7 == 0) SPAN(20000 + 200);                    /* IRQ0 to the guest ISR */
        if (ms_index % 5 < 2) {                                      /* 2 requests per 5 ms */
            if ((ms_index / 5 * 2 + ms_index % 5) % 8 == 7) { SPAN(30000); paging_wait(8000000); }
            else SPAN(50000);
        }
        if (ms_index >= 1000 && ms_index < 1032) { SPAN(226000); paging_wait(8000000); }
        if (ms_index >= 2000 && ms_index < 2256) { SPAN(10000); paging_wait(8000000); }
        paging_wait(1000000);
    }
#undef SPAN
    CHECK(late_grow_logs == grows && audio.lead_periods == 8 && !audio.adaptive_lead);
    CHECK(!audio.halts && !audio.deadlines && !ac97.dma_halts && !audio.error);
    CHECK(audio.max_service_gap_us <= 2000 && longest <= 2000000);
    printf("f3-46 combined replay: paged creation, DOS/4GW start (2048 first touches), 140 Hz ISR, disk trace, "
           "256 page-outs/256 page-ins: late-grow=0 lead=%u max_gap_us=%u longest_span_us=%llu PASS\n",
           audio.lead_periods, audio.max_service_gap_us, (unsigned long long)(longest / 1000));
    modeled_cycles = 0;
    late_cleanup();
}
