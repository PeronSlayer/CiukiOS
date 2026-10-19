/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_ADVAPI_INTERNAL_H
#define W32_ADVAPI_INTERNAL_H
#include <w32/base.h>
#include <w32/server.h>
/* Backend takes copied buffers and returns transport error or a complete reply.
 * Host tests replace only this transport, never the public API implementation. */
uint32_t w32r_exchange(const void *,uint32_t,void *,uint32_t,uint32_t *);
void w32r_pause(void);
void w32r_last_error(uint32_t);
int w32r_to_utf8(char *,uint32_t,const void *,int wide);
uint32_t w32r_from_utf8(void *,uint32_t,const char *,int wide);
/* Process-heap blocks for the f5-20A entry points (NULL-safe free). */
void *w32r_alloc(uint32_t);
void w32r_free(void *);
uint32_t w32r_registry_access(uint32_t);
#endif
