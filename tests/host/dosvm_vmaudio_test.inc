/* f4-05 T0 "DOS audio" output parts: production f3-05 devices, the f4-05 VM
 * sources (audio/vm_source.c), the integer OPL3 port (audio/opl3.c) and the
 * f4-01 mixer (audio/mixer.c), linked as their own translation units. A fake
 * transport keeps seven periods queued; the VM clock advances in 1 ms ticks
 * in step with the output frames. Included by dosvm_test.c after
 * dosvm_audio_test.inc, whose device helpers it uses. */
#include <ciuki/audio.h>
#include <ciuki/audio_vm.h>
#include <ciuki/opl3.h>

static unsigned audio_wakes;
void audio_wake(void) { audio_wakes++; }

#define VQ_CAP 40000u
static struct audio_frame vq_out[AUDIO_QUANTUM];
static int16_t vq_cap[VQ_CAP][2];
static unsigned vq_n;
static uint64_t vq_frames, vq_ticks0;
static void (*vq_service)(void *);
static void *vq_arg;

static void vq_begin(void)
{
    audio.ready = true;
    audio.caps.output_rate=48000;
    audio.master = (struct ciuki_mixer_master){ .version = 1, .size = 32, .left = 100, .right = 100 };
    audio.output = 2048; audio.played_output = 256;
    vq_n = 0; vq_frames = 0; vq_ticks0 = g_ticks; vq_service = 0; vq_arg = 0;
}
static void vq_end(void) { audio.ready = false; vq_service = 0; }
static void vq_mix(void)
{
    audio.played_output = audio.output - 7 * AUDIO_QUANTUM; /* the fake transport: seven periods queued */
    audio_mix(vq_out, AUDIO_QUANTUM);
    audio_vm_published(); /* this fake transport accepts the complete period */
    for (unsigned k = 0; k < AUDIO_QUANTUM; k++) if (vq_n < VQ_CAP) {
        vq_cap[vq_n][0] = vq_out[k].left; vq_cap[vq_n][1] = vq_out[k].right; vq_n++;
    }
}
static void vq_time(unsigned frames)
{
    vq_frames += frames;
    while ((g_ticks - vq_ticks0) * audio_output_rate() < vq_frames * 1000) { ms(1); if (vq_service) vq_service(vq_arg); }
}
static void vq_run(unsigned quanta) { for (unsigned i = 0; i < quanta; i++) { vq_mix(); vq_time(AUDIO_QUANTUM); } }
static struct audio_vm_stats vstats(struct dosvm *v) { struct audio_vm_stats s; CHECK(audio_vm_stats(dosvm_key(v), &s)); return s; }
static void unity(struct dosvm *v) { for (uint8_t r = 0x30; r <= 0x35; r++) mixer_w(v, r, 0xF8); }
static unsigned first_sound(unsigned from)
{
    for (unsigned i = from; i < vq_n; i++) if (vq_cap[i][0] || vq_cap[i][1]) return i;
    return vq_n;
}
/* The f4-01 interpolation applied to a whole source sequence, from frac 0. */
static unsigned vq_reference(int16_t (*src)[2], unsigned frames, uint32_t rate, int16_t (*out)[2], unsigned max)
{
    unsigned n = 0; uint64_t idx = 0; uint32_t frac = 0;
    while (idx < frames && n < max) {
        const int16_t *a = src[idx], *b = idx + 1 < frames ? src[idx + 1] : a;
        for (unsigned c = 0; c < 2; c++) out[n][c] = (int16_t)(a[c] + ((int64_t)b[c] - a[c]) * frac / 48000);
        n++; frac += rate;
        while (frac >= 48000) { frac -= 48000; idx++; }
    }
    return n;
}

static void test_vm_attach(void)
{
    audio.ready = false;
    struct audio_vm_stats st;
    struct dosvm *v = audio_vm_create(); struct dosvm_key key = dosvm_key(v);
    CHECK(!audio_vm_stats(key, &st) && !ainfo(v).consumer); /* no backend: f3-05 keeps discarding */
    destroy(v, true);
    vq_begin();
    unsigned wakes = audio_wakes;
    v = audio_vm_create(); key = dosvm_key(v);
    CHECK(audio_vm_stats(key, &st) && st.state == AUDIO_VM_PENDING && st.epoch && audio_wakes == wakes + 1 && ainfo(v).consumer);
    unsigned observed_wakes = audio_wakes;
    CHECK(!audio_vm_playing() && audio_wakes == observed_wakes); /* observation never wakes the worker */
    vq_time(1200); /* 25 ms of VM clock: the anchor lies inside the timeline */
    vq_run(1);
    st = vstats(v); CHECK(st.state == AUDIO_VM_ACTIVE && st.rendered == AUDIO_QUANTUM && st.lead == (int32_t)AUDIO_VM_LATENCY);
    /* Another consumer (the f3 probe) revokes the source: it ends with a counter. */
    uint32_t token; CHECK(!dosvm_audio_attach(key, &token));
    vq_run(1); st = vstats(v); CHECK(st.state == AUDIO_VM_FREE && st.revoked == 1);
    destroy(v, true); CHECK(audio_vm_stats(key, &st) && st.state == AUDIO_VM_FREE);
    /* The release path revokes before f3-05 frees the queues (ASan guards the pages). */
    v = audio_vm_create(); key = dosvm_key(v); vq_run(1);
    CHECK(vstats(v).state == AUDIO_VM_ACTIVE);
    destroy(v, true); CHECK(audio_vm_stats(key, &st) && st.state == AUDIO_VM_FREE && !st.revoked);
    CHECK(audio_vm_set_gain(key, 50, 50, false) == -EINVAL);
    vq_run(2);
    vq_end(); empty();
}

