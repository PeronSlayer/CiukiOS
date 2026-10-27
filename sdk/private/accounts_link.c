/* SPDX-License-Identifier: MIT */
#include "accounts_link.h"
#include "adapter.h" /* apps/accounts private interface, supplied at link time */
#include <errno.h>

int ciuki_accounts_same_link(void *context, const struct acct_subject *a, const struct acct_subject *b)
{
    const struct ciuki_accounts_link_binding *binding=context;
    if (!a || !b || !a->retained || !b->retained) return -EACCES;
    if (!binding || !binding->same_link) return -EIO;
    /* Metadata in acct_subject is a snapshot, not link authority. Distinct
     * handles may resolve to leases of the same endpoint; even equal handles
     * require kernel liveness/revocation checks. */
    return binding->same_link(binding->context,a->retained,b->retained);
}
