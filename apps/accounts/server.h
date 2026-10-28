/* SPDX-License-Identifier: MIT
 * Serving loop of the sealed ACCOUNT process (sec-int-01). main.c runs it on
 * the target; the host integration harness drives the same functions over
 * the kernel host harness. Not an SDK ABI.
 */
#ifndef ACCT_SERVER_H
#define ACCT_SERVER_H
#include "accounts.h"
#include "kernel_adapter.h"
#include "firstboot.h"

#define ACCT_SERVER_OPEN_RETRY_MS 30000u
#define ACCT_SERVER_FIRSTBOOT_RETRY_MS 100u
struct acct_tree_io; /* sec-own: production.h */
struct acct_server {
    struct acct_kernel_adapter kernel;
    struct acct_store tables;   /* kernel table bound to the adapter above */
    struct acct_service service;
    struct acct_transport transport;
    struct acct_response response;
    uint64_t next_open;
    unsigned sweep;
    int opened, last_open_error, host_test;
    /* sec-user-02 first-boot adapter: the shared G32 settings binding (NULL:
     * no G32 migration) and the last adapter result, for diagnostics. */
    const struct acct_legacy_io *legacy;
    int firstboot_result;
    /* sec-own: native file access for the DOS saves migration (NULL: none). */
    const struct acct_tree_io *tree;
};
/* Binds the production kernel adapter beside the supplied OWNER/storage
 * tables and volume. host_test is set only by host harnesses; the image
 * passes 0, so acct_service_open demands real tables. 0 or -errno. */
int acct_server_init(struct acct_server *, const struct acct_store *tables, int host_test);
/* Opens the service when closed and the retry time has come (every
 * ACCT_SERVER_OPEN_RETRY_MS). Returns 1 once open, 0 while closed. */
int acct_server_open(struct acct_server *, uint64_t now);
/* SERVICE_ACCEPT on AUTH and ACCOUNTS until both backlogs are empty. */
void acct_server_accept(struct acct_server *);
/* Drains one registered endpoint: frames go to the service while it is
 * open; a request that cannot be admitted gets a status-only reply (EIO
 * while closed). Closed/revoked links are retired. */
void acct_server_serve(struct acct_server *, int fd);
/* Retires registered endpoints whose link is no longer live. */
void acct_server_sweep(struct acct_server *);
/* Runs queued work; returns the wait bound (ms) before the next pass. */
unsigned acct_server_work(struct acct_server *, unsigned idle_ms);
/* One pass of the image's loop: open, accept, work, wait, serve, sweep. */
void acct_server_step(struct acct_server *, unsigned idle_ms);
/* Closes the service and every registered endpoint (host harness teardown). */
void acct_server_close(struct acct_server *);
#endif
