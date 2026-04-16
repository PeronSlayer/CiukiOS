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
void video_set_font_scale(u32 scale_x, u32 scale_y);
u32 video_columns(void);
u32 video_text_rows(void);
int video_ready(void);
u32 video_width_px(void);
u32 video_height_px(void);
u32 video_pitch_bytes(void);
u32 video_bpp(void);
u32 video_cell_width_px(void);
u32 video_cell_height_px(void);
void video_fill(u32 rgb);
void video_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb);
void video_put_pixel(u32 x, u32 y, u32 rgb);
void video_present(void);
void video_present_dirty(void);
void video_mark_dirty(u32 x, u32 y, u32 w, u32 h);
int  video_is_double_buffered(void);
void video_blit_row(u32 dst_x, u32 dst_y, const u32 *pixels_rgb, u32 count);

#endif
