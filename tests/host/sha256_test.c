/* FIPS 180-4 known-answer vectors. SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <string.h>
#include <ciuki/sha256.h>

static unsigned failures;
static void check(const uint8_t digest[32], const char *expected)
{
    char hex[65];
    sha256_hex(digest, hex);
    if (strcmp(hex, expected)) {
        printf("sha256 mismatch: %s expected %s\n", hex, expected);
        failures++;
    }
}

int main(void)
{
    uint8_t digest[32];
    sha256("", 0, digest);
    check(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha256("abc", 3, digest);
    check(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    struct sha256_ctx c;
    sha256_init(&c);
    char a[1000];
    memset(a, 'a', sizeof(a));
    for (unsigned i = 0; i < 1000; i++)
        sha256_update(&c, a, sizeof(a));
    sha256_final(&c, digest);
    check(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    /* 56-byte message exercises padding in a second block. */
    const char *s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_init(&c);
    for (size_t i = 0; i < strlen(s); i++)
        sha256_update(&c, s + i, 1);
    sha256_final(&c, digest);
    check(digest, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    printf("sha256 vectors (empty, abc, million-a, padding/streaming): %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
