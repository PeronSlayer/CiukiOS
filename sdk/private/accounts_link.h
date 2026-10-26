/* SPDX-License-Identifier: MIT
 * Private ACCOUNT/LOGON binding; never installed in the public SDK sysroot. */
#ifndef CIUKI_PRIVATE_ACCOUNTS_LINK_H
#define CIUKI_PRIVATE_ACCOUNTS_LINK_H
#include <stdint.h>
struct acct_subject;
/* Sealed bootstrap supplies this context. The backend resolves both retained
 * handles in the caller's private lease table and invokes
 * security_subject_same_link on the retained kernel subjects. It must never
 * cast a userspace handle to a kernel pointer or compare handles as identity.
 * Missing runtime transport is an error; QUERY_PEER is not a substitute. */
struct ciuki_accounts_link_binding {
    void *context;
    int (*same_link)(void *, uintptr_t, uintptr_t);
};
/* Exact acct_kernel.same_link callback shape. Other service callbacks sharing
 * acct_kernel.ctx must use the same enclosing sealed binding context. */
int ciuki_accounts_same_link(void *, const struct acct_subject *, const struct acct_subject *);
#endif
