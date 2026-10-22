/* SPDX-License-Identifier: MIT
 * f4-37: the reference decoder for music_x87_test.c, upstream minimp3 with
 * its own int16 output (no MINIMP3_FLOAT_OUTPUT), under renamed entry points
 * so it links beside the production decoder's float-output copy. */
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#define mp3dec_init ref_mp3dec_init
#define mp3dec_decode_frame ref_mp3dec_decode_frame
#include "../../../third_party/minimp3/minimp3.h"
