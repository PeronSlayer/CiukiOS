#ifndef CIUK_MEDIA_ABI_H
#define CIUK_MEDIA_ABI_H
#include <stdint.h>
#ifdef __WATCOMC__
typedef unsigned long media_u32;
#else
typedef uint32_t media_u32;
#endif
#define MEDIA_MAGIC 0x324D5543UL
#define MEDIA_ABI 1
#define MEDIA_BYTES 131072UL
#define MEDIA_INPUT 64UL
#define MEDIA_OUTPUT 65536UL
#define MEDIA_OUTPUT_CAPACITY 49152UL
#define MEDIA_OPEN 1
#define MEDIA_DECODE 2
#define MEDIA_SEEK 3
#define MEDIA_CLOSE 4
#define MEDIA_EXIT 5
#pragma pack(push,1)
struct media_header {
    media_u32 magic,abi,sequence,completed;
    media_u32 operation,error,input_bytes,output_bytes;
    media_u32 sample_rate,total_frames,position,output_frames;
    media_u32 state,heartbeat,reserved[2];
};
#pragma pack(pop)
#endif
