/* Integer rational interpolation, sum then master gain then saturation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/audio.h>
struct audio_state audio;
/* f4-05 DOS VM sources (audio/vm_source.c); weak so host harnesses that link
 * only the native mixer keep working. */
extern void audio_vm_mix(int64_t (*sums)[2], unsigned frames) __attribute__((weak));
extern void audio_vm_tap(const struct audio_frame *out, unsigned frames) __attribute__((weak));
static int16_t saturate(int64_t value)
{
    return value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
}
/* S16 delta * rational phase fits unsigned 32 bits: 65535 * 47999.
 * Apply the sign after division to retain C's truncation toward zero. */
static int32_t interpolate(int16_t a, int16_t b, unsigned phase, unsigned rate)
{
    int32_t delta = (int32_t)b - a;
    uint32_t magnitude = (uint32_t)(delta < 0 ? -delta : delta);
    int32_t step = (int32_t)(magnitude * phase / rate);
    return a + (delta < 0 ? -step : step);
}
static int16_t master_sample(int64_t value, unsigned gain)
{
    if (!gain) return 0;
    if (gain == 100) return saturate(value);
    /* Outside these limits every nonzero percentage already saturates.
     * Inside, value * gain fits int32 even with amplified VM sums. */
    if (value > 3276700) return 32767;
    if (value < -3276800) return -32768;
    return saturate((int32_t)value * (int32_t)gain / 100);
}
static void mixer_retire(struct audio_stream *s, unsigned rate)
{
    s->phase-=rate; s->head=(s->head+1)%AUDIO_RING; s->queued--; s->consumed++;
    if (s->draining && !s->drain_latched && s->consumed+s->discarded>=s->drain_goal) {
        s->drain_output=s->last_output; s->drain_latched=true;
    }
}
static int64_t sums[AUDIO_QUANTUM][2];
void audio_mix(struct audio_frame *out, unsigned frames)
{
    if (frames > AUDIO_QUANTUM) frames = AUDIO_QUANTUM;
    unsigned output_rate = audio_output_rate();
    memset(sums, 0, frames * sizeof(sums[0]));
    for (unsigned slot = 0; slot < AUDIO_STREAMS; slot++) {
        struct audio_stream *s = audio.streams[slot];
        if (!s || !s->started) continue;
        bool starved = false;
        for (unsigned i = 0; i < frames; i++) {
            /* Downsampling can owe a second source-frame advance when a
             * short producer batch ran empty. Settle it before interpolation. */
            while (s->phase>=output_rate && s->queued) mixer_retire(s,output_rate);
            bool tail = s->draining && s->consumed + s->discarded + s->queued <= s->drain_goal;
            if (!s->queued || (s->phase && s->queued == 1 && !tail)) {
                if (!s->draining || s->consumed + s->discarded < s->drain_goal) starved = true;
                continue;
            }
            struct audio_frame a = *audio_frame_at(s, s->head);
            int32_t l = a.left, r = a.right;
            if (s->phase) {
                struct audio_frame b = s->queued > 1 ? *audio_frame_at(s, (s->head + 1) % AUDIO_RING) : a;
                l = interpolate(a.left, b.left, s->phase, output_rate);
                r = interpolate(a.right, b.right, s->phase, output_rate);
            }
            sums[i][0] += l * (int32_t)s->gain[0] / 100;
            sums[i][1] += r * (int32_t)s->gain[1] / 100;
            s->last_output = audio.output + i + 1;
            s->phase += s->rate;
            while (s->phase >= output_rate && s->queued) mixer_retire(s,output_rate);
        }
        if (starved) s->starvation++;
        kwait_wake_all(&s->changed);
    }
    if (audio_vm_mix) audio_vm_mix(sums, frames); /* every admitted VM, focused or not */
    bool hardware = audio.master_in_codec || audio.master_in_hardware;
    unsigned left = hardware && audio.master.left ? 100 : audio.master.left;
    unsigned right = hardware && audio.master.right ? 100 : audio.master.right;
    for (unsigned i = 0; i < frames; i++) {
        out[i].left = audio.master.mute ? 0 : master_sample(sums[i][0], left);
        out[i].right = audio.master.mute ? 0 : master_sample(sums[i][1], right);
    }
    if (audio_vm_tap) audio_vm_tap(out, frames);
    audio.output += frames;
}
