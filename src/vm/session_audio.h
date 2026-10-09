/* Physical AC-link transactions and continuous PCM rate conversion.
 * Independent of DOS, BIOS, allocation and interrupt delivery. */
#ifndef CIUKIOS_SESSION_AUDIO_H
#define CIUKIOS_SESSION_AUDIO_H
#include <stdint.h>
#if defined(__WATCOMC__)
#define CVAUDIO_CALL __cdecl
#else
#define CVAUDIO_CALL
#endif
#define CVAUDIO_SOURCE_RATE 44100u
#define CVAUDIO_FRAMES 256u
#define CVAUDIO_MIN_RATE 8000u
#define CVAUDIO_MAX_RATE 48000u
#define CVAUDIO_INPUT_FRAMES (CVAUDIO_FRAMES * CVAUDIO_SOURCE_RATE / CVAUDIO_MIN_RATE + 2u)
#define CVAUDIO_CODEC_TIMEOUT 1u
#define CVAUDIO_CODEC_ABSENT 2u
#define CVAUDIO_CODEC_RATE 3u
typedef uint32_t (CVAUDIO_CALL *cvaudio_read)(void *, uint32_t, uint32_t);
typedef void (CVAUDIO_CALL *cvaudio_write)(void *, uint32_t, uint32_t, uint32_t);
typedef void (CVAUDIO_CALL *cvaudio_generate)(void *, int16_t *, unsigned);
/* A short source batch is an explicit scheduling boundary. Zero means the
 * caller must yield before asking for more source frames. */
typedef unsigned (CVAUDIO_CALL *cvaudio_generate_partial)(void *, int16_t *, unsigned);
typedef struct cvaudio_codec {
    cvaudio_read read;
    cvaudio_write write;
    void *opaque;
    uint32_t nam, nabm, error, last_port;
    uint16_t saved_ext, saved_rate, saved_pcm;
    uint8_t saved, changed;
} cvaudio_codec;
typedef struct cvaudio_resampler {
    uint32_t phase, rate;
    uint8_t primed;
    int16_t left[2], right[2];
    int16_t input[CVAUDIO_INPUT_FRAMES * 2u];
} cvaudio_resampler;
typedef struct cvaudio_stream {
    uint32_t phase, rate;
    unsigned input_read, input_count, primed;
    int16_t left[2], right[2];
    int16_t input[CVAUDIO_INPUT_FRAMES * 2u];
} cvaudio_stream;
int CVAUDIO_CALL cvaudio_codec_prepare(cvaudio_codec *, uint32_t, uint32_t,
                                      cvaudio_read, cvaudio_write, void *, uint32_t *);
int CVAUDIO_CALL cvaudio_codec_restore(cvaudio_codec *);
void CVAUDIO_CALL cvaudio_resampler_reset(cvaudio_resampler *);
int CVAUDIO_CALL cvaudio_resample(cvaudio_resampler *, int16_t *, unsigned,
                                uint32_t, cvaudio_generate, void *);
/* Continue the exact rational phase across partial output descriptors. The
 * return value is the number of output frames actually produced. */
void CVAUDIO_CALL cvaudio_stream_reset(cvaudio_stream *);
unsigned CVAUDIO_CALL cvaudio_stream_convert(cvaudio_stream *, int16_t *, unsigned,
                                            uint32_t, cvaudio_generate_partial, void *);
/* Source output frames through the next SB interrupt boundary, inclusive. */
unsigned CVAUDIO_CALL cvaudio_dma_slice(uint32_t, uint32_t, uint32_t,
                                      unsigned, unsigned);
#endif
