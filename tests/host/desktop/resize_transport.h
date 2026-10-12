/* SPDX-License-Identifier: MIT */
#ifndef RESIZE_TRANSPORT_H
#define RESIZE_TRANSPORT_H
#include "../../../apps/desktop/desktop.h"
extern const struct desk_ops rt_ops;
extern int rt_block, rt_fail_create, rt_fail_map, rt_eof;
void rt_reset(void);
unsigned rt_surfaces(void);
unsigned rt_mappings(void);
unsigned rt_handles(void);
int rt_pop(struct ciuki_message *);
void rt_push(const struct ciuki_message *);
int rt_close(int);
int rt_unmap(void *, size_t);
void rt_client_tests(void);
#endif
