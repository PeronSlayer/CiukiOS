#include "worker_tls.h"
#include "worker_x509_ip.h"

#include <string.h>

#define WTLS_IO_BYTES 32768u
#define WTLS_SEED_BYTES 32u
#define WTLS_HOST_MAX 255u
#define WTLS_LEAF_CERT_MAX 32768u
#define WTLS_UNIX_EPOCH_DAYS 719528UL

typedef struct {
    const br_x509_class *vtable;
    br_x509_minimal_context *inner;
    unsigned char ip_address[4];
    unsigned char leaf_certificate[WTLS_LEAF_CERT_MAX];
    uint32_t leaf_expected;
    uint32_t leaf_used;
    int ip_mode;
    int leaf_seen;
    int leaf_current;
    int leaf_complete;
} wtls_x509_proxy;

static br_ssl_client_context wtls_client;
static br_x509_minimal_context wtls_x509;
static wtls_x509_proxy wtls_x509_check;
static unsigned char wtls_input[WTLS_IO_BYTES];
static unsigned char wtls_output[WTLS_IO_BYTES];
static unsigned char wtls_seed[WTLS_SEED_BYTES];
static int wtls_active;
static int wtls_closing;
static int wtls_error;

static void wtls_proxy_start_chain(const br_x509_class **ctx, const char *server_name);
static void wtls_proxy_start_cert(const br_x509_class **ctx, uint32_t length);
static void wtls_proxy_append(const br_x509_class **ctx, const unsigned char *data, size_t length);
static void wtls_proxy_end_cert(const br_x509_class **ctx);
static unsigned wtls_proxy_end_chain(const br_x509_class **ctx);
static const br_x509_pkey *wtls_proxy_get_pkey(
    const br_x509_class *const *ctx, unsigned *usages);

static const br_x509_class wtls_proxy_vtable = {
    sizeof(wtls_x509_proxy),
    wtls_proxy_start_chain,
    wtls_proxy_start_cert,
    wtls_proxy_append,
    wtls_proxy_end_cert,
    wtls_proxy_end_chain,
    wtls_proxy_get_pkey
};

static const br_x509_class **wtls_inner_context(wtls_x509_proxy *proxy)
{
    return (const br_x509_class **)(void *)proxy->inner;
}

static void wtls_proxy_start_chain(const br_x509_class **ctx, const char *server_name)
{
    wtls_x509_proxy *proxy = (wtls_x509_proxy *)(void *)ctx;
    const br_x509_class **inner;
    proxy->vtable = &wtls_proxy_vtable;
    proxy->leaf_seen = 0;
    proxy->leaf_current = 0;
    proxy->leaf_complete = 0;
    proxy->leaf_expected = proxy->leaf_used = 0;
    inner = wtls_inner_context(proxy);
    proxy->inner->vtable->start_chain(inner, proxy->ip_mode ? NULL : server_name);
}

static void wtls_proxy_start_cert(const br_x509_class **ctx, uint32_t length)
{
    wtls_x509_proxy *proxy = (wtls_x509_proxy *)(void *)ctx;
    const br_x509_class **inner = wtls_inner_context(proxy);
    proxy->leaf_current = !proxy->leaf_seen;
    if (proxy->leaf_current) {
        proxy->leaf_seen = 1;
        proxy->leaf_expected = length;
        proxy->leaf_used = 0;
        proxy->leaf_complete = length != 0 && length <= WTLS_LEAF_CERT_MAX;
    }
    proxy->inner->vtable->start_cert(inner, length);
}

static void wtls_proxy_append(const br_x509_class **ctx,
    const unsigned char *data, size_t length)
{
    wtls_x509_proxy *proxy = (wtls_x509_proxy *)(void *)ctx;
    const br_x509_class **inner = wtls_inner_context(proxy);
    if (proxy->leaf_current) {
        if (length > WTLS_LEAF_CERT_MAX - proxy->leaf_used) {
            proxy->leaf_complete = 0;
        } else {
            memcpy(proxy->leaf_certificate + proxy->leaf_used, data, length);
            proxy->leaf_used += (uint32_t)length;
        }
    }
    proxy->inner->vtable->append(inner, data, length);
}

static void wtls_proxy_end_cert(const br_x509_class **ctx)
{
    wtls_x509_proxy *proxy = (wtls_x509_proxy *)(void *)ctx;
    const br_x509_class **inner = wtls_inner_context(proxy);
    proxy->inner->vtable->end_cert(inner);
    if (proxy->leaf_current) {
        if (proxy->leaf_used != proxy->leaf_expected) proxy->leaf_complete = 0;
        proxy->leaf_current = 0;
    }
}

