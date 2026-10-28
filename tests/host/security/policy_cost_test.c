/* sec-k08 H13 (tests/host/security/test_policy_coverage.py): measured sizes
 * for the Registry Checker per-user hive audit (lead decision S2-4). The
 * production w32srv registry (apps/w32srv) builds the machine hive and the
 * default user template with wr_defaults, then a new profile's hive exactly
 * as wr_select_profile does at a first logon; the hive files written are
 * measured. The archive bounds are Registry Checker's own (recovery.h).
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../../../apps/w32srv/registry.h"
#include "../../../apps/sysrecovery/recovery.h"
#include "accounts.h"
#include <ciuki/pbkdf2.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { char path[160]; size_t size; } files[16];

static unsigned slot(const char *name)
{
    for (unsigned i = 0; i < 16; i++) if (!strcmp(files[i].path, name)) return i;
    for (unsigned i = 0; i < 16; i++) if (!files[i].path[0]) { snprintf(files[i].path, sizeof(files[i].path), "%s", name); return i; }
    assert(0); return 0;
}
static struct { char path[160]; uint8_t *bytes; size_t size; } store[16];
static int io_read(void *p, const char *name, uint8_t **out, size_t *n)
{
    (void)p;
    for (unsigned i = 0; i < 16; i++) if (store[i].bytes && !strcmp(store[i].path, name)) {
        *out = malloc(store[i].size); assert(*out); memcpy(*out, store[i].bytes, store[i].size); *n = store[i].size; return 1;
    }
    return 0;
}
static int io_write(void *p, const char *name, const void *v, size_t n)
{
    (void)p; unsigned i = slot(name); files[i].size = n;
    free(store[i].bytes); store[i].bytes = malloc(n ? n : 1); assert(store[i].bytes);
    memcpy(store[i].bytes, v, n); store[i].size = n; snprintf(store[i].path, sizeof(store[i].path), "%s", name);
    return 0;
}
static int io_patch(void *p, const char *name, size_t at, const void *v, size_t n)
{
    (void)p; unsigned i = slot(name); assert(at + n <= store[i].size); memcpy(store[i].bytes + at, v, n); return 0;
}
static int io_sync(void *p, const char *name) { (void)p; (void)name; return 0; }
static void io_log(void *p, const char *name) { (void)p; (void)name; }

/* The self-test's security.kdf derivation (src/kernel/lib/pbkdf2.c) is the
 * ACCOUNT service's acct_derive byte for byte, and PBKDF2-HMAC-SHA-256 by
 * the published test vectors (RFC 7914 section 11 / RFC 6070 inputs). */
static void kdf_equivalence(void)
{
    static const struct { const char *password, *salt; uint32_t count; const char *hex; } vectors[] = {
        {"password", "salt", 1, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"},
        {"password", "salt", 2, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"},
        {"password", "salt", 4096, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"}};
    for (unsigned i = 0; i < 3; i++) {
        uint8_t out[32]; char hex[65];
        assert(!pbkdf2_hmac_sha256(vectors[i].password, strlen(vectors[i].password), vectors[i].salt, strlen(vectors[i].salt),
                                   vectors[i].count, out));
        for (unsigned j = 0; j < 32; j++) snprintf(hex + 2 * j, 3, "%02x", out[j]);
        assert(!strcmp(hex, vectors[i].hex));
    }
    assert(CIUKI_ACCOUNT_KDF_ITERATIONS == ACCT_ITERATIONS);
    static const char *const passwords[] = {"", "password1", "a much longer passphrase of 63 printable characters, no more..."};
    for (unsigned i = 0; i < 3; i++) for (unsigned k = 0; k < 2; k++) {
        uint8_t salt[16], a[32], b[32];
        for (unsigned j = 0; j < 16; j++) salt[j] = (uint8_t)(17 * j + 31 * i + 101 * k);
        assert(!acct_derive(passwords[i], salt, ACCT_ITERATIONS, a));
        assert(!pbkdf2_hmac_sha256(passwords[i], strlen(passwords[i]), salt, 16, CIUKI_ACCOUNT_KDF_ITERATIONS, b));
        assert(!memcmp(a, b, 32));
    }
    printf("H13 kdf: kernel self-test PBKDF2-HMAC-SHA-256 matches the RFC vectors and the ACCOUNT service's acct_derive at %u iterations\n",
           ACCT_ITERATIONS);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct wr_server *s = calloc(1, sizeof(*s)); assert(s);
    struct wr_io io = {NULL, io_read, io_write, io_patch, io_sync, io_log};
    assert(wr_init_secure(s, &io, 0));   /* the production broker's mode */
    assert(wr_defaults(s, 1024, 768, 32));
    assert(!wr_flush(s));
    size_t machine = 0, template_hive = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (strstr(files[i].path, "SYSTEM.CRG")) machine = files[i].size > machine ? files[i].size : machine;
        if (strstr(files[i].path, "USER.CRG") && !strstr(files[i].path, "/Windows/Profiles/")) template_hive = files[i].size;
    }
    assert(!wr_select_profile(s, "alice", "/Windows/Profiles/u000003ef"));
    assert(!wr_flush(s));
    size_t profile = 0;
    for (unsigned i = 0; i < 16; i++)
        if (strstr(files[i].path, "/Windows/Profiles/u000003ef/") && strstr(files[i].path, "USER.CRG")) profile = files[i].size;
    for (unsigned i = 0; i < 16; i++) if (files[i].path[0]) printf("H13 cost hive file %s %zu bytes\n", files[i].path, files[i].size);
    assert(machine && profile);
    printf("H13 cost hives: machine hive %zu bytes, default user template %zu bytes, new profile hive %zu bytes; "
           "hive text bound W32R_TEXT %u bytes; Registry Checker archive bound %u bytes, %u files\n",
           machine, template_hive, profile, W32R_TEXT, RC_MAX_BYTES, RC_MAX_FILES);
    wr_destroy(s); free(s);
    for (unsigned i = 0; i < 16; i++) free(store[i].bytes);
    kdf_equivalence();
    return 0;
}
