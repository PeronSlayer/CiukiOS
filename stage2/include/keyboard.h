#ifndef STAGE2_KEYBOARD_H
#define STAGE2_KEYBOARD_H

#include "types.h"

/* Non-ASCII extended keys exposed by the set1 decoder. */
#define STAGE2_KEY_UP    0x80U
#define STAGE2_KEY_DOWN  0x81U
#define STAGE2_KEY_LEFT  0x82U
#define STAGE2_KEY_RIGHT 0x83U

void stage2_keyboard_init(void);
void stage2_keyboard_on_irq1(void);
u64 stage2_keyboard_irq_count(void);
i32 stage2_keyboard_getc_nonblocking(void);
u8 stage2_keyboard_getc_blocking(void);
void stage2_keyboard_flush_buffer(void);
int stage2_keyboard_alt_held(void);

#endif
