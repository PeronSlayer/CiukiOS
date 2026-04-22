#ifndef STAGE2_SPLASH_H
#define STAGE2_SPLASH_H

#include "types.h"

void stage2_splash_show(void);
int stage2_splash_show_graphic(void);
int stage2_splash_show_graphic_layout(u32 reserved_bottom_px);
unsigned int stage2_splash_source_cols(void);
unsigned int stage2_splash_source_rows(void);

#endif
