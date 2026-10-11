/* Integer rational interpolation, sum then master gain then saturation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/audio.h>
struct audio_state audio;
static int16_t saturate(int64_t value)
{
    return value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
}
static int64_t sums[AUDIO_QUANTUM][2];
void audio_mix(struct audio_frame *out, unsigned frames)
{
    if (frames > AUDIO_QUANTUM) frames = AUDIO_QUANTUM;
    memset(sums, 0, sizeof(sums));
    for (unsigned slot = 0; slot < AUDIO_STREAMS; slot++) {
        struct audio_stream *s = audio.streams[slot];
        if (!s || !s->started) continue;
        bool starved = false;
        for (unsigned i = 0; i < frames; i++) {
            bool tail = s->draining && s->consumed + s->discarded + s->queued <= s->drain_goal;
            if (!s->queued || (s->phase && s->queued == 1 && !tail)) {
                if (!s->draining || s->consumed + s->discarded < s->drain_goal) starved = true;
                continue;
            }
            struct audio_frame a = *audio_frame_at(s, s->head);
            struct audio_frame b = s->queued > 1 ? *audio_frame_at(s, (s->head + 1) % AUDIO_RING) : a;
            int32_t l = a.left + (int64_t)((int32_t)b.left - a.left) * s->phase / AUDIO_RATE;
            int32_t r = a.right + (int64_t)((int32_t)b.right - a.right) * s->phase / AUDIO_RATE;
            sums[i][0] += (int64_t)l * s->gain[0] / 100;
            sums[i][1] += (int64_t)r * s->gain[1] / 100;
            s->last_output = audio.output + i + 1;
            s->phase += s->rate;
            if (s->phase >= AUDIO_RATE) {
                s->phase -= AUDIO_RATE;
                s->head = (s->head + 1) % AUDIO_RING; s->queued--; s->consumed++;
                if (s->draining && !s->drain_latched && s->consumed + s->discarded >= s->drain_goal) {
                    s->drain_output = s->last_output; s->drain_latched = true;
                }
            }
        }
        if (starved) s->starvation++;
        kwait_wake_all(&s->changed);
    }
    for (unsigned i = 0; i < frames; i++) {
        out[i].left = audio.master.mute ? 0 : saturate(sums[i][0] * audio.master.left / 100);
        out[i].right = audio.master.mute ? 0 : saturate(sums[i][1] * audio.master.right / 100);
    }
    audio.output += frames;
}
