/* Native FM MIDI output, copied queue and refill-worker ownership.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/audio.h>
#include <ciuki/midi_fm.h>
#include <ciuki/opl3.h>
struct audio_midi {
    struct opl3_chip chip;
    struct midi_fm control;
    uint8_t bytes[512];
    unsigned head, count;
};
static void midi_register(void *ctx,unsigned reg,uint8_t value)
{ opl3_write(&((struct audio_midi *)ctx)->chip,reg,value); }
/* Called with audio.service held. The worker owns synthesis; the writer
 * only appends bytes. A failed allocation or rejected batch changes nothing. */
int audio_midi_write(struct audio_stream *s,struct ciuki_midi_data *data)
{
    if (!s || !data || !data->bytes || data->bytes>CIUKI_MIDI_BYTES || data->accepted) return -EINVAL;
    if (s->draining || (!s->midi && s->configured)) return -EBUSY;
    if (audio_output_rate()!=OPL3_RATE) return -EOPNOTSUPP;
    if (!s->midi) {
        struct audio_midi *m=kzalloc(sizeof(*m));
        if (!m) return -ENOMEM;
        opl3_init(&m->chip); midi_fm_reset(&m->control,midi_register,m);
        s->midi=m; s->source=CIUKI_MIXER_MIDI;
    }
    struct audio_midi *m=s->midi;
    unsigned count=sizeof(m->bytes)-m->count;
    if (!count) return -EAGAIN;
    if (count>data->bytes) count=data->bytes;
    for (unsigned i=0;i<count;i++) m->bytes[(m->head+m->count+i)%sizeof(m->bytes)]=data->data[i];
    m->count+=count; data->accepted=count;
    s->configured=s->started=true; audio_wake();
    return 0;
}
void audio_midi_close(struct audio_stream *s)
{
    if (!s || !s->midi) return;
    kfree(s->midi); s->midi=0;
}
bool audio_midi_active(struct audio_stream *s)
{ return s && s->midi && (s->midi->count || !opl3_silent(&s->midi->chip)); }
static int32_t midi_samples[AUDIO_QUANTUM*2];
void audio_midi_mix(struct audio_stream *s,int64_t (*sum)[2],unsigned frames)
{
    struct audio_midi *m=s->midi;
    if (!m || frames>AUDIO_QUANTUM) return;
    unsigned pending=m->count;
    for (unsigned i=0;i<pending;i++) {
        uint8_t byte=m->bytes[m->head]; m->head=(m->head+1)%sizeof(m->bytes); m->count--;
        midi_fm_byte(&m->control,byte,midi_register,m);
    }
    opl3_generate(&m->chip,midi_samples,frames);
    for (unsigned i=0;i<frames;i++) for (unsigned c=0;c<2;c++)
        sum[i][c]+=(midi_samples[i*2+c]*(int32_t)s->gain[c]/100)*
            (int32_t)audio_source_gain(CIUKI_MIXER_MIDI,c)/100;
    s->last_output=audio.output+frames;
}
