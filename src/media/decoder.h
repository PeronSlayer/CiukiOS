#ifndef CIUK_MEDIA_DECODER_H
#define CIUK_MEDIA_DECODER_H
#include <stdint.h>
#define MEDIA_PCM_RATE 48000UL
#define MEDIA_INPUT_FRAMES 2048
#define MEDIA_OUTPUT_FRAMES (MEDIA_INPUT_FRAMES * 6)
typedef struct media_decoder media_decoder;
int media_decoder_open(const char *path, uint32_t *rate, uint32_t *total_frames);
int media_decoder_read(int16_t *stereo, uint32_t capacity_frames, uint32_t *frames_read);
int media_decoder_seek(uint32_t output_frame);
void media_decoder_close(void);
#endif
