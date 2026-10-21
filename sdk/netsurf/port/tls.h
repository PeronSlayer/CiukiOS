/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_TLS_H
#define CIUKI_TLS_H
#include <bearssl.h>
#include <stddef.h>
#include <stdbool.h>
enum ciuki_tls_error { CIUKI_TLS_OK,CIUKI_TLS_MEMORY,CIUKI_TLS_CLOCK,CIUKI_TLS_ENTROPY,
    CIUKI_TLS_HOST,CIUKI_TLS_CERT,CIUKI_TLS_DATE,CIUKI_TLS_PROTOCOL,CIUKI_TLS_IO,CIUKI_TLS_EOF };
struct ciuki_tls_cert { size_t offset,bytes; };
struct ciuki_tls;
bool ciuki_tls_clock(uint32_t *,uint32_t *);
struct ciuki_tls *ciuki_tls_create(const char *,const br_x509_trust_anchor *,size_t,enum ciuki_tls_error *);
void ciuki_tls_free(struct ciuki_tls *);
/* Nonblocking record movement: at most *budget bytes, no waits. */
enum ciuki_tls_error ciuki_tls_pump(struct ciuki_tls *,int,size_t *);
unsigned ciuki_tls_state(struct ciuki_tls *);
const unsigned char *ciuki_tls_read(struct ciuki_tls *,size_t *);
void ciuki_tls_read_ack(struct ciuki_tls *,size_t);
unsigned char *ciuki_tls_write(struct ciuki_tls *,size_t *);
void ciuki_tls_write_ack(struct ciuki_tls *,size_t);
void ciuki_tls_flush(struct ciuki_tls *);
unsigned ciuki_tls_certificates(struct ciuki_tls *,const unsigned char **,const struct ciuki_tls_cert **);
const char *ciuki_tls_message(enum ciuki_tls_error);
#endif
