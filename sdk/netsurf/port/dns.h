/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_DNS_H
#define CIUKI_DNS_H
#include <stdint.h>
struct ciuki_dns;
struct ciuki_dns *ciuki_dns_create(void);
/* Returns a nonzero generation token, or zero when the bounded queue is full. */
uint64_t ciuki_dns_submit(struct ciuki_dns *,const char *);
/* 0 pending, 1 completed. error is errno (EHOSTUNREACH for negative DNS). */
int ciuki_dns_result(struct ciuki_dns *,uint64_t,uint32_t *,int *);
void ciuki_dns_notify(struct ciuki_dns *,void (*)(void *),void *);
void ciuki_dns_cancel(struct ciuki_dns *,uint64_t);
/* Worker owns the final release. Never joins an outstanding resolve4 on the UI. */
void ciuki_dns_destroy(struct ciuki_dns *);
#endif
