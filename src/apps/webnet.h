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
int webnet_poll(void);
void webnet_cancel(void);
int webnet_read(void);
const char *webnet_error(void);
int webnet_http_status(void);

#endif
