/* SPDX-License-Identifier: MIT
 * stb_image v2.30 (third_party/stb, MIT or Public Domain) compiled for the
 * Image Viewer: PNG, baseline/progressive JPEG, BMP and GIF (first frame). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS   /* the SDK excludes compiler TLS */
#define STBI_MAX_DIMENSIONS 8192
/* stb_image keeps its GIF state (~35 KB) and PNG palette buffers on the stack.
 * The SDK warns above 4 KB frames (guard-page policy); the main thread stack
 * is 1 MiB (CIUKI_MAIN_STACK_BASE..LIMIT) and the decoder runs at a shallow
 * depth from viewer_decode, so these frames stay inside it. Lead review item. */
#pragma clang diagnostic ignored "-Wframe-larger-than"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wcast-qual"
#include "../../third_party/stb/stb_image.h"