/* Every advertised PCM layout reaches the mix exactly at 24000 Hz (2:1). */
static void vm_mode(struct dosvm *v, uint8_t cmd, uint8_t mode, unsigned units, const uint8_t *bytes, unsigned nbytes)
{
    bool bits16 = cmd < 0xC0, stereo = mode & 0x20, sign = mode & 0x10, autoinit = cmd & 4;
    CHECK(!dosvm_write(v, 0x20000, bytes, nbytes));
    if (bits16) dma16(v, 5, 0x20000, (uint16_t)(nbytes / 2 - 1), autoinit); else dma8(v, 1, 0x20000, (uint16_t)(nbytes - 1), autoinit);
    unsigned start = vq_n;
    dsp(v, 0x41); dsp(v, 0x5D); dsp(v, 0xC0); /* 24000 */
    dsp(v, cmd); dsp(v, mode); dsp(v, (uint8_t)(units - 1)); dsp(v, (uint8_t)((units - 1) >> 8));
    if (autoinit) dsp(v, bits16 ? 0xD9 : 0xDA);
    for (unsigned q = 0; q < 10; q++) { vq_mix(); vq_time(AUDIO_QUANTUM); a_in(v, SB + 0xE); a_in(v, SB + 0xF); }
    v->monitor.pic[0].irr = 0;
    static int16_t src[64][2], ref[160][2];
    unsigned frames = stereo ? units / 2 : units;
    for (unsigned f = 0; f < frames; f++) for (unsigned c = 0; c < 2; c++) {
        unsigned unit = stereo ? f * 2 + c : f;
        int32_t x;
        if (bits16) { uint16_t w = (uint16_t)(bytes[unit * 2] | (bytes[unit * 2 + 1] << 8)); x = sign ? (int16_t)w : (int32_t)w - 32768; }
        else x = sign ? (int8_t)bytes[unit] * 256 : ((int)bytes[unit] - 128) * 256;
        src[f][c] = (int16_t)x;
    }
    unsigned n = vq_reference(src, frames, 24000, ref, 160);
    CHECK(n == frames * 2);
    unsigned at = first_sound(start);
    CHECK(at + n < vq_n);
    for (unsigned j = 0; j < n; j++) CHECK(vq_cap[at + j][0] == ref[j][0] && vq_cap[at + j][1] == ref[j][1]);
    CHECK(!vq_cap[at + n][0] && !vq_cap[at + n][1] && first_sound(at + n) == vq_n); /* then silence */
}
static void test_vm_modes(void)
{
    vq_begin();
    struct dosvm *v = audio_vm_create(); unity(v); dsp_reset(v);
    vq_time(1200); vq_run(2);
    uint8_t bytes[64]; for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(i * 37 + 11);
    for (unsigned bits16 = 0; bits16 < 2; bits16++) for (unsigned autoinit = 0; autoinit < 2; autoinit++)
        for (unsigned mode = 0; mode < 4; mode++)
            vm_mode(v, (uint8_t)((bits16 ? 0xB0 : 0xC0) | (autoinit ? 6 : 0)), (uint8_t)(mode << 4), bits16 ? 16 : 32, bytes, 32);
    struct audio_vm_stats st = vstats(v);
    CHECK(st.blocks == 16 && !st.late_frames && !st.starved && !st.holds && !st.skips && !st.seq_gaps && st.streams == 16);
    /* DSP 10h direct DAC: a held level from its timeline frame, 100 ms. */
    audio_vm_tap_reset(dosvm_key(v));
    dsp(v, 0x10); dsp(v, 0xC0);
    vq_run(30);
    st = vstats(v); CHECK(st.dac_events == 1 && st.pcm.frames == DOSVM_AUDIO_HZ / 10 && st.pcm.peak[0] == 16384 && st.pcm.peak[1] == 16384);
    /* Creative ADPCM is not advertised: timing and IRQ kept, silence, counted. */
    audio_vm_tap_reset(dosvm_key(v));
    dma8(v, 1, 0x20000, 9, false); dsp(v, 0x74); dsp(v, 9); dsp(v, 0);
    vq_run(8); a_in(v, SB + 0xE); v->monitor.pic[0].irr = 0;
    st = vstats(v); CHECK(st.unsupported_blocks == 1 && !st.pcm.frames);
    dsp_reset(v); destroy(v, true); vq_end(); empty();
}

