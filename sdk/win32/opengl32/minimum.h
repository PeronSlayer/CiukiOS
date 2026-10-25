/* Existing worker-only render3d minimum translation, no syscall/SDK additions.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_GL_MINIMUM_H
#define W32_GL_MINIMUM_H
#include "backend.h"
#include "../../../src/kernel/drivers/video/render3d.h"
/* Zero: fully expressible. Nonzero: use software, output must be discarded.
 * The provider supplies owned resource handles; COPY remains offscreen only.
 * Native depth/channel rounding requires independent tolerance qualification. */
int gl_hw_minimum_encode(const struct gl_hw_command *, unsigned target,
    unsigned depth, unsigned copy, struct render3d_request *);
#endif
