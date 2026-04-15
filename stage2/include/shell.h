#ifndef STAGE2_SHELL_H
#define STAGE2_SHELL_H

#include "bootinfo.h"
#include "handoff.h"

void stage2_shell_run(boot_info_t *boot_info, handoff_v0_t *handoff);

#endif
