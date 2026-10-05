#include "worker_ca.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WCA_MAX_PEM_BYTES   (16UL * 1024UL * 1024UL)
#define WCA_MAX_CERT_BYTES  (32UL * 1024UL)
#define WCA_MAX_DN_BYTES    (8192UL)
#define WCA_MAX_ANCHORS     256u

struct wca_dn_sink {
    unsigned char *data;
    size_t length;
    size_t capacity;
    int failed;
};

struct wca_der_sink {
    unsigned char *data;
    size_t length;
    int certificate;
    int overflow;
};

static void wca_append_dn(void *opaque, const void *data, size_t length)
{
    struct wca_dn_sink *sink = (struct wca_dn_sink *)opaque;
    size_t capacity;
    unsigned char *p;

    if (sink->failed || length == 0) return;
    if (length > WCA_MAX_DN_BYTES - sink->length) {
        sink->failed = 1;
        return;
    }
    if (sink->length + length > sink->capacity) {
        capacity = sink->capacity ? sink->capacity : 128u;
        while (capacity < sink->length + length) {
            if (capacity > WCA_MAX_DN_BYTES / 2u) {
                capacity = WCA_MAX_DN_BYTES;
                break;
            }
            capacity *= 2u;
        }
        p = (unsigned char *)realloc(sink->data, capacity);
        if (p == NULL) {
            sink->failed = 1;
            return;
        }
        sink->data = p;
        sink->capacity = capacity;
    }
    memcpy(sink->data + sink->length, data, length);
    sink->length += length;
}

static void wca_append_der(void *opaque, const void *data, size_t length)
{
    struct wca_der_sink *sink = (struct wca_der_sink *)opaque;

    if (!sink->certificate || sink->overflow || length == 0) return;
    if (length > WCA_MAX_CERT_BYTES - sink->length) {
        sink->overflow = 1;
        return;
    }
    memcpy(sink->data + sink->length, data, length);
    sink->length += length;
}

static int wca_decode_ca(wca_store *store, const unsigned char *der,
    size_t der_length)
{
    br_x509_decoder_context *decoder;
    br_x509_pkey *key;
    struct wca_dn_sink dn;
    size_t index;

    if (der_length == 0 || der_length > WCA_MAX_CERT_BYTES ||
        store->count >= WCA_MAX_ANCHORS) return -1;
    index = store->count;
    decoder = &store->decoders[index];
    memset(&dn, 0, sizeof dn);
    br_x509_decoder_init(decoder, wca_append_dn, &dn);
    br_x509_decoder_push(decoder, der, der_length);
    key = br_x509_decoder_get_pkey(decoder);
    if (br_x509_decoder_last_error(decoder) != 0 || key == NULL ||
        !br_x509_decoder_isCA(decoder) || dn.failed || dn.length == 0 ||
        (key->key_type != BR_KEYTYPE_RSA && key->key_type != BR_KEYTYPE_EC)) {
        free(dn.data);
        return -1;
    }

    store->names[index] = dn.data;
    store->anchors[index].dn.data = dn.data;
    store->anchors[index].dn.len = dn.length;
    store->anchors[index].flags = BR_X509_TA_CA;
    store->anchors[index].pkey = *key;
    store->count++;
    return 0;
}

void wca_free(wca_store *store)
{
    size_t i;

    if (store == NULL) return;
    if (store->names != NULL) {
        for (i = 0; i < WCA_MAX_ANCHORS; i++) free(store->names[i]);
    }
    free(store->names);
    free(store->decoders);
    free(store->anchors);
    memset(store, 0, sizeof *store);
}

int wca_load_pem(const char *path, wca_store *store)
{
    FILE *file;
    long file_length;
    unsigned char *pem_bytes = NULL;
    unsigned char *der_bytes = NULL;
    size_t offset, used;
    br_pem_decoder_context pem;
    struct wca_der_sink der;
    int event, rc = -1;

    if (path == NULL || store == NULL) return -1;
    memset(store, 0, sizeof *store);
    file = fopen(path, "rb");
    if (file == NULL) return -1;
    if (fseek(file, 0, SEEK_END) != 0 ||
        (file_length = ftell(file)) <= 0 ||
        (unsigned long)file_length > WCA_MAX_PEM_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) goto done;

    pem_bytes = (unsigned char *)malloc((size_t)file_length);
    der_bytes = (unsigned char *)malloc(WCA_MAX_CERT_BYTES);
    store->anchors = (br_x509_trust_anchor *)calloc(WCA_MAX_ANCHORS,
        sizeof *store->anchors);
    store->decoders = (br_x509_decoder_context *)calloc(WCA_MAX_ANCHORS,
        sizeof *store->decoders);
    store->names = (unsigned char **)calloc(WCA_MAX_ANCHORS,
        sizeof *store->names);
    if (pem_bytes == NULL || der_bytes == NULL || store->anchors == NULL ||
        store->decoders == NULL || store->names == NULL) goto done;
    if (fread(pem_bytes, 1, (size_t)file_length, file) != (size_t)file_length)
        goto done;

    memset(&der, 0, sizeof der);
    der.data = der_bytes;
    br_pem_decoder_init(&pem);
    br_pem_decoder_setdest(&pem, NULL, NULL);
    offset = 0;
    while (offset < (size_t)file_length) {
        used = br_pem_decoder_push(&pem, pem_bytes + offset,
            (size_t)file_length - offset);
        offset += used;
        event = br_pem_decoder_event(&pem);
        if (event == BR_PEM_BEGIN_OBJ) {
            const char *name = br_pem_decoder_name(&pem);
            der.length = 0;
            der.overflow = 0;
            der.certificate = strcmp(name, "CERTIFICATE") == 0;
            br_pem_decoder_setdest(&pem,
                der.certificate ? wca_append_der : NULL, &der);
        } else if (event == BR_PEM_END_OBJ) {
            if (der.certificate && (der.overflow ||
                wca_decode_ca(store, der.data, der.length) != 0)) goto done;
            der.certificate = 0;
            br_pem_decoder_setdest(&pem, NULL, NULL);
        } else if (event == BR_PEM_ERROR) {
            goto done;
        } else if (used == 0) {
            goto done;
        }
    }
    if (der.certificate || store->count == 0) goto done;
    rc = 0;

done:
    fclose(file);
    free(pem_bytes);
    free(der_bytes);
    if (rc != 0) wca_free(store);
    return rc;
}