/* A guest refilling the half just played with the next block (8-bit: the
 * f3-05 pattern 0x20 + sequence; 16-bit: a rising ramp). */
struct vg { struct dosvm *v; uint32_t buffer; unsigned half, seq, serviced, stop_at, units; bool bits16, eligible; };
static int16_t vg_value(const struct vg *g, unsigned seq, unsigned i)
{
    return g->bits16 ? (int16_t)((int)(seq * 64 + i) * 5 - 20000) : (int16_t)(((int)(0x20 + seq) - 128) * 256);
}
static void vg_fill(struct vg *g, unsigned half, unsigned seq)
{
    uint8_t b[256]; unsigned n = g->units * (g->bits16 ? 2 : 1);
    for (unsigned i = 0; i < g->units; i++) {
        if (g->bits16) { uint16_t w = (uint16_t)vg_value(g, seq, i); b[i * 2] = (uint8_t)w; b[i * 2 + 1] = (uint8_t)(w >> 8); }
        else b[i] = (uint8_t)(0x20 + seq);
    }
    CHECK(!dosvm_write(g->v, g->buffer + half * n, b, n));
}
static void vg_service(void *arg)
{
    struct vg *g = arg; struct dosvm *v = g->v;
    for (unsigned guard = 0; guard < 64 && g->eligible && v86_irq_pending(&v->monitor) == 7; guard++) {
        CHECK(dosvm_pic_ack(&v->monitor) == 0x0F);
        CHECK(mixer_r(v, 0x82) & (g->bits16 ? 2 : 1));
        g->serviced++;
        vg_fill(g, g->half, g->seq++); g->half ^= 1;
        if (g->serviced == g->stop_at) dsp(v, g->bits16 ? 0xD9 : 0xDA);
        a_in(v, SB + (g->bits16 ? 0xF : 0xE));
        device_out(v, 0x20, 0x20);
    }
}
static void vg_start(struct vg *g, uint16_t rate)
{
    struct dosvm *v = g->v;
    vg_fill(g, 0, g->seq++); vg_fill(g, 1, g->seq++);
    if (g->bits16) dma16(v, 5, g->buffer, (uint16_t)(g->units * 2 - 1), true);
    else dma8(v, 1, g->buffer, (uint16_t)(g->units * 2 - 1), true);
    dsp(v, 0x41); dsp(v, (uint8_t)(rate >> 8)); dsp(v, (uint8_t)rate);
    if (g->bits16) { dsp(v, 0xB6); dsp(v, 0x10); dsp(v, (uint8_t)(g->units - 1)); dsp(v, 0); } /* signed mono, auto-init */
    else { dsp(v, 0x48); dsp(v, (uint8_t)(g->units - 1)); dsp(v, 0); dsp(v, 0x1C); }
}
/* The contract's regression through the whole output path: a 64-unit 8-bit
 * auto-init double buffer over 2048 units, plus a 16-bit ramp checking the
 * fractional phase across fetches, quanta and contiguous segments. */
static void test_vm_double_buffer(void)
{
    for (unsigned variant = 0; variant < 2; variant++) {
        vq_begin();
        struct dosvm *v = audio_vm_create(); struct dosvm_key key = dosvm_key(v); unity(v); dsp_reset(v);
        vq_time(1200); vq_run(2);
        struct vg g = { v, 0x30000, 0, 0, 0, variant ? 19 : 31, 64, variant == 1, true };
        vq_service = vg_service; vq_arg = &g;
        audio_vm_tap_reset(key);
        struct dosvm_audio_info b = ainfo(v);
        unsigned start = vq_n, blocks = g.stop_at + 1;
        vg_start(&g, 22050);
        for (unsigned q = 0; q < 200 && (ainfo(v).active || vstats(v).log_count < blocks); q++) vq_run(1);
        vq_run(4);
        struct dosvm_audio_info a = ainfo(v);
        struct audio_vm_stats st = vstats(v);
        CHECK(!a.active && a.boundaries - b.boundaries == blocks && a.services - b.services == blocks && g.serviced == blocks);
        CHECK(st.log_count == blocks && !st.late_frames && !st.starved && !st.holds && !st.skips && !st.seq_gaps && st.streams == 1);
        for (unsigned k = 0; k < blocks; k++) {
            const struct audio_vm_block_log *l = &st.log[k];
            CHECK(l->frames == 64 && l->flags == DOSVM_BLOCK_COMPLETE && l->first[0] == vg_value(&g, k, 0) && l->first[1] == l->first[0]);
            if (k) CHECK(l->sequence == st.log[k - 1].sequence + 1 && l->first[0] != st.log[k - 1].first[0]); /* fresh, no stale half */
        }
        static int16_t src[2048][2], ref[4600][2];
        unsigned frames = blocks * 64;
        for (unsigned f = 0; f < frames; f++) src[f][0] = src[f][1] = vg_value(&g, f / 64, f % 64);
        unsigned n = vq_reference(src, frames, 22050, ref, 4600);
        unsigned at = first_sound(start);
        CHECK(n > 0 && at + n <= vq_n && st.pcm.frames == n && st.pcm_frames == frames);
        for (unsigned j = 0; j < n; j++) CHECK(vq_cap[at + j][0] == ref[j][0] && vq_cap[at + j][1] == ref[j][1]);
        CHECK(first_sound(at + n) == vq_n);
        dsp_reset(v); destroy(v, true); vq_end(); empty();
    }
}

