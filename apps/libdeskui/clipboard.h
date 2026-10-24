/* SPDX-License-Identifier: MIT. Native clipboard client, process-wide. */
#ifndef DU_CLIPBOARD_H
#define DU_CLIPBOARD_H
#include "../desktop/protocol.h"
#include <stddef.h>
typedef int (*du_clip_transport)(void *, struct desk_clip_wire *, const void *, struct desk_clip_wire *, void **);
void du_clip_bind(du_clip_transport, void *);
int du_clip_call(struct desk_clip_wire *, const void *, struct desk_clip_wire *, void **);
int du_clip_open(void);
int du_clip_close(void);
int du_clip_empty(void);
int du_clip_put(unsigned format, const void *, uint32_t);
int du_clip_get(unsigned format, void **, uint32_t *);
int du_clip_register(const char *);
int du_clip_query(struct desk_clip_wire *);
int du_clip_available(unsigned format); /* notification-cached menu state; GET remains fresh */
void du_clip_observe(uint32_t);
uint32_t du_clip_observed(void);
int du_system_clipboard_text_set(const char *, size_t);
const char *du_system_clipboard_text_get(void);
#endif
