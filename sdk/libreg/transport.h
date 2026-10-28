/* Private direct REGISTRY adapter. SPDX-License-Identifier: MIT */
#ifndef CIUKI_REG_TRANSPORT_H
#define CIUKI_REG_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
struct reg_remote { int fd; uint32_t root, service, endpoint, service_generation, peer_pid; uint64_t serial; };
struct rt;
int reg_remote_load(struct reg_remote *,struct rt **);
int reg_remote_commit(struct reg_remote *,struct rt *,uint64_t);
int reg_remote_win32(struct reg_remote *,const void *,size_t,void *,size_t,size_t *);
void reg_remote_close(struct reg_remote *);
/* Production uses call 101 and channels. Host H07 supplies a boundary fake
 * for the transport, linking the production broker and libreg above it. */
int reg_registry_rpc(struct reg_remote *,unsigned,const void *,size_t,void *,size_t,size_t *);
#endif
