/* 2026-10-24 lead decision: a grown transport lead survives the idle stop.
 * Replays the session sequence of the QEMU serial of image 9fe0a02e
 * (docs/validation/doom-launch-audio-2026-10-24.md) through the production
 * service and the ICH DMA fake: 1 ms ticks, 48 kHz, the refill worker every
 * 4 ms (its running deadline) except during the recorded stall.
 *   L400-L451  boot startup+logon sound, 41.8 ms service gap (gap_us=41829),
 *              underrun at published=55
 *   L1426      window sound 15 cpustat windows (30 s) later, 73 ms stretch,
 *              underrun at published=60
 *   L2492      timed demo: late-grow to 28 periods, idle-stop at L7351
 *   L7613      window_close 4 windows (8 s) later, 78 ms stretch, underrun
 *              at published=15: 90 ms hole at WAV 125.49 s
 *   L8659      DOOM 5 windows (10 s) after the click session's stop, 87 ms
 *              stretch, underrun at published=41 (L8716)
 * Before the decision every idle stop reset the lead to 8 periods and an
 * underrun did not grow it, so each of these sessions began with 42.7 ms.
 * SPDX-License-Identifier: GPL-2.0-only */
static void hold_ms(unsigned ms, unsigned stall_at, unsigned stall_ms)
{
    for (unsigned t = 0; t < ms; t++) {
        mock_advance(96); g_ticks++; hw.tick++;
        bool stalled = stall_ms && t >= stall_at && t < stall_at + stall_ms;
        bool resumed = stall_ms && t == stall_at + stall_ms;
        if (!stalled && (resumed || t % 4 == 3)) audio_service();
    }
}
struct hold_session { unsigned start_published; uint64_t underruns; unsigned lead_after; };
/* One sound: the source starts, plays active_ms with one stall, stops; the
 * transport plays its idle tail and stops (idle-stop). */
static struct hold_session hold_session(bool vm, unsigned active_ms, unsigned stall_at, unsigned stall_ms)
{
    struct hold_session r = {0};
    uint64_t underruns = audio.underruns;
    vm_attached_hook = vm; vm_active_hook = vm_nonzero_hook = true;
    CHECK(!audio.running);
    audio_service();
    CHECK(audio.running && !audio.error);
    r.start_published = (unsigned)ac97.published;
    hold_ms(active_ms, stall_at, stall_ms);
    vm_active_hook = vm_nonzero_hook = false;
    hold_ms(AUDIO_IDLE_TAIL_MS + 300, 0, 0);
    CHECK(!audio.running && !audio.error);
    r.underruns = audio.underruns - underruns;
    r.lead_after = held_lead;
    return r;
}
static void hold_idle(unsigned ms) { g_ticks += ms; hw.tick += ms; audio_service(); CHECK(!audio.running); }
static void test_lead_hold_replay(void)
{
    /* The kept lead as a function of the idle time (no session replay). */
    CHECK(audio_held_lead(28, 8, 0) == 28 && audio_held_lead(28, 8, AUDIO_LEAD_HOLD_MS) == 28);
    CHECK(audio_held_lead(28, 8, AUDIO_LEAD_HOLD_MS + 1) == 26 && audio_held_lead(28, 8, 40000) == 24);
    CHECK(audio_held_lead(28, 8, 75000) == 10 && !audio_held_lead(28, 8, 80000) && !audio_held_lead(28, 8, UINT32_MAX));
    CHECK(!audio_held_lead(8, 8, 0) && !audio_held_lead(0, 8, 0) && audio_held_lead(9, 8, 31000) == 0);

    late_setup(); held_lead = 0; held_since = 0;
    /* L400-L451: the first sound after boot has nothing to keep: nominal
     * lead, the f4-31/f4-32 latency bound, and the recorded underrun. */
    struct hold_session boot = hold_session(false, 550, 280, 42);
    printf("lead hold: boot start=%u underruns=%llu held=%u\n", boot.start_published,
           (unsigned long long)boot.underruns, boot.lead_after);
    CHECK(boot.start_published == AUDIO_LEAD && boot.underruns == 1);
    CHECK(boot.lead_after == 28); /* the underrun grew the lead, the stop kept it */
    /* L1426, 15 cpustat windows (30 s) after the boot sound began, i.e.
     * about 28.6 s after its 1.35 s session stopped: the kept lead covers
     * the 73 ms stretch. */
    hold_idle(28600);
    struct hold_session window = hold_session(false, 310, 30, 73);
    printf("lead hold: window_open after 30 s start=%u underruns=%llu\n", window.start_published,
           (unsigned long long)window.underruns);
    CHECK(window.start_published == 28 && !window.underruns);
    /* L2492: a long session that grew late; L7613, 8 s after its stop. */
    hold_idle(70000);
    struct hold_session demo = hold_session(true, 2000, 400, 131);
    CHECK(demo.lead_after == 28);
    hold_idle(8000);
    struct hold_session close = hold_session(false, 310, 40, 78);
    printf("lead hold: window_close after 8 s start=%u underruns=%llu\n", close.start_published,
           (unsigned long long)close.underruns);
    CHECK(close.start_published == 28 && !close.underruns);
    /* L8659: DOOM 10 s after the click session's stop, 87 ms stretch. */
    hold_idle(10000);
    struct hold_session doom = hold_session(true, 1000, 130, 87);
    printf("lead hold: DOOM after 10 s start=%u underruns=%llu\n", doom.start_published,
           (unsigned long long)doom.underruns);
    CHECK(doom.start_published == 28 && !doom.underruns);
    /* Decay: about 39.3 s after the stop (the tail ran ~0.3 s past it) the
     * kept lead is 28 - 2 steps of 2 periods. */
    hold_idle(39000);
    CHECK(g_ticks - held_since > 35000 && g_ticks - held_since <= 40000);
    struct hold_session later = hold_session(false, 200, 0, 0);
    CHECK(later.start_published == 24 && !later.underruns);
    /* After a long idle the first sound has the nominal lead again. */
    hold_idle(120000);
    struct hold_session quiet = hold_session(false, 200, 0, 0);
    printf("lead hold: after 120 s start=%u (%u us of lead)\n", quiet.start_published,
           quiet.start_published * AUDIO_QUANTUM * 1000u / 48u);
    CHECK(quiet.start_published == AUDIO_LEAD && !quiet.underruns);
    vm_attached_hook = false; held_lead = 0; held_since = 0;
    late_cleanup();
    puts("audio lead hold: grown lead kept 30 s after the idle stop then decayed, underrun grows it, first sound after a long idle at the nominal lead PASS");
}
