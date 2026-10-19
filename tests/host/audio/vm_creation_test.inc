/* Actual eager-backing loop replayed into the production audio service. */
#include "vm_creation_model.inc"
static uint64_t creation_audio_ns;
static void creation_audio_service(uint64_t span_ns)
{
    uint64_t previous = creation_audio_ns;
    creation_audio_ns += span_ns;
    /* Independent DMA advances at 48 kHz, even while the creating task
     * cannot service the worker. Accumulate fractions across checkpoints. */
    unsigned frames = (unsigned)(creation_audio_ns * 48000 / 1000000000 -
                                previous * 48000 / 1000000000);
    mock_advance(frames * 2);
    uint64_t ticks = creation_audio_ns / 1000000 - previous / 1000000;
    g_ticks += ticks; hw.tick += ticks;
    modeled_cycles = creation_audio_ns % 1000000 / 1000;
    /* A millisecond timer wake sets need_resched; a checkpoint with no
     * newly ready worker need not invoke it. Retain that scheduling delay. */
    if (ticks) audio_service();
}
static void test_vm_creation_lead(void)
{
    for (unsigned points = 0; points < 2; points++) {
        late_setup();
        modeled_cycles = 0;
        vm_attached_hook = vm_active_hook = vm_nonzero_hook = true;
        audio_service();
        CHECK(audio.running && audio.lead_periods == 8 && !audio.adaptive_lead);
        unsigned grows = late_grow_logs;
        creation_audio_ns = 0;
        struct creation_result result = creation_replay(points != 0, creation_audio_service);
        CHECK(result.pages == 6144 && !audio.error);
        if (points) {
            CHECK(late_grow_logs == grows && audio.lead_periods == 8 && !audio.adaptive_lead);
            CHECK(!audio.halts && !audio.deadlines && !ac97.dma_halts);
            CHECK(audio.max_service_gap_us <= 2000 && ac97.fetched > 0);
        } else {
            CHECK(late_grow_logs == grows + 1 && audio.lead_periods == 28);
        }
        printf("F0-41 audio creation: points=%u lead_periods=%u late-grow=%u max_gap_us=%u PASS\n",
               result.points, audio.lead_periods, late_grow_logs - grows, audio.max_service_gap_us);
        modeled_cycles = 0;
        late_cleanup();
    }
}
