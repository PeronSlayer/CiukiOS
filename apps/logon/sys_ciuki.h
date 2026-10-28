/* SPDX-License-Identifier: MIT
 * Target seams over the Ciuki SDK: client_ciuki.c (CSC1 client seam, used by
 * LOGON_UI and the desktop) and sys_ciuki.c (LOGON's seam). */
#ifndef CIUKI_LOGON_SYS_CIUKI_H
#define CIUKI_LOGON_SYS_CIUKI_H
#include "logon_proto.h"
struct lgn_sys;
extern const struct lgn_sys lgn_sys_ciuki;
extern const struct lgn_client_sys lgn_client_ciuki;
int lgn_ciuki_security(void *, struct ciuki_security_args *, const void *, uint32_t, void *, uint32_t);
int lgn_ciuki_send(void *, int, const struct ciuki_message *);
int lgn_ciuki_recv(void *, int, struct ciuki_message *);
int lgn_ciuki_close(void *, int);
int lgn_ciuki_wait(void *, const int *, unsigned, unsigned);
uint64_t lgn_ciuki_now(void *);
void lgn_log_ciuki(const char *line); /* kernel debug log, one line */
#endif
