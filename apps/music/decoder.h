/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_MUSIC_DECODER_H
#define CIUKI_MUSIC_DECODER_H
#include <stddef.h>
#include <stdint.h>
struct music_decoder;
typedef int (*music_cancel)(void *);
/* All calls belong to the worker. Streaming storage does not grow with song length. */
int music_decoder_open(struct music_decoder **, const char *path, unsigned output_rate, music_cancel, void *);
int music_decoder_read(struct music_decoder *, int16_t *stereo, unsigned frames);
int music_decoder_seek(struct music_decoder *, uint64_t output_frame);
/* Zero duration means unknown until EOF when an MP3 lacks a Xing/Info count. */
uint64_t music_decoder_length(const struct music_decoder *);
unsigned music_decoder_rate(const struct music_decoder *);
unsigned music_decoder_channels(const struct music_decoder *);
void music_decoder_close(struct music_decoder *);
#endif
