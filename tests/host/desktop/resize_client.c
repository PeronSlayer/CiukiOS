/* Compile the unchanged production glue with only its OS calls redirected.
 * SPDX-License-Identifier: MIT */
#include "replay_base.h"
#include "resize_transport.h"
#define close rt_close
#define munmap rt_unmap
#include "../../../apps/libdeskui/client.c"
