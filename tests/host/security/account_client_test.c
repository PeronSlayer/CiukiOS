/* H12 (sec-user-04): the Control Panel's account client encodes exactly what
 * the production account service decodes (apps/accounts/protocol.c): every
 * management kind round-trips through acct_body_decode, and the client's
 * operation digest equals acct_management_digest, which RESERVE binds.
 * SPDX-License-Identifier: MIT */
#include "accounts.h"
#include "accounts_client.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static int round_trip(uint32_t wire, uint32_t flags, uint32_t uid, const char *name, const char *old, const char *pw,
                      const char *picture, unsigned expected_operation)
{
    uint8_t body[312], digest[32], service_digest[32];
    CHECK(!cpa_management_encode(wire, flags, uid, 77, name, old, pw, picture, 0x1234, body, digest));
    struct acct_request q;
    int e = acct_body_decode(2, wire, body, sizeof(body), 77, &q);
    if (e) { fprintf(stderr, "decode wire=%u flags=%#x: %d\n", wire, flags, e); return 1; }
    CHECK(q.operation == expected_operation && q.uid == uid && q.generation == 77 && q.client_operation_id == 0x1234);
    CHECK(!strcmp(q.name, name) && !strcmp(q.password, pw) && !strcmp(q.picture, picture));
    if (wire == 2) CHECK(!strcmp(q.old_password, old) && q.self_password);
    CHECK(!acct_management_digest(&q, service_digest) && !memcmp(digest, service_digest, 32));
    /* A different generation or secret is a different operation. */
    uint8_t other[312], other_digest[32];
    CHECK(!cpa_management_encode(wire, flags, uid, 78, name, old, pw, picture, 0x1234, other, other_digest));
    CHECK(memcmp(digest, other_digest, 32));
    acct_wipe(&q, sizeof(q));
    return 0;
}

int main(void)
{
    if (round_trip(2, 0, 1000, "", "oldpassword", "newpassword", "", ACCT_PASSWORD)) return 1;          /* own password */
    if (round_trip(3, 1, 0, "Bob", "", "bobpass12", "", ACCT_CREATE)) return 1;                       /* Standard */
    if (round_trip(3, 3, 0, "Admin2", "", "adminpass1", "", ACCT_CREATE)) return 1;                   /* administrator */
    if (round_trip(4, 1024, 1001, "Robert", "", "", "", ACCT_UPDATE)) return 1;                        /* rename */
    if (round_trip(4, 512 | 2, 1001, "", "", "", "", ACCT_UPDATE)) return 1;                           /* type */
    if (round_trip(4, 256 | 1, 501, "", "", "", "", ACCT_UPDATE)) return 1;                            /* enable */
    if (round_trip(4, 2048, 501, "", "", "guestpass1", "", ACCT_UPDATE)) return 1;                    /* reset */
    if (round_trip(4, 4096, 1000, "", "", "", "tiles/dog.xrgb", ACCT_UPDATE)) return 1;               /* picture */
    if (round_trip(5, 0, 1001, "", "", "", "", ACCT_DELETE)) return 1;                                 /* delete */
    uint8_t body[312], digest[32];
    char long_name[40]; memset(long_name, 'a', sizeof(long_name) - 1); long_name[39] = 0;
    CHECK(cpa_management_encode(3, 1, 0, 1, long_name, "", "password1", "", 1, body, digest) == -EINVAL);
    /* Helpers: built-ins keep their name/type; nobody removes his own account. */
    struct cpa_view v; memset(&v, 0, sizeof(v)); v.valid = 1; v.self_uid = 1000; v.self_admin = 1; v.count = 3;
    v.records[0].uid = 500; v.records[1].uid = 501; v.records[2].uid = 1000;
    CHECK(cpa_allowed(&v, CPA_CREATE, NULL) && !cpa_allowed(&v, CPA_RENAME, &v.records[0]) && !cpa_allowed(&v, CPA_TYPE, &v.records[1]) &&
          !cpa_allowed(&v, CPA_DELETE, &v.records[2]) && !cpa_allowed(&v, CPA_ENABLE, &v.records[2]) &&
          cpa_allowed(&v, CPA_ENABLE, &v.records[1]) && cpa_allowed(&v, CPA_PASSWORD, &v.records[1]));
    v.self_admin = 0; v.self_uid = 501;
    CHECK(!cpa_allowed(&v, CPA_CREATE, NULL) && cpa_allowed(&v, CPA_PASSWORD, &v.records[1]) &&
          !cpa_allowed(&v, CPA_PASSWORD, &v.records[2]) && !cpa_allowed(&v, CPA_ENABLE, &v.records[1]));
    char secret[8] = "secret"; cpa_wipe(secret, sizeof(secret)); CHECK(!secret[0] && !secret[5]);
    printf("H12 account client codec: 9 management kinds decoded by the service, equal operation digests, bounds, "
           "role helpers PASS checks=%u\n", checks);
    return 0;
}
