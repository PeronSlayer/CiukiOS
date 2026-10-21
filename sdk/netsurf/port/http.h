/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_HTTP_H
#define CIUKI_HTTP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zlib.h>
#define CIUKI_HTTP_URL 4096u
#define CIUKI_HTTP_HEADERS 32768u
#define CIUKI_HTTP_LINE 8192u
#define CIUKI_HTTP_FIELDS 128u
#define CIUKI_HTTP_BODY (8u*1024u*1024u)
#define CIUKI_HTTP_SLICE 16384u
#define CIUKI_HTTP_POST 65536u
enum ciuki_http_event { CIUKI_HTTP_HEADER, CIUKI_HTTP_HEAD, CIUKI_HTTP_DATA };
/* False cancels parsing immediately, including from a header callback. */
typedef bool (*ciuki_http_callback)(void *,enum ciuki_http_event,const void *,size_t);
struct ciuki_http {
    unsigned state,status,fields,interims,crlf;
    size_t line_bytes,header_bytes,wire_bytes,body_bytes,remaining;
    bool length,transfer,close,http10,head,done,cancelled,zactive,zend,zpending;
    unsigned coding;
    const char *error;
    char line[CIUKI_HTTP_LINE+3];
    char location[CIUKI_HTTP_URL+1];
    z_stream z;
    unsigned char pending[CIUKI_HTTP_SLICE];
    unsigned char output[CIUKI_HTTP_SLICE];
    size_t pending_bytes;
    ciuki_http_callback callback;
    void *context;
};
void ciuki_http_init(struct ciuki_http *,bool,ciuki_http_callback,void *);
void ciuki_http_destroy(struct ciuki_http *);
bool ciuki_http_feed(struct ciuki_http *,const void *,size_t);
/* Drain before another feed; each feed/drain emits at most one 16 KiB slice. */
bool ciuki_http_drain(struct ciuki_http *);
bool ciuki_http_pending(const struct ciuki_http *);
bool ciuki_http_eof(struct ciuki_http *);
bool ciuki_http_reusable(const struct ciuki_http *);
/* Rejects request smuggling and unsupported caller-controlled protocol headers. */
bool ciuki_http_field(const char *,size_t,bool request);
#endif
