/* Private i740 state, no userspace ABI. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VIDEO_I740_H
#define CIUKI_VIDEO_I740_H
#include "../native.h"
struct i740_engine {
    struct video_native native;
    volatile uint8_t *cursor;
    uint32_t cursor_offset;
    uint8_t cursor_saved[4096], saved_cursor[7];
    bool cursor_saved_valid;
};
int i740_engine_prepare(struct i740_engine *);
extern const struct display_driver i740_driver;
#endif