/* A pause shorter than the render lag: the resumed segment is already queued
 * when the stream reaches the pause, and must start at its own stamp after
 * the gap, not be appended to the stream (no prefetch across segments). */
static void test_vm_gap(void)
{
    vq_begin();
    struct dosvm *v = audio_vm_create(); unity(v); dsp_reset(v);
    vq_time(1200); vq_run(2);
    uint8_t ring[160]; memset(ring, 0x90, sizeof(ring)); CHECK(!dosvm_write(v, 0x30000, ring, sizeof(ring)));
    dma8(v, 1, 0x30000, 159, true);
    unsigned start = vq_n;
    dsp(v, 0x41); dsp(v, 0x1F); dsp(v, 0x40); dsp(v, 0x48); dsp(v, 79); dsp(v, 0); dsp(v, 0x1C); /* 10 ms blocks */
    vq_time(4 * 48); dsp(v, 0xD0);  /* pause after 4 ms */
    vq_time(6 * 48); dsp(v, 0xD4);  /* continue 6 ms later */
    dsp(v, 0xDA);                   /* exit at the end of this block */
    for (unsigned q = 0; q < 16; q++) { vq_run(1); a_in(v, SB + 0xE); }
    v->monitor.pic[0].irr = 0;
    unsigned a0 = first_sound(start), a1 = a0;
    while (a1 < vq_n && vq_cap[a1][0]) a1++;
    unsigned b0 = first_sound(a1), b1 = b0;
    while (b1 < vq_n && vq_cap[b1][0]) b1++;
    struct audio_vm_stats st = vstats(v);
    CHECK(a1 > a0 && b0 < vq_n && first_sound(b1) == vq_n);
    CHECK(b0 - a1 >= 5 * 48 && b0 - a1 <= 7 * 48);         /* the 6 ms pause, at tick granularity */
    CHECK((a1 - a0) + (b1 - b0) == 80 * 6 && vq_cap[a0][0] == 16 * 256);
    CHECK(st.streams == 2 && !st.starved && !st.late_frames && st.blocks == 1 && st.segments == 2);
    dsp_reset(v); destroy(v, true); vq_end(); empty();
}

/* A stalled guest (never eligible) only silences its own source: native
 * playback and another VM render every quantum, unchanged. */
static struct audio_frame native_ring[2][1024];
static void test_vm_stall(void)
{
    vq_begin();
    struct dosvm *v = audio_vm_create(), *w = audio_vm_create(); unity(v); unity(w); dsp_reset(v); dsp_reset(w);
    for (unsigned i = 0; i < 1024; i++) { native_ring[0][i] = native_ring[1][i] = (struct audio_frame){ 1000, -1000 }; }
    struct audio_stream native = { .ring = { native_ring[0], native_ring[1] }, .rate = AUDIO_RATE, .format = CIUKI_AUDIO_S16LE,
                                   .channels = 2, .gain = { 100, 100 }, .queued = AUDIO_RING, .started = true, .configured = true };
    audio.streams[0] = &native;
    vq_time(1200); vq_run(2);
    uint8_t quiet[160]; memset(quiet, 0x80, sizeof(quiet)); CHECK(!dosvm_write(v, 0x30000, quiet, sizeof(quiet)));
    dma8(v, 1, 0x30000, 159, true);
    dsp(v, 0x41); dsp(v, 0x1F); dsp(v, 0x40); dsp(v, 0x48); dsp(v, 79); dsp(v, 0); dsp(v, 0x1C); /* 10 ms blocks, never serviced */
    struct vg g = { w, 0x40000, 0, 0, 0, 0, 64, true, true };
    vq_service = vg_service; vq_arg = &g;
    vg_start(&g, 22050);
    unsigned start = vq_n;
    uint64_t consumed = native.consumed;
    for (unsigned q = 0; q < 24; q++) { native.queued = AUDIO_RING; vq_run(1); }
    struct audio_vm_stats sv = vstats(v), sw = vstats(w);
    CHECK(native.consumed - consumed == 24 * AUDIO_QUANTUM && !native.starvation);
    CHECK(sv.rendered == 26 * AUDIO_QUANTUM && sw.rendered == 26 * AUDIO_QUANTUM);
    CHECK(sv.late_blocks >= 5 && sv.stale_blocks >= 4 && !sv.pcm.frames && ainfo(v).late >= 5 && !v->monitor.vif);
    CHECK(sw.log_count >= 20 && !sw.late_frames && !sw.starved && !sw.seq_gaps && !sw.late_blocks);
    for (unsigned k = 1; k < sw.log_count && k < AUDIO_VM_LOG; k++) CHECK(sw.log[k].first[0] == vg_value(&g, k, 0));
    /* Output = native + w's ramp (v contributes the replayed silent halves). */
    static int16_t src[2048][2], ref[4600][2];
    for (unsigned f = 0; f < 2048; f++) src[f][0] = src[f][1] = vg_value(&g, f / 64, f % 64);
    unsigned at = start;
    while (at < vq_n && vq_cap[at][0] == 1000 && vq_cap[at][1] == -1000) at++;
    unsigned n = vq_reference(src, 2048, 22050, ref, 4600);
    /* w keeps playing past the 2048 reference frames: the last source frame's
     * interval interpolates toward frame 2048, so it is left out. */
    CHECK(at + n < vq_n);
    for (unsigned j = 0; j + 3 < n; j++)
        CHECK(vq_cap[at + j][0] == ref[j][0] + 1000 && vq_cap[at + j][1] == ref[j][1] - 1000);
    audio.streams[0] = 0;
    dsp_reset(v); dsp_reset(w); destroy(v, true); destroy(w, true); vq_end(); empty();
}

