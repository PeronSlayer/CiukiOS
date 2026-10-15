/* f4-05 DOS VM audio sources: each DOS VM's f3-05 SB16 PCM segments, direct
 * DAC events and OPL3 register events, rendered on the VM audio timeline
 * into the f4-01 mixer by the refill worker. Integer only; no blocking, no
 * allocation, no guest memory access: f3-05 copies PCM out under irq_save and
 * keeps the virtual PIC, IF, causes and block lateness. Directive
 * docs/directives/f4-05-vm-audio-output.md; research and decisions
 * docs/design/f4-acceptance.md, section f4-05. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/audio.h>
#include <ciuki/audio_vm.h>
#include <ciuki/dosvm_audio.h>
#include <ciuki/opl3.h>

#define VM_WINDOW 256u                      /* source frames held for interpolation */
#define VM_EVENTS 128u
#define VM_DAC_HOLD (DOSVM_AUDIO_HZ / 10u)  /* a direct-DAC level is held 100 ms */
#define VM_HZ DOSVM_AUDIO_HZ                /* timeline and mixer rate: 48 kHz both */
_Static_assert(DOSVM_AUDIO_HZ == AUDIO_RATE && OPL3_RATE == AUDIO_RATE, "one 48 kHz clock");
/* WAVE_TABLEMUL has S16 waves and U16 attenuation. At most 36 operators,
 * conservatively all doubled as drums, plus S16 PCM and a held DAC level.
 * CT1745's largest gain is < 8; these bounds also cover cancellation. */
#define VM_RAW_BOUND (36u * 32768u * 2u + 65536u)
#define VM_MIX_BOUND (VM_RAW_BOUND * 8u)
_Static_assert(VM_MIX_BOUND <= INT32_MAX / 100u, "VM diagnostic percentage fits int32");
_Static_assert(VM_MIX_BOUND * DOSVM_LIMIT <= INT32_MAX / 2u, "VM resampler delta fits int32");

/* Pending peaks belong to a mixed quantum; only successful publication
 * admits them to the one-second window. Diagnostics only observe it. */
struct vm_level {
    uint32_t pending[3], peak[3], last[3]; /* PCM, FM, combined digital output */
    uint64_t start, at;
};

struct vm_slot {
    /* Control: written by attach/detach/revoke under irq_save. */
    struct dosvm_key key;
    uint32_t token, epoch;
    uint8_t state, gain[2];
    bool muted;
    /* Render state, owned by the refill worker, valid for live_epoch. */
    uint32_t live_epoch;
    bool anchored, busy;
    uint64_t next_t;      /* next VM timeline frame to render */
    uint64_t rendered_to; /* output frame after the last rendered quantum */
    uint32_t hold;        /* output frames of silence before the timeline resumes */
    /* PCM stream: frame k at origin + k * 48000 / rate. */
    bool stream, starving, gap_ahead; /* gap_ahead: the next queued segment does not continue it */
    uint32_t rate, frac;  /* frac: position between idx and idx + 1, in 1/48000 */
    uint32_t advance[2], remainder; /* precomputed step with/without a phase carry */
    uint64_t origin, idx, win_first;
    unsigned win_count;
    bool seg_known;       /* the queue head segment feeds the stream */
    uint64_t seg_start;
    uint32_t seg_sequence, seq_last;
    bool block_open, block_first_set;
    uint32_t block_sequence, block_frames;
    uint16_t block_flags;
    int16_t block_first[2];
    int16_t win[VM_WINDOW][2];
    /* OPL3 and direct DAC */
    struct opl3_chip chip;
    bool opl_used, resync;
    uint64_t resync_clock;
    struct dosvm_audio_event ev[VM_EVENTS];
    unsigned ev_head, ev_count;
    int32_t dac;
    uint64_t dac_until;
    bool head_started;
    uint64_t published;
    struct vm_level level;
    struct audio_vm_stats stats;
};
static struct vm_slot vm_slots[DOSVM_LIMIT];
static struct audio_vm_tap ring;
static struct vm_level ring_level;
static uint32_t rate_epoch;
static volatile bool kick_armed = true;
static int32_t pcm_buf[AUDIO_QUANTUM * 2], opl_buf[AUDIO_QUANTUM * 2];
static uint8_t resync_regs[512];
/* A chip after DBOPL's Setup, built once in thread context (attach) and
 * copied by the worker: a reset costs a copy, not 1,500 register writes. */
static struct opl3_chip chip_template;
static volatile bool template_ready;

/* Ciuki guest level policy: linear n/31, including exact silence at zero.
 * Output gain 41h/42h: 0, 6, 12, 18 dB. Saturation is after the full sum. */
static uint32_t level_gain(unsigned n) { return (n & 31u) * 65536u / 31u; }
static const uint32_t output_gain[4] = { 65536, 130762, 260904, 520571 };

/* One i686 widening multiply of two 32-bit operands, then a fixed shift.
 * Q16 must retain its low bits: reducing the gain precision changes pins. */
