#ifndef STAGE2_VIDEO_H
#define STAGE2_VIDEO_H

#include "types.h"
#include "bootinfo.h"

void video_init(boot_info_t *bi);
void video_putchar(char c);
void video_write(const char *s);
void video_write_hex64(u64 v);
void video_write_hex8(u8 v);

#endif
