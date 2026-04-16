#ifndef STAGE2_KEYBOARD_H
#define STAGE2_KEYBOARD_H

#include "types.h"

void stage2_keyboard_init(void);
void stage2_keyboard_on_irq1(void);
u64 stage2_keyboard_irq_count(void);
i32 stage2_keyboard_getc_nonblocking(void);
u8 stage2_keyboard_getc_blocking(void);
void stage2_keyboard_flush_buffer(void);

#endif
