/* SPDX-License-Identifier: MIT
 * Unmodified upstream stb headers; third_party/stb/README.md and LICENSE.
 * Only PNG, BMP, GIF and JPEG decoders. Callback writers let the caller check I/O errors. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 2048
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#ifdef __clang__
#pragma clang diagnostic ignored "-Wframe-larger-than"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wcast-qual"
#endif
#include "../../third_party/stb/stb_image.h"
/* The upstream JPEG writer shifts a bit buffer left into the sign bit of an int (stb_image_write.h, stbiw__jpg_writeBits):
 * well defined on two's complement targets, but flagged by UBSan in the host tests; the header stays unmodified. */
#ifdef __clang__
#pragma clang attribute push(__attribute__((no_sanitize("shift"))), apply_to = function)
#endif
#include "../../third_party/stb/stb_image_write.h"
#ifdef __clang__
#pragma clang attribute pop
#endif