/* Pacing: a frozen VM clock makes the source hold (silence, no timeline);
 * a stalled worker makes it skip; a missed quantum re-anchors. The output
 * of a rising ramp never goes back: no committed PCM is replayed. */
static void test_vm_pacing(void)
{
    vq_begin();
    struct dosvm *v = audio_vm_create(); unity(v); dsp_reset(v);
    vq_time(1200); vq_run(2);
    struct vg g = { v, 0x30000, 0, 0, 0, 0, 64, true, true };
    vq_service = vg_service; vq_arg = &g;
    vg_start(&g, 22050);
    vq_run(10);
    for (unsigned q = 0; q < 6; q++) vq_mix(); /* VM clock frozen */
    struct audio_vm_stats st = vstats(v);
    CHECK(st.holds >= 1 && st.hold_frames > 0 && !st.skips && st.lag_min < (int32_t)AUDIO_VM_FLOOR);
    vq_run(10);
    vq_time(120 * 48); /* worker stalled 120 ms, guest serviced (inside f3-05's 64 segments) */
    vq_run(1);
    st = vstats(v);
    CHECK(st.skips == 1 && st.skipped_frames >= 120 * 48 - 2 * AUDIO_VM_SKIP_BAND && st.late_frames > 0);
    vq_run(6);
    audio.output += 4 * AUDIO_QUANTUM; /* quanta published without this source */
    vq_run(4);
    st = vstats(v); CHECK(st.reanchors == 1);
    int32_t last = INT32_MIN; unsigned sounds = 0;
    for (unsigned i = 0; i < vq_n; i++) {
        if (!vq_cap[i][0]) continue;
        CHECK(vq_cap[i][0] >= last && vq_cap[i][1] == vq_cap[i][0]);
        last = vq_cap[i][0]; sounds++;
    }
    CHECK(sounds > 4000);
    dsp_reset(v); destroy(v, true); vq_end(); empty();
}

/* The transport's idle tail: audio_service's refill policy (audio_refill_lead)
 * with the AC'97 DMA modelled period by period (start once AUDIO_LEAD periods
 * are published, one period fetched per 256 frames of clock, the idle tail
 * stopped after AUDIO_IDLE_TAIL_MS or at an underrun). VMPLAY's stage 3
 * (QEMU campaign 12, image e0af9210) starts 150 ms after stage 2 ends, while
 * the idle tail runs: every block must render, as on a stopped transport. */
