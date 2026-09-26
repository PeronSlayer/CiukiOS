#ifndef CIUKIOS_GRAPHICS_BRIDGE_H
#define CIUKIOS_GRAPHICS_BRIDGE_H
#include <stdint.h>
#define CG_DISCOVER_AX 0xD74A
#define CG_DISCOVER_BX 0x4347
#define CG_DISCOVER_REPLY 0x4743
#define CG_FLAG_FOCUSED 1
#define CG_FLAG_CLOSE 2
#define CG_FLAG_MULTIPLEX 4
#define CG_PRESENT_POLL 2
#pragma pack(push,1)
typedef struct { uint16_t offset,segment; } cg_far;
typedef struct {
    uint16_t version,size,width,height,pitch,flags;
    cg_far pixels,palette,present,input;
    uint32_t milliseconds,presented_frames;
    uint16_t status,host_bpp;
    uint32_t clock_deferred_samples,reserved;
} cg_descriptor;
#pragma pack(pop)
typedef char cg_descriptor_size_must_be_48[(sizeof(cg_descriptor)==48)?1:-1];
#ifdef __cplusplus
extern "C" {
#endif
int cg_open(void);
int cg_present(int palette_changed);
int cg_key(unsigned *scan, int *pressed);
uint32_t cg_ticks(void);
int cg_close_requested(void);
int cg_failed(void);
unsigned cg_error_code(void);
unsigned cg_error_service(void);
unsigned char *cg_pixels(void);
unsigned char *cg_palette(void);
const volatile cg_descriptor *cg_info(void);
#ifdef __cplusplus
}
#endif
#endif
