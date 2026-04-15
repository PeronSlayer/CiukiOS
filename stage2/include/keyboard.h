#ifndef STAGE2_KEYBOARD_H
#define STAGE2_KEYBOARD_H

#include "types.h"

void stage2_keyboard_init(void);
void stage2_keyboard_on_irq1(void);
u64 stage2_keyboard_irq_count(void);

#endif