static unsigned wtls_proxy_end_chain(const br_x509_class **ctx)
{
    wtls_x509_proxy *proxy = (wtls_x509_proxy *)(void *)ctx;
    const br_x509_class **inner = wtls_inner_context(proxy);
    unsigned status = proxy->inner->vtable->end_chain(inner);
    if (status != 0) return status;
    if (proxy->ip_mode && (!proxy->leaf_seen || !proxy->leaf_complete ||
        !wx509_has_ipv4_san(proxy->leaf_certificate, proxy->leaf_used,
            proxy->ip_address))) return BR_ERR_X509_BAD_SERVER_NAME;
    return 0;
}

static const br_x509_pkey *wtls_proxy_get_pkey(
    const br_x509_class *const *ctx, unsigned *usages)
{
    wtls_x509_proxy *proxy = (wtls_x509_proxy *)(void *)ctx;
    const br_x509_class *const *inner =
        (const br_x509_class *const *)(const void *)proxy->inner;
    return proxy->inner->vtable->get_pkey(inner, usages);
}

static int wtls_parse_ipv4(const char *host, unsigned char address[4])
{
    unsigned octet;
    const char *p = host;
    for (octet = 0; octet < 4; ++octet) {
        unsigned value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10u + (unsigned)(*p++ - '0');
            if (value > 255u || ++digits > 3u) return 0;
        }
        if (digits == 0) return 0;
        address[octet] = (unsigned char)value;
        if (octet < 3) {
            if (*p++ != '.') return 0;
        } else if (*p != 0) return 0;
    }
    return 1;
}

static int wtls_fail(int error)
{
    wtls_error = error ? error : BR_ERR_BAD_STATE;
    return WTLS_FAILED;
}

static int wtls_state(void)
{
    if (!wtls_active) return WTLS_FAILED;
    if (br_ssl_engine_last_error(&wtls_client.eng) != BR_ERR_OK)
        return wtls_fail(br_ssl_engine_last_error(&wtls_client.eng));
    if (br_ssl_engine_current_state(&wtls_client.eng) & BR_SSL_CLOSED)
        return WTLS_CLOSED;
    return WTLS_OK;
}

