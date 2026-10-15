/* Raw reserved-sector recorder. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BLACKBOX_H
#define CIUKI_BLACKBOX_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define BLACKBOX_DESCRIPTOR 8u
#define BLACKBOX_FIRST 32u
#define BLACKBOX_SECTORS 128u
#define BLACKBOX_RESERVED (BLACKBOX_FIRST + BLACKBOX_SECTORS)
#define BLACKBOX_REPLAY_BYTES (BLACKBOX_SECTORS * 512u)
struct blkdev;
void blackbox_serial_replay(const char *, size_t); /* output sink, no recapture */
/* Bind only a verified system partition before FAT mount. Replay callback
 * receives bounded [blackbox] lines; no logging is done by this module. */
int blackbox_bind(struct blkdev *, uint64_t start, uint64_t count,
                  void (*serial)(const char *, size_t));
void blackbox_capture(const char *, size_t); /* RAM only, any producer */
bool blackbox_available(void); /* validated descriptor and durable device */
int blackbox_checkpoint(void); /* rare checkpoint/shutdown: drain full bounded RAM */
int blackbox_flush(void); /* direct writes + device flush, task context */
void blackbox_panic(const char *, size_t); /* bounded emergency callback */
const char *blackbox_replay(size_t *);
void blackbox_reset(void); /* quiescent detach */
#endif
