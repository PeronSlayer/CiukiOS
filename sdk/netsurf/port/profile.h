/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NETSURF_PROFILE_H
#define CIUKI_NETSURF_PROFILE_H
#include <stddef.h>
struct internet_profile_lock;
int ciuki_profile_transaction_begin(struct internet_profile_lock *);
void ciuki_profile_history_days(unsigned);
int ciuki_profile_init(const char *);
const char *ciuki_profile_path(const char *);
int ciuki_profile_replace(const char *,int (*)(const char *,void *),void *);
int ciuki_profile_admit(const char *,size_t,unsigned);
int ciuki_profile_visit(const char *);
int ciuki_profile_history_save(void);
void ciuki_profile_fini(void);
#endif
