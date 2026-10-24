/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_NET_H
#define CIUKI_NET_H
#include "http.h"
#include "tls.h"
#include <poll.h>
struct ciuki_net;
struct ciuki_connection;
enum ciuki_fetch_stage { CIUKI_FETCH_DNS,CIUKI_FETCH_CONNECT,CIUKI_FETCH_TLS,
    CIUKI_FETCH_REQUEST,CIUKI_FETCH_RESPONSE,CIUKI_FETCH_REDIRECT,
    CIUKI_FETCH_DONE,CIUKI_FETCH_ERROR };
struct ciuki_request {
    const char *host,*target,*post,*cookie,*proxy_host,*proxy_auth;
    unsigned proxy_port;
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
    char proxy_host[254],proxy_auth[264];unsigned proxy_port;
    char host[254];unsigned port;bool secure,post,started,finished,cancelled,retried,pooled;
    uint64_t dns_token,started_ms;
    char *request;size_t request_bytes,sent;
    /* True only after this request's application data reaches the socket. */
    bool transmitted;
    const char *error;
    char tls_message[192];
    /* f6-22: a network failure or readiness message with the network state. */
    char error_text[400];
    enum ciuki_tls_error tls_error;
    /* Protected diagnostics: never retain the query, credentials or fragment. */
    char path_prefix[41];
    unsigned record_mask,record_id;
    enum ciuki_fetch_stage stage;
    int io_errno,tls_engine_error;
    uint64_t record_ms;
};
#define CIUKI_NET_PENDING_BUDGET (128u*1024u)
size_t ciuki_net_pending_bytes(void);
struct ciuki_net *ciuki_net_create(const br_x509_trust_anchor *,size_t);
void ciuki_net_notify(struct ciuki_net *,void (*)(void *),void *);
int ciuki_net_timeout(struct ciuki_net *,int);
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
void ciuki_net_record(struct ciuki_job *,enum ciuki_fetch_stage);
/* Also used for errors raised by the NetSurf binding after response headers. */
void ciuki_net_failure(struct ciuki_job *,const char *);
/* f6-22: the network as this browser sees it: the kernel's /dev/net record
 * (adapter, link, address) and the outcome of this browser's last name
 * lookup. The word is one of unknown (no /dev/net), no-adapter, stopped,
 * no-link, no-address, address, dns-ok, dns-failed; the text is a sentence
 * for error and readiness pages, empty when /dev/net is unavailable. */
const char *ciuki_net_state_word(void);
size_t ciuki_net_state_text(char *,size_t);
/* fx-11: "fetches=N fetch_ok=N fetch_errors=N last_error=<stage>:<errno>:<tls>"
 * for the exit line; error records also reach ciuki_netsurf_log_record
 * (chrome.c: NETSURF.LOG, 16 per session) when that hook is linked. */
size_t ciuki_net_fetch_summary(char *,size_t);
void ciuki_netsurf_log_record(const char *);
#endif
