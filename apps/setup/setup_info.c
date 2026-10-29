/* SPDX-License-Identifier: GPL-2.0-only */
#include "setup_info.h"
#include <stdio.h>
#include <string.h>

/* RELEASE.JSON is written by scripts/build_cd.py (json.dumps, sorted
 * keys): "key": "string" or "key": integer. Keys are matched exactly. */
static const char *value(const char *json, size_t length, const char *key)
{
    size_t k = strlen(key);
    for (size_t i = 0; i + k + 3 <= length; i++) {
        if (json[i] != '"' || memcmp(json + i + 1, key, k) || json[i + 1 + k] != '"') continue;
        size_t at = i + k + 2;
        while (at < length && (json[at] == ' ' || json[at] == ':')) at++;
        return at < length ? json + at : NULL;
    }
    return NULL;
}
static int string(const char *json, size_t length, const char *key, char *out, size_t size, size_t exact)
{
    const char *v = value(json, length, key);
    if (!v || *v != '"') return -1;
    size_t n = 0;
    for (v++; v < json + length && *v != '"'; v++) {
        if (*v == '\\' || (unsigned char)*v < 32 || n + 1 >= size) return -1;
        out[n++] = *v;
    }
    if (v >= json + length || (exact && n != exact)) return -1;
    out[n] = 0;
    return 0;
}
static int hex(const char *s)
{
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return -1;
    return 0;
}
int setup_release_parse(const char *json, size_t length, struct setup_release *out)
{
    memset(out, 0, sizeof(*out));
    if (string(json, length, "content_digest", out->content_digest, sizeof(out->content_digest), 64) ||
        string(json, length, "disk_image_sha256", out->disk_image_sha256, sizeof(out->disk_image_sha256), 64) ||
        string(json, length, "version", out->version, sizeof(out->version), 0) ||
        hex(out->content_digest) || hex(out->disk_image_sha256)) return -1;
    /* Older media can omit these additive presentation fields. */
    if (value(json, length, "version_full") && string(json, length, "version_full", out->version_full, sizeof(out->version_full), 0)) return -1;
    if (value(json, length, "version_display") && string(json, length, "version_display", out->version_display, sizeof(out->version_display), 0)) return -1;
    const char *v = value(json, length, "payload_files");
    if (!v || *v < '0' || *v > '9') return -1;
    unsigned long n = 0;
    for (; v < json + length && *v >= '0' && *v <= '9'; v++) {
        if (n > 100000000ul) return -1;
        n = n * 10 + (unsigned long)(*v - '0');
    }
    out->payload_files = n;
    return 0;
}
int setup_media_record(char *out, size_t size, const struct setup_release *r, unsigned long counted)
{
    int n = counted == r->payload_files ?
        snprintf(out, size, "[setup] media ok iso=%s image=%s payload=%lu mode=placeholder\n",
                 r->content_digest, r->disk_image_sha256, r->payload_files) :
        snprintf(out, size, "[setup] media bad iso=%s image=%s payload=%lu found=%lu mode=placeholder\n",
                 r->content_digest, r->disk_image_sha256, r->payload_files, counted);
    return n < 0 || (size_t)n >= size ? -1 : n;
}
