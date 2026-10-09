#include "session_audio.h"
#include "session_devices.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Intel-style posted AC-link writes. A NAM transaction without first
 * acquiring CAS fails; the register changes only after hardware latency. */
struct fake_codec {
    uint16_t regs[64];
    unsigned acquired, busy, pending, reg, value, violation, calls, writes;
    unsigned stuck, absent, rate_ignored;
};
static uint32_t fake_read(void *opaque, uint32_t port, uint32_t width)
{
    struct fake_codec *f = opaque;
    unsigned reg;
    ++f->calls;
    if (port == 0x80) return 0;
    if (port == 0x1834) {
        assert(width == 1);
        if (f->stuck) return 1;
        if (f->busy) {
            if (!--f->busy && f->pending) {
                if (f->reg != 0x2c || (f->regs[0x2a / 2] & 1))
                    f->regs[f->reg / 2] = (uint16_t)f->value;
                else ++f->rate_ignored;
                f->pending = 0;
            }
            return 1;
        }
        if (f->acquired) { ++f->violation; return 1; }
        f->acquired = 1;
        return 0;
    }
    assert(width == 2 && port >= 0x1c00 && port < 0x1c80);
    if (!f->acquired || f->busy) { ++f->violation; return 0xffff; }
    f->acquired = 0;
    reg = port - 0x1c00;
    return f->absent ? 0xffff : f->regs[reg / 2];
}
static void fake_write(void *opaque, uint32_t port, uint32_t value, uint32_t width)
{
    struct fake_codec *f = opaque;
    assert(width == 2 && port >= 0x1c00 && port < 0x1c80);
    if (!f->acquired || f->busy) { ++f->violation; return; }
    f->acquired = 0;
    ++f->writes;
    f->reg = port - 0x1c00; f->value = value;
    f->pending = 1; f->busy = 7;
}
static void fake_init(struct fake_codec *f, int variable)
{
    memset(f, 0, sizeof *f);
    f->regs[0x02 / 2] = 0x1a1a;   /* user's volume 5 */
    f->regs[0x18 / 2] = 0x8808;
    f->regs[0x28 / 2] = (uint16_t)variable;
    f->regs[0x2a / 2] = 0x0200;
    f->regs[0x2c / 2] = 48000;
}
struct signal { unsigned frames; int constant; };
static void generate(void *opaque, int16_t *out, unsigned frames)
{
    struct signal *s = opaque;
    unsigned i;
    for (i = 0; i != frames; ++i) {
        int16_t sample = s->constant ? 32767 :
            (int16_t)(12000.0 * sin(s->frames * (2.0 * 3.14159265358979323846 / 147.0)));
        out[2 * i] = sample;
        out[2 * i + 1] = s->constant ? -32768 : (int16_t)-sample;
        ++s->frames;
    }
}
static int16_t whole[96000 * 2];
static cvaudio_resampler a, b;
static cvaudio_stream stream;
struct partial_signal { struct signal signal; unsigned blocked, calls; };
static unsigned partial_generate(void *opaque, int16_t *out, unsigned frames)
{
    struct partial_signal *p = opaque;
    ++p->calls;
    if (p->blocked) { p->blocked = 0; return 0; }
    if (frames > 63u) frames = 63u;
    generate(&p->signal, out, frames);
    p->blocked = 1;                    /* guest runs between DMA blocks */
    return frames;
}
int main(void)
{
    struct fake_codec f;
    cvaudio_codec c;
    uint32_t rate;
    unsigned i, n, count, crossing;
    int16_t out[CVAUDIO_FRAMES * 2];
    const unsigned rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000};
    struct signal s, t;
    struct partial_signal partial;
    fake_init(&f, 1);
    /* Negative control: the old back-to-back NAM accesses lose writes. */
    fake_write(&f, 0x1c2a, 1, 2);
    assert(f.violation == 1 && f.regs[0x2a / 2] == 0x0200);
    fake_init(&f, 1);
    assert(cvaudio_codec_prepare(&c, 0x1c00, 0x1800, fake_read, fake_write, &f, &rate));
    assert(rate == 44100 && f.regs[0x2c / 2] == 44100 && !f.busy && !f.acquired);
    assert(f.regs[0x02 / 2] == 0x1a1a && f.regs[0x18 / 2] == 0x0808);
    f.regs[0x02 / 2] = 0x8000;   /* user mutes while the game runs */
    assert(cvaudio_codec_restore(&c));
    assert(f.regs[0x2a / 2] == 0x0200 && f.regs[0x2c / 2] == 48000);
    assert(f.regs[0x18 / 2] == 0x8808 && f.regs[0x02 / 2] == 0x8000);
    assert(!f.violation && !f.rate_ignored && !f.busy && !f.acquired);
    fake_init(&f, 0);
    assert(cvaudio_codec_prepare(&c, 0x1c00, 0x1800, fake_read, fake_write, &f, &rate));
    assert(rate == 48000 && cvaudio_codec_restore(&c) && !f.violation);
    fake_init(&f, 1); f.stuck = 1;
    assert(!cvaudio_codec_prepare(&c, 0x1c00, 0x1800, fake_read, fake_write, &f, &rate));
    assert(c.error == CVAUDIO_CODEC_TIMEOUT && rate == 0 && f.calls == 131070 && !f.writes);
    fake_init(&f, 1); f.absent = 1;
    assert(!cvaudio_codec_prepare(&c, 0x1c00, 0x1800, fake_read, fake_write, &f, &rate));
    assert(c.error == CVAUDIO_CODEC_ABSENT && !f.writes);
    fake_init(&f, 0); f.regs[0x2c / 2] = 0;
    assert(!cvaudio_codec_prepare(&c, 0x1c00, 0x1800, fake_read, fake_write, &f, &rate));
    assert(c.error == CVAUDIO_CODEC_RATE && !f.violation);

    for (n = 0; n != sizeof rates / sizeof rates[0]; ++n) {
        rate = rates[n]; count = rate * 2u;
        memset(&s, 0, sizeof s); memset(&t, 0, sizeof t);
        cvaudio_resampler_reset(&a); cvaudio_resampler_reset(&b);
        for (i = 0; i != count;) {
            unsigned frames = count - i;
            if (frames > CVAUDIO_FRAMES) frames = CVAUDIO_FRAMES;
            assert(cvaudio_resample(&a, whole + i * 2, frames, rate, generate, &s));
            i += frames;
        }
        assert(s.frames == CVAUDIO_SOURCE_RATE * 2u + 2u);
        crossing = 0;
        for (i = 0; i != count; ++i) {
            /* Arbitrary descriptor boundaries must not alter the signal. */
            assert(cvaudio_resample(&b, out, 1, rate, generate, &t));
            assert(out[0] == whole[i * 2] && out[1] == whole[i * 2 + 1]);
            assert(out[0] == -out[1]);
            if (i && whole[(i - 1) * 2] <= 0 && out[0] > 0) ++crossing;
        }
        assert(crossing == 600); /* exactly 300 Hz for two seconds */
        assert(s.frames == t.frames && a.phase == b.phase);
        memset(&partial, 0, sizeof partial);
        cvaudio_stream_reset(&stream);
        for (i = 0; i != count;) {
            unsigned requested = count - i, produced, j;
            if (requested > CVAUDIO_FRAMES) requested = CVAUDIO_FRAMES;
            produced = cvaudio_stream_convert(&stream, out, requested, rate,
                                               partial_generate, &partial);
            assert(produced <= requested);
            for (j = 0; j != produced * 2u; ++j) assert(out[j] == whole[i * 2u + j]);
            i += produced;
        }
        assert(partial.calls > count / 63u);
        assert(partial.signal.frames >= 2u + ((count - 1u) * CVAUDIO_SOURCE_RATE) / rate);
        printf("rate %u: 2 seconds, %u source frames, %u positive crossings, boundary invariant\n",
               (unsigned)rate, s.frames, crossing);
    }
    cvaudio_resampler_reset(&a); memset(&s, 0, sizeof s); s.constant = 1;
    assert(cvaudio_resample(&a, out, CVAUDIO_FRAMES, 48000, generate, &s));
    for (i = 0; i != CVAUDIO_FRAMES; ++i)
        assert(out[i * 2] == 32767 && out[i * 2 + 1] == -32768);
    assert(!cvaudio_resample(&a, out, CVAUDIO_FRAMES, 0, generate, &s));
    assert(!cvaudio_resample(&a, out, CVAUDIO_FRAMES + 1, 48000, generate, &s));
    assert(sizeof(cvdev_audio_report) == 128u);
    puts("session audio: PASS (posted codec, rollback, volume, timing and stereo)");
    return 0;
}
