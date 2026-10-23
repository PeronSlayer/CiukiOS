/* SPDX-License-Identifier: MIT -- application-private native adapter. */
#ifndef SETUP_NATIVE_H
#define SETUP_NATIVE_H
#include "setup.h"
#include <stdio.h>
int st_native_open(struct st_host *, int display_fd, int input_fd);
void st_native_close(void);
/* Streaming parser for f9-01 CDMANIFE.JSN; fixed storage, no payload reads. */
int st_manifest_sizes(FILE *, struct st_size out[SETUP_COMP_COUNT]);
#endif
