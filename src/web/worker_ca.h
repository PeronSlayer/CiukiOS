#ifndef CIUK_WORKER_CA_H
#define CIUK_WORKER_CA_H

#include <stddef.h>
#include "bearssl.h"

/* Runtime PEM trust store. The decoded key pointers remain valid until free. */
typedef struct wca_store {
    br_x509_trust_anchor *anchors;
    br_x509_decoder_context *decoders;
    unsigned char **names;
    size_t count;
} wca_store;

/* Loads only valid CA certificates from a PEM bundle. Fails closed on a
 * malformed PEM stream, resource bound, unreadable file, or empty store. */
int wca_load_pem(const char *path, wca_store *store);
void wca_free(wca_store *store);

#endif
