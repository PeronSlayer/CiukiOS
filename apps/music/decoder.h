/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_MUSIC_DECODER_H
#define CIUKI_MUSIC_DECODER_H
#include <stddef.h>
#include <stdint.h>
struct music_decoder;
typedef int (*music_cancel)(void *);
/* One caller owns the handle at a time; join the reader before seek/close. Streaming storage does not grow with song length. */
int music_decoder_open(struct music_decoder **, const char *path, unsigned output_rate, music_cancel, void *);
int music_decoder_read(struct music_decoder *, int16_t *stereo, unsigned frames);
int music_decoder_seek(struct music_decoder *, uint64_t output_frame);
/* Zero duration means unknown until EOF when an MP3 lacks a Xing/Info count. */
uint64_t music_decoder_length(const struct music_decoder *);
unsigned music_decoder_rate(const struct music_decoder *);
unsigned music_decoder_channels(const struct music_decoder *);
void music_decoder_close(struct music_decoder *);
/* f4-37: where the decoder's time goes, in time-stamp-counter cycles
 * (docs/design/music-player.md, "Decoder cost record"). MP3 splits minimp3's
 * layer III frame into decode (side info, bit reservoir, scalefactors,
 * Huffman decoding with requantisation, stereo processing), imdct (reorder,
 * alias reduction, IMDCT and overlap), synth (DCT-II and polyphase window)
 * and pcm (float to int16); WAV and Ogg Vorbis blocks count as decode. adapter is the
 * rest of music_decoder_read (rate conversion, stereo extraction, input
 * buffering), io the stdio reads inside it, including any wait for storage.
 * frames counts decoded MPEG frames (blocks for WAV/Ogg), out output frames,
 * best the fewest cycles one complete MPEG frame took (0: none). Every
 * eighth MPEG frame the synthesis and overlap state is scanned for values
 * below FLT_MIN (tiny, scans); de/ue count frames whose float unit raised the
 * denormal-operand or underflow flag. */
struct music_cost {
    uint64_t decode, imdct, synth, pcm, adapter, io, best;
    unsigned frames, out, tiny, scans, de, ue;
};
/* Takes the counters accumulated since the last call; the handle's owner only. */
void music_decoder_cost(struct music_decoder *, struct music_cost *);
static inline uint64_t music_cycles(void)
{
#if defined(__i386__) || defined(__x86_64__)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}
#endif