int wtls_init(const char *host, uint32_t unix_utc,
    wtls_entropy_fn entropy, void *opaque,
    const br_x509_trust_anchor *anchors, size_t anchor_count)
{
    size_t host_length;
    uint32_t day_count, seconds;
    int ip_mode;

    wtls_active = 0;
    wtls_closing = 0;
    wtls_error = BR_ERR_OK;
    if (host == NULL || unix_utc == 0 || entropy == NULL ||
        anchors == NULL || anchor_count == 0) return wtls_fail(BR_ERR_BAD_PARAM);
    host_length = strlen(host);
    if (host_length == 0 || host_length > WTLS_HOST_MAX)
        return wtls_fail(BR_ERR_BAD_PARAM);
    ip_mode = wtls_parse_ipv4(host, wtls_x509_check.ip_address);

    memset(&wtls_client, 0, sizeof wtls_client);
    memset(&wtls_x509, 0, sizeof wtls_x509);
    br_ssl_client_init_full(&wtls_client, &wtls_x509, anchors, anchor_count);
    wtls_x509_check.vtable = &wtls_proxy_vtable;
    wtls_x509_check.inner = &wtls_x509;
    wtls_x509_check.ip_mode = ip_mode;
    br_ssl_engine_set_x509(&wtls_client.eng, &wtls_x509_check.vtable);
    br_ssl_engine_set_versions(&wtls_client.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_add_flags(&wtls_client.eng, BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffers_bidi(&wtls_client.eng,
        wtls_input, sizeof wtls_input, wtls_output, sizeof wtls_output);

    day_count = unix_utc / 86400UL + WTLS_UNIX_EPOCH_DAYS;
    seconds = unix_utc % 86400UL;
    br_x509_minimal_set_time(&wtls_x509, day_count, seconds);

    /* BearSSL's RNG must be seeded before client_reset() starts the handshake.
     * The callback is a cryptographic source and must fail if unavailable. */
    memset(wtls_seed, 0, sizeof wtls_seed);
    if (!entropy(opaque, wtls_seed, sizeof wtls_seed)) {
        memset(wtls_seed, 0, sizeof wtls_seed);
        return wtls_fail(BR_ERR_NO_RANDOM);
    }
    br_ssl_engine_inject_entropy(&wtls_client.eng,
        wtls_seed, sizeof wtls_seed);
    memset(wtls_seed, 0, sizeof wtls_seed);

    if (!br_ssl_client_reset(&wtls_client, ip_mode ? NULL : host, 0))
        return wtls_fail(br_ssl_engine_last_error(&wtls_client.eng));
    wtls_active = 1;
    return WTLS_OK;
}

int wtls_step(const void *cipher_input, uint32_t input_bytes,
    uint32_t *consumed)
{
    const unsigned char *src = (const unsigned char *)cipher_input;
    uint32_t done = 0;
    int status;

    if (consumed != NULL) *consumed = 0;
    if (!wtls_active || (input_bytes != 0 && src == NULL) || consumed == NULL)
        return wtls_fail(BR_ERR_BAD_PARAM);
    status = wtls_state();
    if (status != WTLS_OK) return status;

    while (done < input_bytes) {
        size_t available = 0;
        unsigned state = br_ssl_engine_current_state(&wtls_client.eng);
        unsigned char *dst;
        size_t amount;

        if ((state & BR_SSL_RECVREC) == 0) break;
        dst = br_ssl_engine_recvrec_buf(&wtls_client.eng, &available);
        if (dst == NULL || available == 0) break;
        amount = (size_t)(input_bytes - done);
        if (amount > available) amount = available;
        memcpy(dst, src + done, amount);
        br_ssl_engine_recvrec_ack(&wtls_client.eng, amount);
        done += (uint32_t)amount;
        status = wtls_state();
        if (status != WTLS_OK) break;
    }
    *consumed = done;
    if (status < 0) return status;
    if (done != 0) return WTLS_OK;
    if (status == WTLS_CLOSED) return status;
    return done == input_bytes ? WTLS_OK : WTLS_AGAIN;
}

int wtls_pull_record(void *output, uint32_t capacity, uint32_t *produced)
{
    unsigned char *dst = (unsigned char *)output;
    unsigned char *src;
    size_t available = 0;
    size_t amount;
    int status;

    if (produced != NULL) *produced = 0;
    if (!wtls_active || produced == NULL || (capacity != 0 && dst == NULL))
        return wtls_fail(BR_ERR_BAD_PARAM);
    status = wtls_state();
    if (status < 0) return status;
    src = br_ssl_engine_sendrec_buf(&wtls_client.eng, &available);
    if (src == NULL || available == 0 || capacity == 0)
        return status == WTLS_CLOSED ? status : WTLS_AGAIN;
    amount = available;
    if (amount > capacity) amount = capacity;
    memcpy(dst, src, amount);
    br_ssl_engine_sendrec_ack(&wtls_client.eng, amount);
    *produced = (uint32_t)amount;
    status = wtls_state();
    return status < 0 ? status : WTLS_OK;
}

int wtls_send_plaintext(const void *input, uint32_t input_bytes,
    uint32_t *consumed)
{
    const unsigned char *src = (const unsigned char *)input;
    unsigned char *dst;
    size_t available = 0;
    size_t amount;
    int status;

    if (consumed != NULL) *consumed = 0;
    if (!wtls_active || consumed == NULL || (input_bytes != 0 && src == NULL))
        return wtls_fail(BR_ERR_BAD_PARAM);
    status = wtls_state();
    if (status != WTLS_OK) return status;
    if (input_bytes == 0) return WTLS_OK;

    dst = br_ssl_engine_sendapp_buf(&wtls_client.eng, &available);
    if (dst == NULL || available == 0) return WTLS_AGAIN;
    amount = input_bytes;
    if (amount > available) amount = available;
    memcpy(dst, src, amount);
    br_ssl_engine_sendapp_ack(&wtls_client.eng, amount);
    br_ssl_engine_flush(&wtls_client.eng, 0);
    *consumed = (uint32_t)amount;
    status = wtls_state();
    return status < 0 ? status : WTLS_OK;
}

int wtls_pull_plaintext(void *output, uint32_t capacity, uint32_t *produced)
{
    unsigned char *dst = (unsigned char *)output;
    unsigned char *src;
    size_t available = 0;
    size_t amount;
    int status;

    if (produced != NULL) *produced = 0;
    if (!wtls_active || produced == NULL || (capacity != 0 && dst == NULL))
        return wtls_fail(BR_ERR_BAD_PARAM);
    status = wtls_state();
    if (status < 0) return status;
    src = br_ssl_engine_recvapp_buf(&wtls_client.eng, &available);
    if (src == NULL || available == 0 || capacity == 0)
        return status == WTLS_CLOSED ? status : WTLS_AGAIN;
    amount = available;
    if (amount > capacity) amount = capacity;
    memcpy(dst, src, amount);
    br_ssl_engine_recvapp_ack(&wtls_client.eng, amount);
    *produced = (uint32_t)amount;
    status = wtls_state();
    return status < 0 ? status : WTLS_OK;
}

int wtls_close(void)
{
    int status;

    status = wtls_state();
    if (status < 0 || status == WTLS_CLOSED) return status;
    if (!wtls_closing) {
        wtls_closing = 1;
        br_ssl_engine_close(&wtls_client.eng);
    }
    return wtls_state();
}

int wtls_ready(void)
{
    int status = wtls_state();
    if (status != WTLS_OK) return status;
    return (br_ssl_engine_current_state(&wtls_client.eng) & BR_SSL_SENDAPP) != 0;
}

int wtls_last_error(void)
{
    if (wtls_error != BR_ERR_OK) return wtls_error;
    if (wtls_active) return br_ssl_engine_last_error(&wtls_client.eng);
    return BR_ERR_BAD_STATE;
}
