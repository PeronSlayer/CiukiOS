/* G1: the kernel's POSIX TZ rule evaluator as its own libciuki member, so
 * libsetup's configuration writer and the libc time functions share one
 * copy at link time. SPDX-License-Identifier: MIT */
#include "../../src/kernel/lib/tzrule.c"
