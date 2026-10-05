#ifndef CIUK_WORKER_X509_IP_H
#define CIUK_WORKER_X509_IP_H

#include <stddef.h>

/* True only when a well-formed leaf certificate contains an exact IPv4
 * subjectAltName iPAddress value. DNS names and Common Name are never used. */
int wx509_has_ipv4_san(const unsigned char *certificate, size_t length,
    const unsigned char address[4]);

#endif
