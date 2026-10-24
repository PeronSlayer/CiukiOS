/* Bounded owned decoded PCM. No user mapping reaches DMA.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/audio.h>
#include <ciuki/task.h>
/* f1-101: task.c; host audio harnesses without the scheduler see null hooks. */
#pragma weak sched_audio_source_end
#pragma weak audio_midi_close

int audio_stream_open(struct audio_stream **out)
{
    if (!audio.ready) return -ENODEV;
    unsigned slot;
    for (slot = 0; slot < AUDIO_STREAMS && audio.streams[slot]; slot++);
    if (slot == AUDIO_STREAMS) return -ENOMEM;
    struct audio_stream *s = kzalloc(sizeof(*s));
    if (!s) return -ENOMEM;
    for (unsigned i = 0; i < 2; i++) {
        s->pages[i] = pmm_alloc();
        if (!s->pages[i]) {
            if (s->pages[0]) pmm_free(s->pages[0]);
            kfree(s); return -ENOMEM;
        }
        s->ring[i] = P2V(s->pages[i]);
    }
    s->generation = gen_alloc();
    if (!s->generation) { pmm_free(s->pages[0]); pmm_free(s->pages[1]); kfree(s); return -ENOMEM; }
    s->rate = AUDIO_RATE; s->format = CIUKI_AUDIO_S16LE; s->channels = 2;
    s->gain[0] = s->gain[1] = 100;
    kwait_init(&s->changed);
    audio.streams[slot] = s; audio.commitment_pages += 2;
    *out = s;
    return 0;
}
void audio_stream_close(struct audio_stream *s)
{
    if (!s) return;
    if (audio_midi_close) audio_midi_close(s);
    if (sched_audio_source_end) sched_audio_source_end(s); /* f1-101 */
    for (unsigned i = 0; i < AUDIO_STREAMS; i++) if (audio.streams[i] == s) audio.streams[i] = 0;
    audio.commitment_pages -= 2;
    pmm_free(s->pages[0]); pmm_free(s->pages[1]); kfree(s); /* published periods contain copies, never references */
}
int audio_stream_format(struct audio_stream *s, struct ciuki_audio_format *f)
{
    if (f->format != CIUKI_AUDIO_U8 && f->format != CIUKI_AUDIO_S16LE) return -EOPNOTSUPP;
    if (f->channels != 1 && f->channels != 2) return -EOPNOTSUPP;
    if (f->rate < 8000 || f->rate > AUDIO_RATE || f->output_rate) return -EINVAL;
    if (s->configured) return -EBUSY;
    s->rate = f->rate; s->format = f->format; s->channels = f->channels;
    f->output_rate = audio_output_rate();
    return 0;
}
void audio_stream_reset(struct audio_stream *s)
{
    if (s->midi) s->source=CIUKI_MIXER_WAVE;
    if (audio_midi_close) audio_midi_close(s);
    s->discarded += s->queued;
    s->head = s->queued = s->phase = 0;
    s->have_previous = false;
    s->configured = s->started = s->draining = s->drain_latched = false;
    /* f1-101: the producers re-register on their next write or refill read. */
    s->producer_behind = false;
    if (sched_audio_source_end) sched_audio_source_end(s);
    s->generation = gen_alloc();
    if (!s->generation) audio_fail(-EIO);
    kwait_wake_all(&s->changed);
}
static int16_t decode(const uint8_t *p, uint32_t format)
{
    if (format == CIUKI_AUDIO_U8) return (int16_t)(((int)p[0] - 128) * 256);
    uint16_t v = (uint16_t)(p[0] | (uint16_t)p[1] << 8);
    return v < 0x8000 ? (int16_t)v : (int16_t)((int32_t)v - 65536);
}
int audio_stream_enqueue(struct audio_stream *s, const void *buffer, uint32_t bytes)
{
    if (s->midi) return -EBUSY;
    uint32_t sample = s->format == CIUKI_AUDIO_U8 ? 1 : 2;
    uint32_t frame = sample * s->channels;
    if (bytes % frame) return -EINVAL;
    if (!bytes) return 0;
    if (audio.error || !audio.ready) return -EIO;
    uint32_t count = bytes / frame;
    if (count > AUDIO_RING - s->queued) count = AUDIO_RING - s->queued;
    if (!count) return -EAGAIN;
    const uint8_t *p = buffer;
    for (unsigned i = 0; i < count; i++, p += frame) {
        struct audio_frame *at = audio_frame_at(s, (s->head + s->queued + i) % AUDIO_RING);
        at->left = decode(p, s->format);
        at->right = s->channels == 1 ? at->left : decode(p + sample, s->format);
    }
    s->queued += count; s->accepted += count;
    s->configured = s->started = true;
    audio_stream_lead(s);
    audio_wake();
    return (int)(count * frame);
}
