/* CiukWeb image decoder CAPP interface. The caller keeps this packed request
 * in conventional memory for the decoder session and supplies a 6144-byte
 * RGB888 scratch buffer. EV_OPEN receives request segment:offset in a:b;
 * EV_POLL writes one decoded rectangle and updated status into this record. */
#ifndef CIUKIOS_WEBIMG_H
#define CIUKIOS_WEBIMG_H

#include "app.h"

#define WEBIMG_ABI_VERSION 1
#define WEBIMG_PATH_BYTES 260
#define WEBIMG_RGB_ROW_MAX (2048UL * 3UL)

#define WEBIMG_STATUS_IDLE    0
#define WEBIMG_STATUS_READY   1  /* dimensions available after EV_OPEN */
#define WEBIMG_STATUS_PIXELS  2  /* EV_POLL produced x/y/w/h RGB888 bytes */
#define WEBIMG_STATUS_DONE    3
#define WEBIMG_STATUS_ERROR   4

#define WEBIMG_FMT_UNKNOWN 0
#define WEBIMG_FMT_PNG     1
#define WEBIMG_FMT_JPEG    2
#define WEBIMG_FMT_GIF     3

#define WEBIMG_ERR_NONE       0
#define WEBIMG_ERR_IO         1
#define WEBIMG_ERR_FORMAT     2
#define WEBIMG_ERR_UNSUPPORTED 3
#define WEBIMG_ERR_LIMIT      4
#define WEBIMG_ERR_MEMORY     5
#define WEBIMG_ERR_OUTPUT     6

#pragma pack(push, 1)
struct webimg_request {
    u16 abi_version;
    u16 struct_bytes;
    char path[WEBIMG_PATH_BYTES]; /* local compressed file; read-only */
    u16 output_seg;
    u16 output_off;
    u16 output_capacity;          /* >= WEBIMG_RGB_ROW_MAX */
    u16 status;
    u16 format;
    u16 error;
    u16 total_width;
    u16 total_height;
    u16 x, y, w, h;               /* rectangle emitted by last EV_POLL */
    u16 bytes_written;
};
#pragma pack(pop)

#endif