struct vt { bool running; uint64_t published, retired, start, base, idle_since; unsigned max_lead; };
static struct vt vt;
static void vt_service(void)
{
    uint64_t now = g_ticks * 48;
    if (vt.running) {
        uint64_t played = now - vt.start;
        if (played >= vt.published * AUDIO_QUANTUM) { /* the DMA halted at the last valid buffer */
            CHECK(vt.idle_since);                    /* only the idle tail may underrun */
            vt.running = false; vt.base = audio.output; vt.published = vt.retired = 0; vt.idle_since = 0;
        } else { vt.retired = played / AUDIO_QUANTUM; audio.played_output = vt.base + played; }
    }
    bool active = audio_vm_active();
    bool tail_start = !active && vt.running && !vt.idle_since;
    if (tail_start) vt.idle_since = g_ticks;
    unsigned limit, n = 0, lead = audio_refill_lead(active, tail_start, audio_vm_attached(), &limit);
    while ((active || vt.running) && vt.published - vt.retired < lead && n++ < limit) {
        if (!vt.running) audio.output += AUDIO_QUANTUM;
        else audio_mix(vq_out, AUDIO_QUANTUM);
        vt.published++;
        if (!vt.running && vt.published == AUDIO_LEAD) { vt.running = true; vt.start = now; }
    }
    if (vt.published - vt.retired > vt.max_lead) vt.max_lead = (unsigned)(vt.published - vt.retired);
    if (!active && vt.running && g_ticks - vt.idle_since >= AUDIO_IDLE_TAIL_MS) {
        vt.running = false; vt.base = audio.output; vt.published = vt.retired = 0; vt.idle_since = 0;
    } else if (active) vt.idle_since = 0;
}
static void vt_ms(struct vg *g, unsigned n) { for (unsigned i = 0; i < n; i++) { ms(1); if (g) vg_service(g); vt_service(); } }
static unsigned vt_fresh(const struct audio_vm_stats *st, const struct vg *g, unsigned blocks)
{
    unsigned fresh = 0;
    for (unsigned k = 0; k < blocks && k < st->log_count; k++)
        fresh += st->log[k].frames == 64 && st->log[k].flags == DOSVM_BLOCK_COMPLETE && st->log[k].first[0] == vg_value(g, k, 0);
    return fresh;
}
static void test_vm_idle_tail(void)
{
    unsigned limit;
    CHECK(audio_refill_lead(true, false, true, &limit) == AUDIO_LEAD && limit == 1);
    CHECK(audio_refill_lead(false, true, false, &limit) == AUDIO_TAIL_LEAD && limit == AUDIO_TAIL_LEAD); /* native-only tail unchanged */
    audio.ready = true;
    audio.master = (struct ciuki_mixer_master){ .version = 1, .size = 32, .left = 100, .right = 100 };
    audio.output = audio.played_output = 0;
    memset(&vt, 0, sizeof(vt));
    struct dosvm *v = audio_vm_create(); struct dosvm_key key = dosvm_key(v); unity(v); dsp_reset(v);
    vt_ms(0, 30);
    CHECK(!vt.running); /* an idle VM leaves the transport stopped */
    /* Stage 2 on a stopped transport: 32 blocks of 64 units at 8000 Hz. */
    struct vg a = { v, 0x30000, 0, 0, 0, 31, 64, false, true };
    audio_vm_tap_reset(key);
    vg_start(&a, 8000);
    for (unsigned t = 0; t < 2000 && (ainfo(v).active || vstats(v).log_count < 32); t++) vt_ms(&a, 1);
    struct audio_vm_stats st = vstats(v);
    CHECK(a.serviced == 32 && st.log_count == 32 && vt_fresh(&st, &a, 32) == 32 && !st.late_frames && !st.skips);
    /* Stage 3, 150 ms later: 16-bit at 12000 Hz, 64-frame blocks (5.3 ms), 8 blocks. */
    vt_ms(&a, 150);
    CHECK(vt.running && vt.idle_since);
    audio_vm_tap_reset(key);
    struct audio_vm_stats before = vstats(v);
    struct vg b = { v, 0x30000, 0, 0, 0, 7, 64, true, true };
    vg_start(&b, 12000);
    for (unsigned t = 0; t < 2000 && (ainfo(v).active || vstats(v).log_count < 8); t++) vt_ms(&b, 1);
    vt_ms(&b, 150);
    st = vstats(v);
    unsigned fresh = vt_fresh(&st, &b, 8);
    printf("idle tail: stage3 blocks=%u fresh=%u frames=%llu late=%llu skips=%u skipped=%llu holds=%u max_lead_periods=%u\n",
           st.log_count, fresh, (unsigned long long)st.pcm.frames, (unsigned long long)(st.late_frames - before.late_frames),
           st.skips - before.skips, (unsigned long long)(st.skipped_frames - before.skipped_frames), st.holds - before.holds, vt.max_lead);
    CHECK(b.serviced == 8 && st.log_count == 8 && fresh == 8 && st.pcm.frames == 8 * 64 * 4);
    CHECK(st.late_frames == before.late_frames && st.skips == before.skips && st.starved == before.starved);
    vt_ms(&b, AUDIO_IDLE_TAIL_MS + 50);
    CHECK(!vt.running && audio_vm_attached() && vt.max_lead == AUDIO_LEAD); /* the shallow tail still ends the transport */
    dsp_reset(v); destroy(v, true);
    audio.ready = false; empty();
}

