#ifndef CIUK_PLAYER_AUDIO_H
#define CIUK_PLAYER_AUDIO_H

#include "app.h"

#define APP_AUDIO_OPEN   0
#define APP_AUDIO_WRITE  1
#define APP_AUDIO_POLL   2
#define APP_AUDIO_PAUSE  3
#define APP_AUDIO_SEEK   4
#define APP_AUDIO_VOLUME 5
#define APP_AUDIO_CLOSE  6

#define APP_AUDIO_F_PAUSED 0x0001
#define APP_AUDIO_F_EOS    0x0002

#define APP_AUDIO_STATUS_OK          0
#define APP_AUDIO_STATUS_PLAYING     1
#define APP_AUDIO_STATUS_MUTED       2
#define APP_AUDIO_STATUS_UNAVAILABLE 3
#define APP_AUDIO_STATUS_READ_ERROR  4
#define APP_AUDIO_STATUS_BUSY        5
#define APP_AUDIO_STATUS_NO_MEMORY   6
#define APP_AUDIO_STATUS_DMA_ERROR  7

/* For APP_AUDIO_SEEK, input packet.played_frames is the requested target. */

#pragma pack(push,1)
struct app_audio_packet {
    u16 bytes;
    u16 status;
    u32 sample_rate;
    u32 total_frames;
    u32 played_frames;
    u32 queued_frames;
    u16 data_seg;
    u16 data_off;
    u16 frames;
    u16 volume;
    u16 flags;
};
#pragma pack(pop)

int app_audio(int op, struct app_audio_packet *packet);

#endif
