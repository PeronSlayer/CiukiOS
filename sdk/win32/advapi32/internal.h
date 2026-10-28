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
/* --- sec-user-05 identity transports (identity.c). Production bodies are in
 * rpc.c; host tests replace only these boundaries. --- */
struct ciuki_w32_object_args;
/* One W32_OBJECT record; buffer (or NULL) becomes the record's name address.
 * Returns the Win32 error (transport failures included), 0 on success. */
uint32_t w32r_object(struct ciuki_w32_object_args *, void *buffer);
/* Per-install machine SID components A/B/C (REGISTRY RB_MACHINE_ID). */
uint32_t w32r_machine(uint32_t sid[3]);
/* One ACCOUNTS exchange (op, copied body, reply): 0 or -errno. */
int32_t w32r_accounts(uint32_t, const void *, uint32_t, void *, uint32_t, uint32_t *);
/* Frees a process-heap block; 0 when the pointer is not one. */
int w32r_release(void *);
/* --- end sec-user-05 --- */
#endif