/* CT1745 master x voice (x output gain) and the source gain/mute. */
static void test_vm_gains(void)
{
    vq_begin();
    struct dosvm *v = audio_vm_create(); struct dosvm_key key = dosvm_key(v); unity(v); dsp_reset(v);
    vq_time(1200); vq_run(2);
    uint8_t words[128]; for (unsigned i = 0; i < 64; i++) { words[i * 2] = 0x10; words[i * 2 + 1] = 0x27; } /* 10000 */
    static const struct { uint8_t reg, value; unsigned gain; bool mute; uint32_t peak; } cases[] = {
        { 0x32, 0xF8, 100, false, 10000 }, /* 0 dB */
        { 0x32, 0xC8, 100, false, 2511 },  /* voice 25: -12 dB, Q16 16462 */
        { 0x41, 0x40, 100, false, 19952 }, /* output gain +6 dB, Q16 130762 */
        { 0x41, 0x00, 50, false, 5000 },   /* source gain 50 */
        { 0x41, 0x00, 100, true, 0 },      /* muted: consumed, silent */
    };
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        mixer_w(v, cases[i].reg, cases[i].value); mixer_w(v, (uint8_t)(cases[i].reg + 1), cases[i].value);
        if (cases[i].reg == 0x41) { mixer_w(v, 0x32, 0xF8); mixer_w(v, 0x33, 0xF8); }
        CHECK(!audio_vm_set_gain(key, cases[i].gain, cases[i].gain, cases[i].mute));
        audio_vm_tap_reset(key);
        uint64_t consumed = vstats(v).pcm_frames;
        CHECK(!dosvm_write(v, 0x20000, words, sizeof(words)));
        dma16(v, 5, 0x20000, 63, false);
        dsp(v, 0x41); dsp(v, 0x5D); dsp(v, 0xC0); dsp(v, 0xB0); dsp(v, 0x10); dsp(v, 63); dsp(v, 0);
        vq_run(8); a_in(v, SB + 0xF); v->monitor.pic[0].irr = 0;
        struct audio_vm_stats st = vstats(v);
        CHECK(st.pcm.peak[0] == cases[i].peak && st.pcm.peak[1] == cases[i].peak && st.pcm_frames - consumed == 64);
        CHECK(st.pcm.frames == (cases[i].mute ? 0u : 128u) && st.gain[0] == cases[i].gain && st.muted == cases[i].mute);
    }
    dsp_reset(v); destroy(v, true); vq_end(); empty();
}

/* OPL3 events at their timeline frame, exactly as a reference chip given the
 * same writes; routing; late events; GAP resynchronisation. */
static void opl_note(struct dosvm *v, struct opl3_chip *ref)
{
    static const uint8_t regs[][2] = { {0x20, 0x21}, {0x23, 0x21}, {0x40, 0x3F}, {0x43, 0x00}, {0x60, 0xF0}, {0x63, 0xF0},
                                       {0x80, 0x0F}, {0x83, 0x0F}, {0xC0, 0x01}, {0xA0, 0x44}, {0xB0, 0x32} };
    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
        opl_w(v, 0x388, 0, regs[i][0], regs[i][1]);
        if (ref) opl3_write(ref, regs[i][0], regs[i][1]);
    }
}
static int32_t opl_ref[8192][2];
static void test_vm_opl(void)
{
    vq_begin();
    struct dosvm *v = audio_vm_create(); struct dosvm_key key = dosvm_key(v); unity(v);
    vq_time(1200); vq_run(2);
    static struct opl3_chip ref; opl3_init(&ref);
    unsigned start = vq_n;
    opl_note(v, &ref); CHECK(audio_vm_playing()); /* queued events take priority before mixing */
    /* one burst: the writes share a timeline frame (or the next) */
    vq_run(48);
    opl3_generate(&ref, &opl_ref[0][0], 4096);
    unsigned r0 = 0; while (r0 < 4096 && !opl_ref[r0][0]) r0++;
    unsigned at = first_sound(start);
    CHECK(r0 < 64 && at + 4000 < vq_n);
    for (unsigned j = 0; j + r0 < 4096 && j < 4000; j++)
        CHECK(vq_cap[at + j][0] == opl_ref[r0 + j][0] && vq_cap[at + j][1] == opl_ref[r0 + j][1]);
    struct audio_vm_stats st = vstats(v);
    CHECK(st.opl_events == 11 && !st.late_events && st.opl.frames > 4000 && st.opl.crossings[0] == st.opl.crossings[1]);
    uint32_t hz10 = (uint32_t)((uint64_t)st.opl.crossings[0] * 480000u / st.opl.frames);
    CHECK(hz10 >= 4350 && hz10 <= 4450);
    opl_w(v, 0x388, 0, 0xB0, 0x12); vq_run(10);  /* key off: release 15 */
    CHECK(first_sound(vq_n - 512) == vq_n);
    CHECK(audio_vm_active() == false && !audio_vm_playing());
    /* OPL3 NEW, output A only. */
    audio_vm_tap_reset(key);
    opl_w(v, 0x388, 1, 0x05, 0x01); opl_w(v, 0x388, 0, 0xC0, 0x11); opl_w(v, 0x388, 0, 0xB0, 0x36);
    vq_run(48);
    st = vstats(v); CHECK(st.opl.frames > 8000 && st.opl.peak[0] > 1000 && !st.opl.peak[1] && !st.opl.sumsq[1]);
    hz10 = (uint32_t)((uint64_t)st.opl.crossings[0] * 480000u / st.opl.frames);
    CHECK(hz10 >= 8750 && hz10 <= 8850 && audio_vm_active());
    opl_w(v, 0x388, 0, 0xB0, 0x16); opl_w(v, 0x388, 0, 0xC0, 0x01); opl_w(v, 0x388, 1, 0x05, 0x00); vq_run(10);
    /* Late events: written, then the output skips ahead of their frames. */
    unsigned late = vstats(v).late_events;
    opl_w(v, 0x388, 0, 0x40, 0x3E); opl_w(v, 0x388, 0, 0x40, 0x3F);
    vq_time(100 * 48); audio.output += 2 * AUDIO_QUANTUM; vq_run(1);
    st = vstats(v); CHECK(st.late_events == late + 2 && st.reanchors == 1);
    /* Overflow: f3-05 queues a GAP, the source reloads the register file. */
    for (unsigned i = 0; i < DOSVM_EVENTS + 8; i++) opl_w(v, 0x388, 0, 0xA1, (uint8_t)i);
    CHECK(ainfo(v).lost_events > 0);
    vq_run(6);                           /* the queue drains as the timeline reaches it */
    opl_w(v, 0x388, 0, 0xA1, 0x44);      /* next write: f3-05 queues GAP before it */
    vq_run(6);
    st = vstats(v); CHECK(st.opl_resyncs == 1 && !st.foreign_events);
    destroy(v, true); vq_end(); empty();
}

