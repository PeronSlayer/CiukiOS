#ifndef STAGE2_VIDEO_H
#define STAGE2_VIDEO_H

#include "types.h"
#include "bootinfo.h"

void video_init(boot_info_t *bi);
void video_cls(void);
void video_putchar(char c);
void video_write(const char *s);
void video_write_hex64(u64 v);
void video_write_hex8(u8 v);
void video_set_cursor(u32 col, u32 row);
void video_set_colors(u32 fg, u32 bg);
void video_set_text_window(u32 start_row);
u32 video_columns(void);

#endif
