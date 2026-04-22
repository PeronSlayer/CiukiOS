#ifndef STAGE2_TIMER_H
#define STAGE2_TIMER_H

#include "types.h"

void stage2_timer_init(void);
void stage2_timer_on_irq0(void);
u64 stage2_timer_ticks(void);

#endif
