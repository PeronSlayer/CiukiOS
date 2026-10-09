#include "session_audio.h"

/* Reading CAS acquires the semaphore when bit 0 was clear. Do not do an
 * extra completion-only CAS read without a following NAM transaction. */
static int codec_wait(cvaudio_codec *c, unsigned reg)
{
    uint32_t guard;
    c->last_port = c->nam + reg;
    for (guard = 0; guard != 65535u; ++guard) {
        if (!(c->read(c->opaque, c->nabm + 0x34u, 1) & 1u)) return 1;
        (void)c->read(c->opaque, 0x80u, 1);
    }
    c->error = CVAUDIO_CODEC_TIMEOUT;
    return 0;
}
static int codec_read(cvaudio_codec *c, unsigned reg, uint16_t *value)
{
    if (!codec_wait(c, reg)) return 0;
    *value = (uint16_t)c->read(c->opaque, c->nam + reg, 2);
    if (*value != 0xffffu) return 1;
    c->error = CVAUDIO_CODEC_ABSENT;
    return 0;
}
static int codec_write(cvaudio_codec *c, unsigned reg, uint16_t value)
{
    if (!codec_wait(c, reg)) return 0;
    c->write(c->opaque, c->nam + reg, value, 2);
    return 1;
}

int cvaudio_codec_restore(cvaudio_codec *c)
{
    uint16_t completion;
    int ok = 1;
    if (!c->saved || !c->changed) return 1;
    /* Do not disable VRA before restoring its old sample rate. */
    if (!codec_write(c, 0x2c, c->saved_rate)) ok = 0;
    if (!codec_write(c, 0x2a, c->saved_ext)) ok = 0;
    if (!codec_write(c, 0x18, c->saved_pcm)) ok = 0;
    if (!codec_read(c, 0x18, &completion)) ok = 0;
    if (ok) c->changed = 0;
    return ok;
}

int cvaudio_codec_prepare(cvaudio_codec *c, uint32_t nam, uint32_t nabm,
                          cvaudio_read read, cvaudio_write write, void *opaque,
                          uint32_t *actual_rate)
{
    uint16_t caps, ext, rate, completion;
    uint32_t initial_error, initial_port;
    c->read = read; c->write = write; c->opaque = opaque;
    c->nam = nam; c->nabm = nabm;
    c->saved = c->changed = 0; c->error = c->last_port = 0;
    *actual_rate = 0;
    if (!codec_read(c, 0x2a, &c->saved_ext) ||
        !codec_read(c, 0x2c, &c->saved_rate) ||
        !codec_read(c, 0x18, &c->saved_pcm) ||
        !codec_read(c, 0x28, &caps)) return 0;
    c->saved = 1;
    if (caps & 1u) {
        c->changed = 1;
        if (!codec_write(c, 0x2a, (uint16_t)(c->saved_ext | 1u)) ||
            !codec_read(c, 0x2a, &ext)) goto failed;
        if (!(ext & 1u)) { c->error = CVAUDIO_CODEC_RATE; goto failed; }
        if (!codec_write(c, 0x2c, CVAUDIO_SOURCE_RATE)) goto failed;
    }
    if (!codec_read(c, 0x2c, &rate)) goto failed;
    if (rate < CVAUDIO_MIN_RATE || rate > CVAUDIO_MAX_RATE) {
        c->error = CVAUDIO_CODEC_RATE;
        goto failed;
    }
    c->changed = 1;
    /* Keep the user's master volume/mute. Only enable the PCM source, with
     * its existing attenuation, and wait for that posted write to finish. */
    if (!codec_write(c, 0x18, (uint16_t)(c->saved_pcm & 0x7fffu)) ||
        !codec_read(c, 0x18, &completion)) goto failed;
    *actual_rate = rate;
    return 1;
failed:
    initial_error = c->error; initial_port = c->last_port;
    (void)cvaudio_codec_restore(c);
    c->error = initial_error; c->last_port = initial_port;
    return 0;
}

void cvaudio_resampler_reset(cvaudio_resampler *r)
{
    r->phase = r->rate = 0; r->primed = 0;
}

