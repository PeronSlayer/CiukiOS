/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NET_H
#define CIUKI_NET_H
#include "http.h"
#include "tls.h"
#include <poll.h>
struct ciuki_net;
struct ciuki_connection;
struct ciuki_request {
    const char *host,*target,*post,*cookie;
    const char **headers;
    unsigned port;
    bool tls,head;
};
struct ciuki_job {
    /* Large parser and wire buffers exist only after pool admission. */
    struct ciuki_http *http;
    struct ciuki_request pending;
    void *snapshot;size_t pending_charge;
    ciuki_http_callback callback;void *context;
    struct ciuki_net *owner;
    struct ciuki_connection *connection;
    char host[254];unsigned port;bool secure,post,started,finished,cancelled,retried,pooled;
    uint64_t dns_token,started_ms;
    char *request;size_t request_bytes,sent;
    /* True only after this request's application data reaches the socket. */
    bool transmitted;
    const char *error;
    enum ciuki_tls_error tls_error;
};
#define CIUKI_NET_PENDING_BUDGET (128u*1024u)
size_t ciuki_net_pending_bytes(void);
struct ciuki_net *ciuki_net_create(const br_x509_trust_anchor *,size_t);
void ciuki_net_destroy(struct ciuki_net *);
struct ciuki_job *ciuki_net_job(const struct ciuki_request *,ciuki_http_callback,void *);
void ciuki_net_job_free(struct ciuki_job *);
bool ciuki_net_start(struct ciuki_net *,struct ciuki_job *);
void ciuki_net_cancel(struct ciuki_job *);
void ciuki_net_poll(struct ciuki_net *);
/* Called every UI turn, even when NetSurf has no active fetches. */
void ciuki_net_service_idle(struct ciuki_net *);
/* Port-local fd_set callers translate these to their own representation. */
unsigned ciuki_net_fds(struct ciuki_net *,struct pollfd [8]);
struct ciuki_tls *ciuki_net_tls(struct ciuki_job *);
#endif
