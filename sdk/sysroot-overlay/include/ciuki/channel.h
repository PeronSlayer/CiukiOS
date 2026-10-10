/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_CHANNEL_H
#define CIUKI_CHANNEL_H
#include <ciuki/abi.h>
int ciuki_channel_pair(int [2]);
int ciuki_channel_send(int,const struct ciuki_message *,uint32_t);
int ciuki_channel_recv(int,struct ciuki_message *,uint32_t);
int ciuki_wait_word(uint32_t *,uint32_t,const struct ciuki_timespec *,int);
int ciuki_wake_word(uint32_t *,uint32_t);
#endif
