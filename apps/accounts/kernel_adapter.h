/* SPDX-License-Identifier: MIT
 * Production binding of the private acct_kernel seams to the sec-k01 kernel
 * (call 101 and the service channels), sec-int-01. The ACCOUNT process is a
 * user-space sealed service: its retained subjects are leases over the
 * accepted endpoint descriptors it keeps open, identified by the kernel's
 * per-pair endpoint generation and revalidated through QUERY_PEER on every
 * liveness decision. Not an SDK ABI; only server.c and the host integration
 * harness use it.
 */
#ifndef ACCT_KERNEL_ADAPTER_H
#define ACCT_KERNEL_ADAPTER_H
#include "adapter.h"
#include "accounts_link.h"

#define ACCT_KERNEL_LINKS 32u   /* accepted endpoints kept open by the service */
#define ACCT_KERNEL_LEASES 64u  /* retained subjects over those endpoints */
#define ACCT_KERNEL_PICTURES "/System/Security/Pictures"

struct acct_kernel_link {
    int fd;
    uint32_t endpoint_generation, service, service_generation;
    uint32_t peer_pid, peer_process_generation, peer_role, session_id;
    uint64_t active_request, last_request;
    int occupied, retiring;
};
struct acct_kernel_lease { unsigned link; uint32_t serial; unsigned refs; int occupied; };
struct acct_kernel_adapter {
    struct ciuki_accounts_link_binding binding; /* first: acct_kernel.ctx points here */
    struct acct_kernel_link links[ACCT_KERNEL_LINKS];
    struct acct_kernel_lease leases[ACCT_KERNEL_LEASES];
    struct acct_kernel_link logon; /* private ACCOUNT->LOGON client link (service 5) */
    uint64_t logon_request;
    uint32_t serial, pid, generation, boot_flags;
    int leased, safe_mode;
    unsigned allocations;
    char picture_root[64];
};
/* Fills *out with the production callbacks over this adapter. */
int acct_kernel_adapter_init(struct acct_kernel_adapter *, struct acct_kernel *out);
/* SERVICE_ACCEPT for one service and registration of the accepted endpoint:
 * 0 with *fd, -EAGAIN when the backlog is empty, -ENOSPC when every link
 * slot is in use, other -errno. */
int acct_kernel_adapter_accept(struct acct_kernel_adapter *, uint32_t service, int *fd);
/* 1 when the registered endpoint's peer is still live, 0 when revoked/dead,
 * -EBADF when fd is not a registered endpoint. */
int acct_kernel_adapter_link_live(struct acct_kernel_adapter *, int fd);
/* Retires a registered endpoint: 0 when closed now, -EBUSY when a lease or
 * an exchange still refers to it (it leaves the wait set and closes when
 * the last reference goes), -EBADF when unknown. */
int acct_kernel_adapter_retire(struct acct_kernel_adapter *, int fd);
/* Registered endpoint descriptors that are not retiring, for the wait set. */
unsigned acct_kernel_adapter_fds(const struct acct_kernel_adapter *, int *fds, unsigned capacity);
/* Closes the private ACCOUNT->LOGON link (teardown). */
void acct_kernel_adapter_shutdown(struct acct_kernel_adapter *);
#endif
