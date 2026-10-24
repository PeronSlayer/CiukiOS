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
/* Producer callback: DESK_CLIP_RENDER (one format), DESTROY, or RELEASE
 * (render all before window destruction). RENDER calls du_clip_put without
 * opening the clipboard; RELEASE opens/closes it like WM_RENDERALLFORMATS. */
typedef void (*du_clip_producer)(void *, unsigned event, unsigned format);
void du_clip_producer_bind(du_clip_producer, void *);
int du_clip_delay(unsigned format);
void du_clip_event(const struct desk_clip_wire *);
void du_clip_release(void);
/* CF_OWNERDISPLAY viewer messages 0309h..030eh, explicit byte payload. */
int du_clip_display(unsigned message,const void *,uint32_t,void **,uint32_t *);
int du_clip_register(const char *);
int du_clip_query(struct desk_clip_wire *);
int du_clip_available(unsigned format); /* notification-cached menu state; GET remains fresh */
void du_clip_observe(uint32_t);
uint32_t du_clip_observed(void);
int du_system_clipboard_text_set(const char *, size_t);
const char *du_system_clipboard_text_get(void);
#endif