static uint32_t gain_product(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * b) >> 16);
}
static uint32_t combined_gain(unsigned master, unsigned level, unsigned out, unsigned source)
{
    uint32_t gain = gain_product(level_gain(master), level_gain(level));
    gain = gain_product(gain, output_gain[out & 3]);
    return gain * source / 100; /* at most 520571 * 100 */
}
static int32_t gained_sample(int32_t sample, uint32_t gain)
{
    return (int32_t)(((int64_t)sample * (int32_t)gain) >> 16);
}
static int32_t pcm_interpolate(int16_t a, int16_t b, unsigned phase)
{
    int32_t delta = (int32_t)b - a;
    uint32_t magnitude = (uint32_t)(delta < 0 ? -delta : delta);
    int32_t step = (int32_t)(magnitude * phase / VM_HZ); /* 65535 * 47999 < 2^32 */
    return a + (delta < 0 ? -step : step);
}
/* Full-width timeline / small divisor, only at stream/quantum boundaries.
 * Base-65536 digits keep each dividend in uint32 for divisors <= 65536
 * (DSP rates are clamped to 45000, timeline/output rates are <= 48000).
 * Unlike casting uptime to uint32, this also works after a clock wrap. */
static uint64_t timeline_div(uint64_t value, uint32_t divisor, uint32_t *remainder)
{
    uint32_t high = (uint32_t)(value >> 32), low = (uint32_t)value;
    uint64_t quotient = (uint64_t)(high / divisor) << 32;
    uint32_t part = ((high % divisor) << 16) | (low >> 16);
    quotient |= (uint64_t)(part / divisor) << 16;
    part = ((part % divisor) << 16) | (low & 65535u);
    quotient |= part / divisor;
    if (remainder) *remainder = part % divisor;
    return quotient;
}
static void pcm_step(struct vm_slot *s, uint32_t rate)
{
    s->rate = rate;
    s->advance[0] = rate / VM_HZ; s->advance[1] = s->advance[0] + 1;
    s->remainder = rate % VM_HZ;
}
/* Delta between four amplified VM sums fits int32, but delta * phase need
 * not. Divide the magnitude first and preserve the exact remainder. */
static int32_t mix_interpolate(int32_t a, int32_t b, unsigned phase, unsigned rate)
{
    int32_t delta = b - a;
    uint32_t magnitude = (uint32_t)(delta < 0 ? -delta : delta);
    int32_t step = (int32_t)((magnitude / rate) * phase + (magnitude % rate) * phase / rate);
    return a + (delta < 0 ? -step : step);
}

