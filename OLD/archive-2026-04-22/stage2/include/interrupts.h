#ifndef STAGE2_INTERRUPTS_H
#define STAGE2_INTERRUPTS_H

#include "types.h"

void stage2_init_idt(void);
void stage2_enable_interrupts(void);
void stage2_exception_panic(u64 vector, u64 error_code);

#endif
