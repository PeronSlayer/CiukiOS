/* Cooperative IPv4/DNS/TCP/HTTP client over CiukiOS INT 61 native packet API.
 * One active fetch; every call performs bounded work and returns immediately. */
#ifndef CIUKIOS_WEBNET_H
#define CIUKIOS_WEBNET_H

#include "app.h"

#define WEBNET_IDLE       0
#define WEBNET_PENDING    1
#define WEBNET_COMPLETE   2
#define WEBNET_FAILED     3

int webnet_start(const char *url, void *response, int capacity);
/* Streaming HTTP/1.x body API over HTTP or HTTPS. The callback runs
 * cooperatively from webnet_poll() with decoded entity bytes, in chunks
 * <=1024. Return 1 to keep receiving or 0 to abort. Do not retain the
 * callback's temporary chunk; accepted data is valid only on completion. */
typedef int (*webnet_sink_fn)(const u8 *chunk, u16 length);
/* max_body_bytes must be 1..4 MiB; over-limit bodies fail the transfer. */
int webnet_start_sink(const char *url, webnet_sink_fn sink, u32 max_body_bytes);
int webnet_poll(void);
void webnet_cancel(void);
int webnet_read(void);
int webnet_was_truncated(void);
int webnet_redirect(char *target, int capacity);
int webnet_redirect_target_supported(const char *target);
const char *webnet_content_type(void);
/* Resolve an HTTP or HTTPS URL reference according to RFC 3986 §5.2. */
int webnet_resolve_url(const char *base, const char *reference, char *out, u16 capacity);
u32 webnet_wire_bytes(void);
const char *webnet_error(void);
int webnet_http_status(void);

#endif
