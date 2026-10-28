/* Private sealed service loop. SPDX-License-Identifier: MIT */
#ifndef CIUKI_REGISTRY_SERVER_H
#define CIUKI_REGISTRY_SERVER_H
#include "kernel_adapter.h"
struct rb_client {
    uintptr_t lease;
    int fd;
    unsigned slot;
    uint32_t endpoint,op,total,received;
    uint64_t request,last,expected,deadline;
    uint8_t *body;
};
struct rb_server {
    struct rb_kernel kernel;
    struct rb_broker broker;
    struct rb_client clients[RB_LINK_MAX];
};
int rb_server_init(struct rb_server *,const struct rb_storage *,struct wr_server *,
                   int (*account)(void *,uint32_t,struct rb_identity *,int *),void *context);
void rb_server_step(struct rb_server *,unsigned idle_ms);
void rb_server_destroy(struct rb_server *);
int rb_installation_identity(struct rb_identity *);
int rb_account_users(void *,const struct rb_identity *,char **,size_t *);
int rb_account_current(void *,uint32_t,struct rb_identity *,int *);
#endif