static int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static void level_peak(uint32_t *peak, int32_t value)
{
    int32_t v = sat16(value);
    uint32_t magnitude = (uint32_t)(v < 0 ? -v : v);
    if (magnitude > *peak) *peak = magnitude;
}
static void level_publish(struct vm_level *l)
{
    for (unsigned i = 0; i < 3; i++) {
        if (l->pending[i] > l->peak[i]) l->peak[i] = l->pending[i];
        l->pending[i] = 0;
    }
    if (g_ticks - l->start >= 1000) {
        memcpy(l->last, l->peak, sizeof(l->last));
        memset(l->peak, 0, sizeof(l->peak));
        l->at = l->start = g_ticks;
    }
}
static void tap_init(struct audio_vm_tap *t) { memset(t, 0, sizeof(*t)); t->digest = 2166136261u; }
static uint32_t fnv_frame(uint32_t h, int16_t l, int16_t r)
{
    const uint8_t b[4] = { (uint8_t)l, (uint8_t)((uint16_t)l >> 8), (uint8_t)r, (uint8_t)((uint16_t)r >> 8) };
    for (unsigned i = 0; i < 4; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
static void tap_frame(struct audio_vm_tap *t, int32_t left, int32_t right)
{
    int16_t s[2] = { sat16(left), sat16(right) };
    if (!s[0] && !s[1]) return;
    t->frames++;
    t->digest = fnv_frame(t->digest, s[0], s[1]);
    for (unsigned c = 0; c < 2; c++) {
        int32_t v = s[c];
        uint32_t magnitude = (uint32_t)(v < 0 ? -v : v);
        t->sumsq[c] += (uint32_t)(v * v); /* clipped S16 square fits int32 */
        if (magnitude > t->peak[c]) t->peak[c] = magnitude;
        if (t->last[c] < 0 && v >= 0) t->crossings[c]++;
        t->last[c] = v;
    }
}

/* ---- control (thread context) ---- */
int audio_vm_attach(struct dosvm_key key)
{
    rate_epoch++;
    if (!audio.ready || !key.id || key.id > DOSVM_LIMIT) return -ENODEV;
    if (!template_ready) { opl3_init(&chip_template); template_ready = true; }
    uint32_t token = 0;
    int e = dosvm_audio_attach(key, &token);
    if (e) return e;
    struct vm_slot *s = &vm_slots[key.id - 1];
    uint32_t f = irq_save();
    bool first = !audio_vm_attached();
    s->key = key; s->token = token;
    if (!++s->epoch) s->epoch = 1;
    s->state = AUDIO_VM_PENDING; s->gain[0] = s->gain[1] = 100; s->muted = false;
    s->published = 0;
    memset(&s->level, 0, sizeof(s->level)); s->level.start = g_ticks;
    if (first) { memset(&ring_level, 0, sizeof(ring_level)); ring_level.start = g_ticks; }
    memset(&s->stats, 0, sizeof(s->stats));
    tap_init(&s->stats.pcm); tap_init(&s->stats.opl);
    s->stats.opl_head_digest = 2166136261u;
    irq_restore(f);
    audio_wake();
    return 0;
}
void audio_vm_detach(struct dosvm_key key)
{
    rate_epoch++;
    if (!key.id || key.id > DOSVM_LIMIT) return;
    struct vm_slot *s = &vm_slots[key.id - 1];
    uint32_t f = irq_save();
    if (s->state != AUDIO_VM_FREE && s->key.generation == key.generation) {
        s->state = AUDIO_VM_FREE; s->token = 0;
        if (!++s->epoch) s->epoch = 1;
    }
    irq_restore(f);
}
void audio_vm_kick(void)
{
    if (kick_armed) { kick_armed = false; audio_wake(); }
}
int audio_vm_set_gain(struct dosvm_key key, unsigned left, unsigned right, bool mute)
{
    if (!key.id || key.id > DOSVM_LIMIT || left > 100 || right > 100) return -EINVAL;
    struct vm_slot *s = &vm_slots[key.id - 1];
    int e = -EINVAL;
    uint32_t f = irq_save();
    if (s->state != AUDIO_VM_FREE && s->key.generation == key.generation) {
        s->gain[0] = (uint8_t)left; s->gain[1] = (uint8_t)right; s->muted = mute; e = 0;
    }
    irq_restore(f);
    return e;
}
bool audio_vm_stats(struct dosvm_key key, struct audio_vm_stats *out)
{
    if (!out || !key.id || key.id > DOSVM_LIMIT) return false;
    struct vm_slot *s = &vm_slots[key.id - 1];
    uint32_t f = irq_save();
    bool ok = s->key.generation == key.generation && s->epoch;
    if (ok) {
        *out = s->stats;
        out->epoch = s->epoch; out->state = s->state;
        out->muted = s->muted; out->gain[0] = s->gain[0]; out->gain[1] = s->gain[1];
    }
    irq_restore(f);
    return ok;
}
void audio_vm_tap_reset(struct dosvm_key key)
{
    if (!key.id || key.id > DOSVM_LIMIT) return;
    struct vm_slot *s = &vm_slots[key.id - 1];
    uint32_t f = irq_save();
    if (s->key.generation == key.generation) {
        tap_init(&s->stats.pcm); tap_init(&s->stats.opl);
        s->stats.opl_head_digest = 2166136261u; s->stats.opl_head_frames = 0; s->head_started = false;
        s->stats.log_count = 0;
    }
    tap_init(&ring);
    irq_restore(f);
}
void audio_vm_ring(struct audio_vm_tap *out)
{
    uint32_t f = irq_save();
    *out = ring;
    irq_restore(f);
}

/* ---- render (refill worker) ---- */
static void revoke(struct vm_slot *s, uint32_t epoch)
{
    uint32_t f = irq_save();
    if (s->epoch == epoch && s->state != AUDIO_VM_FREE) {
        s->state = AUDIO_VM_FREE; s->token = 0; s->stats.revoked++;
        if (!++s->epoch) s->epoch = 1;
    }
    irq_restore(f);
    s->stream = false; s->busy = false;
}
static void slot_reset(struct vm_slot *s, uint32_t epoch)
{
    s->live_epoch = epoch;
    s->anchored = s->busy = s->stream = s->starving = s->gap_ahead = s->seg_known = false;
    s->block_open = s->block_first_set = s->opl_used = s->resync = s->head_started = false;
    s->next_t = s->rendered_to = 0; s->hold = 0;
    s->rate = s->frac = 0; s->origin = s->idx = s->win_first = 0; s->win_count = 0;
    s->seq_last = 0; s->ev_head = s->ev_count = 0; s->dac = 0; s->dac_until = 0;
    s->chip = chip_template;
}
/* Both calls fail with -EINVAL once the token is revoked (VM stopped,
 * another consumer attached): the source then ends. */
static int peek(struct vm_slot *s, uint32_t token, struct dosvm_audio_block *b)
{
    return dosvm_audio_fetch(s->key, token, b, 0, 0);
}
static void block_begin(struct vm_slot *s, const struct dosvm_audio_block *b)
{
    if (s->block_open && s->block_sequence == b->sequence) return;
    if (s->seq_last && b->sequence != s->seq_last + 1) s->stats.seq_gaps++;
    s->block_open = true; s->block_sequence = b->sequence;
    s->block_frames = 0; s->block_flags = 0; s->block_first_set = false;
}
static void adopt(struct vm_slot *s, const struct dosvm_audio_block *b)
{
    s->seg_known = true; s->seg_start = b->start; s->seg_sequence = b->sequence;
    s->stats.segments++;
    block_begin(s, b);
}
static void note_frames(struct vm_slot *s, const int16_t *frames, unsigned n)
{
    if (n && !s->block_first_set) {
        s->block_first[0] = frames[0]; s->block_first[1] = frames[1]; s->block_first_set = true;
    }
    s->block_frames += n;
}
static void segment_done(struct vm_slot *s, const struct dosvm_audio_block *b)
{
    s->seg_known = false;
    s->block_flags |= b->flags;
    if (b->flags & DOSVM_BLOCK_LATE) s->stats.late_blocks++;
    if (b->flags & DOSVM_BLOCK_STALE) s->stats.stale_blocks++;
    if (b->flags & DOSVM_BLOCK_OVERRUN) s->stats.overrun_blocks++;
    if (b->flags & DOSVM_BLOCK_UNSUPPORTED) s->stats.unsupported_blocks++;
    if (!(b->flags & DOSVM_BLOCK_COMPLETE) || !s->block_open) return;
    struct audio_vm_block_log *log = &s->stats.log[s->stats.log_count % AUDIO_VM_LOG];
    *log = (struct audio_vm_block_log){ .sequence = s->block_sequence, .frames = s->block_frames,
        .first = { s->block_first[0], s->block_first[1] }, .flags = s->block_flags };
    s->stats.log_count++;
    s->stats.blocks++;
    s->seq_last = s->block_sequence;
    s->block_open = false;
}
static bool same_segment(const struct vm_slot *s, const struct dosvm_audio_block *b)
{
    return s->seg_known && b->start == s->seg_start && b->sequence == s->seg_sequence;
}
/* A segment continues the stream when it has the same rate and starts within
 * two timeline frames of the stream's next frame position. */
static bool contiguous(const struct vm_slot *s, const struct dosvm_audio_block *b)
{
    if (b->rate != s->rate || b->offset) return false;
    uint64_t total = s->win_first + s->win_count;
    uint64_t expect = s->origin * s->rate + total * VM_HZ, actual = b->start * s->rate;
    uint64_t diff = expect > actual ? expect - actual : actual - expect;
    return diff <= 2ull * s->rate;
}
/* Appends source frames to the window, never past a segment that does not
 * continue the stream; drops the frames before idx first. */
static void fill(struct vm_slot *s, uint32_t token)
{
    if (s->idx > s->win_first) {
        uint64_t drop = s->idx - s->win_first;
        if (drop > s->win_count) drop = s->win_count;
        memmove(s->win, s->win + drop, (s->win_count - (unsigned)drop) * sizeof(s->win[0]));
        s->win_count -= (unsigned)drop; s->win_first += drop;
    }
    for (unsigned guard = 0; guard < 16 && s->win_count < VM_WINDOW; guard++) {
        struct dosvm_audio_block b;
        if (!s->seg_known) {
            int got = peek(s, token, &b);
            if (got < 0) { if (got != -EAGAIN) revoke(s, s->live_epoch); return; }
            if (b.closed && b.offset >= b.frames) { /* an empty segment retired by the peek */
                if (b.frames == 0) { block_begin(s, &b); s->stats.segments++; segment_done(s, &b); }
                continue;
            }
            if (!contiguous(s, &b)) { s->gap_ahead = true; return; }
            adopt(s, &b);
        }
        int got = dosvm_audio_fetch(s->key, token, &b, &s->win[s->win_count][0], VM_WINDOW - s->win_count);
        if (got < 0) { if (got != -EAGAIN) revoke(s, s->live_epoch); else s->seg_known = false; return; }
        if (!same_segment(s, &b)) { s->seg_known = false; return; } /* not ours: never mix it in */
        note_frames(s, &s->win[s->win_count][0], (unsigned)got);
        s->win_count += (unsigned)got;
        s->stats.pcm_frames += (unsigned)got;
        if (b.closed) segment_done(s, &b);
        else if (!got) return; /* open segment without new frames yet */
    }
}
enum { START_NONE, START_FUTURE, START_DATA };
/* Starts a stream at timeline frame t from the queue head; frames whose
 * position has passed are fetched and dropped (late). */
static int stream_start(struct vm_slot *s, uint32_t token, uint64_t t, uint64_t *future)
{
    for (unsigned guard = 0; guard < 64; guard++) { /* late or empty segments are retired on the way */
        struct dosvm_audio_block b;
        int got = peek(s, token, &b);
        if (got < 0) { if (got != -EAGAIN) revoke(s, s->live_epoch); return START_NONE; }
        if (b.closed && b.offset >= b.frames) {
            if (same_segment(s, &b) || b.frames == 0) {
                if (!same_segment(s, &b)) { block_begin(s, &b); s->stats.segments++; }
                segment_done(s, &b);
            }
            continue;
        }
        uint32_t rate = b.rate ? b.rate : 1;
        if (b.start * rate + (uint64_t)b.offset * VM_HZ > t * rate) {
            *future = b.start + timeline_div((uint64_t)b.offset * VM_HZ, rate, 0);
            return START_FUTURE; /* the head's next frame is still ahead: a gap */
        }
        uint64_t num = (t - b.start) * rate;
        uint32_t frac;
        uint64_t p = timeline_div(num, VM_HZ, &frac);
        if (p < b.offset) { p = b.offset; frac = 0; }
        if (!same_segment(s, &b)) adopt(s, &b);
        s->origin = b.start; pcm_step(s, rate);
        s->win_first = b.offset; s->win_count = 0;
        /* Drop the late frames b.offset .. p-1. */
        bool closed = false;
        while (s->win_first < p) {
            unsigned want = p - s->win_first > VM_WINDOW ? VM_WINDOW : (unsigned)(p - s->win_first);
            struct dosvm_audio_block d;
            int n = dosvm_audio_fetch(s->key, token, &d, &s->win[0][0], want);
            if (n < 0 || !same_segment(s, &d)) { if (n < 0 && n != -EAGAIN) revoke(s, s->live_epoch); return START_NONE; }
            note_frames(s, &s->win[0][0], (unsigned)n);
            s->block_flags |= AUDIO_VM_BLOCK_DROPPED;
            s->win_first += (unsigned)n; s->stats.late_frames += (unsigned)n;
            if (d.closed) { segment_done(s, &d); closed = true; break; }
            if (!n) break;
        }
        if (closed) continue;                  /* the whole segment was late: try the next one */
        if (s->win_first < p) return START_NONE; /* not produced yet: retry on a later quantum */
        s->idx = p; s->frac = frac;
        fill(s, token);
        if (s->idx >= s->win_first + s->win_count) return START_NONE; /* no frame at p yet */
        s->stream = true; s->starving = false; s->gap_ahead = false;
        s->stats.streams++;
        return START_DATA;
    }
    return START_NONE;
}
static void pcm_render(struct vm_slot *s, uint32_t token, const struct dosvm_audio_info *info, int32_t *out, unsigned n)
{
    uint64_t wait = 0; /* no stream can start before this timeline frame in this call */
    bool dsp = info->active && !info->paused;
    for (unsigned k = 0; k < n; k++) {
        uint64_t t = s->next_t + k;
        int32_t left = 0, right = 0;
        if (!s->stream && t >= wait) {
            uint64_t future = t + n;
            int r = stream_start(s, token, t, &future);
            if (r != START_DATA) wait = r == START_FUTURE ? future : s->next_t + n;
        }
        if (s->stream) {
            if (s->idx + 1 >= s->win_first + s->win_count) fill(s, token);
            if (s->idx >= s->win_first + s->win_count) {
                /* Data ran out: inside an open segment, or with the DSP still
                 * playing and no gap queued (pause, stall, new transfer), the
                 * guest is late and the source starves. */
                s->stream = false;
                s->starving = s->seg_known || (dsp && !s->gap_ahead);
                wait = 0;
            } else {
                const int16_t *a = s->win[s->idx - s->win_first];
                const int16_t *b = s->idx + 1 < s->win_first + s->win_count ? s->win[s->idx + 1 - s->win_first] : a;
                left = pcm_interpolate(a[0], b[0], s->frac);
                right = pcm_interpolate(a[1], b[1], s->frac);
                s->frac += s->remainder;
                bool carry = s->frac >= VM_HZ;
                if (carry) s->frac -= VM_HZ;
                s->idx += s->advance[carry];
            }
        }
        if (!s->stream && s->starving) s->stats.starved++;
        if (t < s->dac_until) { left += s->dac; right += s->dac; }
        out[k * 2] = (int32_t)left; out[k * 2 + 1] = (int32_t)right;
    }
}
static void opl_resync(struct vm_slot *s)
{
    uint64_t clock = 0;
    if (!dosvm_audio_opl_registers(s->key, resync_regs, &clock)) return;
    const uint8_t *r = resync_regs;
    opl3_write(&s->chip, 0x105, r[0x105]);
    opl3_write(&s->chip, 0x104, r[0x104]);
    opl3_write(&s->chip, 0x01, r[0x01]);
    opl3_write(&s->chip, 0x08, r[0x08]);
    for (unsigned bank = 0; bank < 0x200; bank += 0x100) {
        static const uint8_t ranges[][2] = { {0x20, 0x35}, {0x40, 0x55}, {0x60, 0x75}, {0x80, 0x95},
                                             {0xE0, 0xF5}, {0xA0, 0xA8}, {0xC0, 0xC8} };
        for (unsigned i = 0; i < ARRAY_SIZE(ranges); i++)
            for (unsigned reg = ranges[i][0]; reg <= ranges[i][1]; reg++) opl3_write(&s->chip, bank + reg, r[bank + reg]);
    }
    opl3_write(&s->chip, 0xBD, r[0xBD]);
    for (unsigned bank = 0; bank < 0x200; bank += 0x100)
        for (unsigned reg = 0xB0; reg <= 0xB8; reg++) opl3_write(&s->chip, bank + reg, r[bank + reg]);
    s->opl_used = true; s->resync = true; s->resync_clock = clock;
    s->stats.opl_resyncs++;
}
static void apply_event(struct vm_slot *s, const struct dosvm_audio_event *e)
{
    if (e->generation != s->key.generation) { s->stats.foreign_events++; return; }
    if (e->kind == DOSVM_EVENT_GAP) { opl_resync(s); return; }
    if (s->resync && e->clock <= s->resync_clock) return; /* already in the register snapshot */
    if (e->clock < s->next_t) s->stats.late_events++;
    if (e->kind == DOSVM_EVENT_OPL) {
        opl3_write(&s->chip, e->reg, e->value);
        s->opl_used = true; s->stats.opl_events++;
    } else if (e->kind == DOSVM_EVENT_DAC) {
        s->dac = ((int32_t)e->value - 128) * 256;
        s->dac_until = (e->clock > s->next_t ? e->clock : s->next_t) + VM_DAC_HOLD;
        s->stats.dac_events++;
    }
}
/* Applies every event due at or before next_t (in write order) and returns
 * the timeline frame of the next pending one. */
static uint64_t events_due(struct vm_slot *s, uint32_t token)
{
    for (unsigned guard = 0; guard < 4096; guard++) {
        if (!s->ev_count) {
            int n = dosvm_audio_events(s->key, token, s->ev, VM_EVENTS);
            if (n < 0) { revoke(s, s->live_epoch); return UINT64_MAX; }
            if (!n) return UINT64_MAX;
            s->ev_head = 0; s->ev_count = (unsigned)n;
        }
        const struct dosvm_audio_event *e = &s->ev[s->ev_head];
        if (e->clock > s->next_t) return e->clock;
        s->ev_head++; s->ev_count--;
        apply_event(s, e);
    }
    return s->next_t + 1;
}
/* Skips the timeline forward: events apply without synthesis, PCM before
 * the new position is dropped by the next stream start. */
static void skip_to(struct vm_slot *s, uint32_t token, uint64_t t)
{
    while (s->next_t < t) {
        uint64_t next = events_due(s, token);
        if (s->state == AUDIO_VM_FREE) return;
        s->next_t = next < t ? next : t;
    }
    s->stream = false; s->starving = false;
}
static bool mix_samples(struct vm_slot *s, const struct dosvm_audio_info *info, int64_t (*sums)[2], unsigned frames)
{
    uint32_t gp[2], go[2];
    unsigned master[2];
    for (unsigned c = 0; c < 2; c++) {
        unsigned source = s->muted ? 0 : s->gain[c];
        gp[c] = combined_gain(info->master[c], info->voice[c], info->output_gain[c], source);
        go[c] = combined_gain(info->master[c], info->fm[c], info->output_gain[c], source);
        master[c] = c ? audio.master.right : audio.master.left;
        if ((audio.master_in_codec || audio.master_in_hardware) && master[c]) master[c] = 100;
        if (audio.master.mute) master[c] = 0;
    }
    bool sound = false;
    for (unsigned k = 0; k < frames; k++) {
        int32_t p[2], o[2];
        for (unsigned c = 0; c < 2; c++) {
            p[c] = gained_sample(pcm_buf[k * 2 + c], gp[c]);
            o[c] = gained_sample(opl_buf[k * 2 + c], go[c]);
            int32_t combined = p[c] + o[c];
            sums[k][c] += combined;
            level_peak(&s->level.pending[0], p[c]);
            level_peak(&s->level.pending[1], o[c]);
            /* Conservative DBOPL bound: 36 operators * 32768 * 2 (drums),
             * plus S16 PCM and DAC, amplified < 8: combined * 100 < 2^31.
             * Percentage truncation is toward zero, as before. */
            level_peak(&s->level.pending[2], combined * (int32_t)master[c] / 100);
        }
        if (p[0] || p[1] || o[0] || o[1]) sound = true;
        tap_frame(&s->stats.pcm, p[0], p[1]);
        tap_frame(&s->stats.opl, o[0], o[1]);
        if (!s->head_started && (o[0] || o[1])) s->head_started = true;
        if (s->head_started && s->stats.opl_head_frames < AUDIO_VM_HEAD) {
            s->stats.opl_head_digest = fnv_frame(s->stats.opl_head_digest, sat16(o[0]), sat16(o[1]));
            s->stats.opl_head_frames++;
        }
    }
    return sound;
}
static void render(struct vm_slot *s, uint32_t token, uint32_t epoch, int64_t (*sums)[2], unsigned frames, uint64_t output, uint64_t played)
{
    struct dosvm_audio_info info;
    if (!dosvm_audio_info(s->key, &info)) { revoke(s, epoch); return; }
    uint64_t now = info.clock;
    uint64_t queued = output > played ? output - played : 0;
    if (queued > (AUDIO_LEAD + 1) * AUDIO_QUANTUM) queued = (AUDIO_LEAD + 1) * AUDIO_QUANTUM;
    uint64_t target = now + queued > AUDIO_VM_LATENCY ? now + queued - AUDIO_VM_LATENCY : 0;
    if (!s->anchored || s->rendered_to != output) {
        /* First quantum, or the output was not rendered for this source
         * (transport restarted): re-anchor, never moving back in time. */
        if (s->anchored) s->stats.reanchors++;
        if (!s->anchored || target > s->next_t) s->next_t = target;
        s->anchored = true; s->hold = 0;
    }
    int64_t lead = (int64_t)(now + queued) - (int64_t)s->next_t;
    if (lead > (int64_t)(AUDIO_VM_LATENCY + AUDIO_VM_SKIP_BAND)) {
        uint64_t skip = (uint64_t)lead - AUDIO_VM_LATENCY;
        skip_to(s, token, s->next_t + skip);
        if (s->state == AUDIO_VM_FREE) return;
        s->stats.skips++; s->stats.skipped_frames += skip;
        lead = AUDIO_VM_LATENCY;
    }
    int64_t lag = (int64_t)now - (int64_t)s->next_t;
    if (!s->hold && (lag < (int64_t)AUDIO_VM_FLOOR || lead < (int64_t)(AUDIO_VM_LATENCY - AUDIO_VM_HOLD_BAND))) {
        int64_t need = (int64_t)AUDIO_VM_LATENCY - lead, floor = (int64_t)AUDIO_VM_FLOOR - lag;
        if (floor > need) need = floor;
        if (need > 0) { s->hold = (uint32_t)need; s->stats.holds++; }
    }
    if (!s->stats.rendered || lead < s->stats.lead_min) s->stats.lead_min = (int32_t)lead;
    if (!s->stats.rendered || lead > s->stats.lead_max) s->stats.lead_max = (int32_t)lead;
    if (!s->stats.rendered || lag < s->stats.lag_min) s->stats.lag_min = (int32_t)lag;
    s->stats.lead = (int32_t)lead; s->stats.lag = (int32_t)lag;

    memset(pcm_buf, 0, frames * 2 * sizeof(pcm_buf[0]));
    memset(opl_buf, 0, frames * 2 * sizeof(opl_buf[0]));
    unsigned i = 0;
    while (i < frames && s->state != AUDIO_VM_FREE) {
        if (s->hold) {
            /* Output silence without consuming timeline; OPL voices keep sounding. */
            unsigned n = s->hold < frames - i ? s->hold : frames - i;
            if (s->opl_used) opl3_generate(&s->chip, &opl_buf[i * 2], n);
            s->hold -= n; s->stats.hold_frames += n; i += n;
            continue;
        }
        uint64_t next = events_due(s, token);
        unsigned n = frames - i;
        if (next - s->next_t < n) n = (unsigned)(next - s->next_t);
        if (s->opl_used) opl3_generate(&s->chip, &opl_buf[i * 2], n);
        pcm_render(s, token, &info, &pcm_buf[i * 2], n);
        s->next_t += n; i += n;
    }
    if (s->state == AUDIO_VM_FREE) return;
    s->rendered_to = output + frames;
    s->stats.rendered += frames;

    bool sound = mix_samples(s, &info, sums, frames);
    s->busy = sound || s->stream || s->hold || s->ev_count || s->dac_until > s->next_t ||
              (s->opl_used && !opl3_silent(&s->chip)) || info.active || info.blocks_queued || info.events_queued;
}
static void vm_mix_48(int64_t (*sums)[2], unsigned frames, uint64_t output, uint64_t played)
{
    if (frames > AUDIO_QUANTUM) frames = AUDIO_QUANTUM;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct vm_slot *s = &vm_slots[i];
        if (!template_ready) return; /* no source was attached yet */
        uint32_t f = irq_save();
        uint8_t state = s->state;
        uint32_t epoch = s->epoch, token = s->token;
        if (state == AUDIO_VM_PENDING) s->state = AUDIO_VM_ACTIVE;
        irq_restore(f);
        if (state == AUDIO_VM_FREE) { s->live_epoch = 0; s->busy = false; continue; }
        if (s->live_epoch != epoch) slot_reset(s, epoch);
        render(s, token, epoch, sums, frames, output, played);
    }
}
/* The VM DSP/events and integer OPL chip keep their 48 kHz timebase on a
 * 44.1 kHz physical backend. Convert the accumulated VM source with a
 * persistent rational phase and two cached lookahead frames. Native streams
 * already convert directly to the effective output rate in mixer.c. */
#define VM_RATE_WINDOW (AUDIO_QUANTUM * 48000u / 44100u + 4u)
static int64_t vm_rate_samples[VM_RATE_WINDOW][2];
static int32_t vm_rate_cache[2][2];
static uint64_t vm_rate_next;
static unsigned vm_rate_cached;
static uint32_t vm_rate_epoch;
void audio_vm_mix(int64_t (*sums)[2], unsigned frames)
{
    if (frames > AUDIO_QUANTUM) frames=AUDIO_QUANTUM;
    unsigned rate=audio_output_rate();
    if (rate==VM_HZ) { vm_rate_cached=0; vm_mix_48(sums,frames,audio.output,audio.played_output); return; }
    /* Only the directed 44.1 kHz and 48 kHz physical rates are admitted. */
    if (rate!=44100 || !frames) return;
    uint32_t phase;
    uint64_t first=timeline_div(audio.output*VM_HZ,rate,&phase);
    unsigned count=(phase+(frames-1)*VM_HZ)/rate+2;
    uint64_t played=timeline_div(audio.played_output*VM_HZ,rate,0);
    if (vm_rate_next!=audio.output || vm_rate_epoch!=rate_epoch) vm_rate_cached=0;
    memset(vm_rate_samples,0,sizeof(vm_rate_samples));
    for (unsigned i=0;i<vm_rate_cached;i++) for (unsigned c=0;c<2;c++) vm_rate_samples[i][c]=vm_rate_cache[i][c];
    for (unsigned i=vm_rate_cached;i<count;) {
        unsigned take=count-i; if (take>AUDIO_QUANTUM) take=AUDIO_QUANTUM;
        vm_mix_48(vm_rate_samples+i,take,first+i,played); i+=take;
    }
    unsigned index=0;
    for (unsigned i=0;i<frames;i++) {
        for (unsigned c=0;c<2;c++) {
            int32_t a=(int32_t)vm_rate_samples[index][c], b=(int32_t)vm_rate_samples[index+1][c];
            sums[i][c]+=mix_interpolate(a,b,phase,rate);
        }
        phase+=VM_HZ; index+=phase/rate; phase%=rate;
    }
    vm_rate_cached=count-index;
    for (unsigned i=0;i<vm_rate_cached;i++) for (unsigned c=0;c<2;c++) vm_rate_cache[i][c]=(int32_t)vm_rate_samples[index+i][c];
    vm_rate_next=audio.output+frames; vm_rate_epoch=rate_epoch;
}
void audio_vm_tap(const struct audio_frame *out, unsigned frames)
{
    bool attached = false;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (vm_slots[i].state != AUDIO_VM_FREE) attached = true;
    if (!attached) return;
    for (unsigned k = 0; k < frames;) {
        unsigned end = frames - k > 16 ? k + 16 : frames;
        uint32_t f = irq_save();
        for (; k < end; k++) {
            tap_frame(&ring, out[k].left, out[k].right);
            level_peak(&ring_level.pending[0], out[k].left);
            level_peak(&ring_level.pending[0], out[k].right);
        }
        irq_restore(f);
    }
}
bool audio_vm_attached(void)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) if (vm_slots[i].state != AUDIO_VM_FREE) return true;
    return false;
}
void audio_vm_published(void)
{
    uint32_t f = irq_save();
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct vm_slot *s = &vm_slots[i];
        if (s->state == AUDIO_VM_ACTIVE && s->live_epoch == s->epoch) {
            s->published++; level_publish(&s->level);
        }
    }
    level_publish(&ring_level);
    irq_restore(f);
}
void audio_vm_diagnostic(struct dosvm_key key, struct audio_vm_diagnostic *out)
{
    uint32_t f = irq_save();
    *out = (struct audio_vm_diagnostic){ .rate=audio_output_rate(), .ready=audio.ready,
        .error=audio.error, .fifo=audio.fifo, .halts=audio.halts, .running=audio.running,
        .master={audio.master.left, audio.master.right}, .master_muted=audio.master.mute,
        .master_in_codec=audio.master_in_codec };
    if (key.id && key.id <= DOSVM_LIMIT) {
        const struct vm_slot *s = &vm_slots[key.id - 1];
        if (s->key.generation == key.generation && s->state != AUDIO_VM_FREE) {
            out->attached = true; out->published = s->published;
            out->pcm = s->stats.pcm.frames; out->opl = s->stats.opl.frames; out->starved = s->stats.starved;
            out->muted = s->muted; out->gain[0] = s->gain[0]; out->gain[1] = s->gain[1];
            out->level_at = s->level.at;
            out->pcm_peak = s->level.last[0]; out->opl_peak = s->level.last[1]; out->mix_peak = s->level.last[2];
            out->ring_peak = ring_level.last[0];
        }
    }
    irq_restore(f);
}
/* Public diagnostics must only observe; do not revoke tokens or arm wakeups. */
bool audio_vm_playing(void)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct vm_slot *s = &vm_slots[i];
        if (s->state == AUDIO_VM_FREE) continue;
        if (s->busy && s->live_epoch == s->epoch) return true;
        struct dosvm_audio_info info;
        if (dosvm_audio_info(s->key, &info) && (info.active || info.pcm_queued || info.blocks_queued || info.events_queued))
            return true;
    }
    return false;
}
bool audio_vm_active(void)
{
    bool any = false;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct vm_slot *s = &vm_slots[i];
        if (s->state == AUDIO_VM_FREE) continue;
        if (s->busy && s->live_epoch == s->epoch) { any = true; continue; }
        /* A revoked token (another consumer, such as the f3 probe, attached)
         * ends the source here: its queue is not ours to play. */
        if (dosvm_audio_events(s->key, s->token, 0, 0) < 0) { revoke(s, s->epoch); continue; }
        struct dosvm_audio_info info;
        if (dosvm_audio_info(s->key, &info) && (info.active || info.pcm_queued || info.blocks_queued || info.events_queued))
            any = true;
    }
    kick_armed = !any; /* an idle worker is woken once by the next VM tick with queued audio */
    return any;
}