int cvaudio_resample(cvaudio_resampler *r, int16_t *out, unsigned frames,
                    uint32_t rate, cvaudio_generate generate, void *opaque)
{
    unsigned needed, i, source = 0;
    uint32_t phase;
    if (!generate || frames > CVAUDIO_FRAMES || rate < CVAUDIO_MIN_RATE ||
        rate > CVAUDIO_MAX_RATE) return 0;
    if (!frames) return 1;
    if (r->rate != rate) { cvaudio_resampler_reset(r); r->rate = rate; }
    if (!r->primed) {
        generate(opaque, r->input, 2);
        r->left[0] = r->input[0]; r->right[0] = r->input[1];
        r->left[1] = r->input[2]; r->right[1] = r->input[3];
        r->primed = 1;
    }
    needed = (unsigned)((r->phase + frames * CVAUDIO_SOURCE_RATE) / rate);
    if (needed > CVAUDIO_INPUT_FRAMES) return 0;
    if (needed) generate(opaque, r->input, needed);
    phase = r->phase;
    for (i = 0; i != frames; ++i) {
        /* Weighted terms fit signed 32 bits for all supported codec rates;
         * multiplying a full-scale difference by phase would overflow. */
        out[i * 2u] = (int16_t)(((int32_t)r->left[0] * (int32_t)(rate - phase) +
                                (int32_t)r->left[1] * (int32_t)phase) / (int32_t)rate);
        out[i * 2u + 1u] = (int16_t)(((int32_t)r->right[0] * (int32_t)(rate - phase) +
                                     (int32_t)r->right[1] * (int32_t)phase) / (int32_t)rate);
        phase += CVAUDIO_SOURCE_RATE;
        while (phase >= rate) {
            phase -= rate;
            r->left[0] = r->left[1]; r->right[0] = r->right[1];
            r->left[1] = r->input[source * 2u];
            r->right[1] = r->input[source * 2u + 1u];
            ++source;
        }
    }
    r->phase = phase;
    return 1;
}

unsigned cvaudio_dma_slice(uint32_t sample_rate, uint32_t phase,
                          uint32_t units_left, unsigned channels, unsigned limit)
{
    uint32_t source, clocks, frames;
    if (!sample_rate || !units_left || !channels || !limit) return limit;
    source = units_left / channels + (units_left % channels != 0u);
    /* DSP blocks contain at most 65536 units; 65536 * 44100 fits u32. */
    if (source > 65536u || phase >= CVAUDIO_SOURCE_RATE) return 1;
    clocks = source * CVAUDIO_SOURCE_RATE;
    if (clocks <= phase) return 1;
    clocks -= phase;
    frames = clocks / sample_rate + (clocks % sample_rate != 0u);
    if (!frames) frames = 1;
    return frames < limit ? (unsigned)frames : limit;
}

void cvaudio_stream_reset(cvaudio_stream *r)
{
    r->phase = r->rate = 0;
    r->input_read = r->input_count = r->primed = 0;
}

static int stream_frame(cvaudio_stream *r, cvaudio_generate_partial generate,
                        void *opaque, unsigned wanted, int16_t *left, int16_t *right)
{
    if (r->input_read == r->input_count) {
        if (wanted > CVAUDIO_INPUT_FRAMES) wanted = CVAUDIO_INPUT_FRAMES;
        r->input_read = 0;
        r->input_count = generate(opaque, r->input, wanted);
        if (r->input_count > wanted) r->input_count = 0;
        if (!r->input_count) return 0;
    }
    *left = r->input[r->input_read * 2u];
    *right = r->input[r->input_read * 2u + 1u];
    ++r->input_read;
    return 1;
}

unsigned cvaudio_stream_convert(cvaudio_stream *r, int16_t *out, unsigned frames,
                                uint32_t rate, cvaudio_generate_partial generate,
                                void *opaque)
{
    unsigned i = 0, wanted;
    int16_t left, right;
    if (!generate || frames > CVAUDIO_FRAMES || rate < CVAUDIO_MIN_RATE ||
        rate > CVAUDIO_MAX_RATE) return 0;
    if (r->rate != rate) { cvaudio_stream_reset(r); r->rate = rate; }
    while (i != frames) {
        wanted = (unsigned)(((frames - i) * CVAUDIO_SOURCE_RATE + r->phase) / rate) + 2u;
        while (r->primed != 2u) {
            if (!stream_frame(r, generate, opaque, wanted, &left, &right)) return i;
            r->left[r->primed] = left;
            r->right[r->primed] = right;
            ++r->primed;
        }
        while (r->phase >= rate) {
            if (!stream_frame(r, generate, opaque, wanted, &left, &right)) return i;
            r->left[0] = r->left[1]; r->right[0] = r->right[1];
            r->left[1] = left; r->right[1] = right;
            r->phase -= rate;
        }
        out[i * 2u] = (int16_t)(((int32_t)r->left[0] * (int32_t)(rate - r->phase) +
                                (int32_t)r->left[1] * (int32_t)r->phase) / (int32_t)rate);
        out[i * 2u + 1u] = (int16_t)(((int32_t)r->right[0] * (int32_t)(rate - r->phase) +
                                     (int32_t)r->right[1] * (int32_t)r->phase) / (int32_t)rate);
        r->phase += CVAUDIO_SOURCE_RATE;
        ++i;
    }
    return i;
}