/* F4-11: the OPL chip and DSP keep their 48 kHz clock on 44.1 kHz
 * transports. Compare every physical sample to an independently resampled
 * reference chip and keep phase across quanta and the end of a source. */
static void test_vm_physical_rate(void)
{
    vq_begin(); audio.caps.output_rate=44100;
    struct dosvm *v=audio_vm_create(); unity(v);
    vq_time(1200); vq_run(2);
    static struct opl3_chip ref; opl3_init(&ref);
    unsigned start=vq_n; opl_note(v,&ref); vq_run(60);
    memset(opl_ref,0,2*sizeof(opl_ref[0]));
    opl3_generate(&ref,&opl_ref[2][0],8190); /* zero immediately before key-on */
    unsigned at=first_sound(start); CHECK(at+4000<vq_n);
    bool found=false; unsigned ref_start=0, ref_phase=0;
    for (unsigned index=0;!found && index<64;index++) for (unsigned phase=0;!found && phase<44100;phase+=300) {
        bool same=true;
        for (unsigned j=0;j<128;j++) {
            uint64_t num=phase+(uint64_t)j*48000; unsigned p=index+num/44100, frac=num%44100;
            int64_t value=opl_ref[p][0]+((int64_t)opl_ref[p+1][0]-opl_ref[p][0])*frac/44100;
            if (vq_cap[at+j][0]!=value) { same=false; break; }
        }
        if (same) { found=true; ref_start=index; ref_phase=phase; }
    }
    CHECK(found);
    for (unsigned j=0;j<4000;j++) {
        uint64_t num=ref_phase+(uint64_t)j*48000; unsigned p=ref_start+num/44100, frac=num%44100;
        for (unsigned c=0;c<2;c++) {
            int64_t value=opl_ref[p][c]+((int64_t)opl_ref[p+1][c]-opl_ref[p][c])*frac/44100;
            CHECK(vq_cap[at+j][c]==value);
        }
    }
    struct audio_vm_stats st=vstats(v);
    CHECK(st.opl_events==11 && !st.late_events && !st.skips);
    CHECK(st.rendered>62*AUDIO_QUANTUM && st.rendered<(uint64_t)62*AUDIO_QUANTUM*48000/44100+4);
    uint32_t hz10=(uint32_t)((uint64_t)st.opl.crossings[0]*480000/st.opl.frames);
    CHECK(hz10>=4350 && hz10<=4450); /* source clock stays at 48k */
    destroy(v,true); unsigned silent=vq_n; vq_run(1);
    for (unsigned j=silent;j<vq_n;j++) CHECK(!vq_cap[j][0] && !vq_cap[j][1]);
    vq_end(); empty(); audio.caps.output_rate=48000;
    puts("DOS VM 44.1 kHz transport: exact resampled OPL reference across periods, fixed virtual pitch/clock, source teardown without cached replay PASS");
}

static void test_vm_audio(void)
{
    test_vm_attach(); test_vm_modes(); test_vm_double_buffer(); test_vm_gap(); test_vm_stall(); test_vm_pacing(); test_vm_idle_tail(); test_vm_gains(); test_vm_opl(); test_vm_physical_rate();
    audio.ready = false;
    puts("DOS VM audio output: PASS (attach/revoke/release, every PCM layout exact at 2:1, DAC, ADPCM refused, 32 fresh blocks and the 16-bit phase through the mixer, pause gap kept, stalled guest beside native and a second VM, hold/skip/re-anchor without replay, CT1745 and source gains, OPL3 exact against a reference chip, routing, late events, GAP)");
}
