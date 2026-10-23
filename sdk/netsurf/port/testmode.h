/* f6-18 test profile of the browser (QEMU f6-net browser cases).
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_TESTMODE_H
#define CIUKI_TESTMODE_H
#include <bearssl.h>
#include <stddef.h>
#include <stdbool.h>
/* The fixture CA anchors, only while the test profile is active (the kernel
 * test request read through probe call 5 starts with "f6:browser run=");
 * NULL and 0 otherwise, which keeps the production bundle. */
const br_x509_trust_anchor *ciuki_test_anchors(size_t *count);
bool ciuki_test_active(void);
/* The fetcher reports every terminal http/https fetch: its URL, HTTP status
 * (0 when no response arrived), decoded body bytes delivered, the job's TLS
 * verdict (enum ciuki_tls_error, -1 for a plain-HTTP fetch) and the error
 * text (only its presence is kept). fx-11c: the URL lets the report pick the
 * shown document's fetch, not a favicon's or an earlier redirect hop's. */
void ciuki_test_fetch_observe(const char *url,unsigned status,size_t bytes,int tls_error,const char *error);
#endif
