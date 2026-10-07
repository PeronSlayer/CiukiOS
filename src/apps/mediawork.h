#ifndef CIUK_MEDIAWORK_H
#define CIUK_MEDIAWORK_H
#include "app.h"
#include "../media/media_abi.h"
int mediawork_start(const char *path);
int mediawork_decode(void);
int mediawork_seek(u32 output_frame);
/* 0 pending, 1 completed, -1 worker/transport failure. */
int mediawork_poll(struct media_header *reply);
int mediawork_read(u32 output_offset, void *buffer, u16 bytes);
int mediawork_close(void);
#endif
