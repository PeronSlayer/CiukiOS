/* SPDX-License-Identifier: MIT
 * Recent documents list; see recent.h. */
#include "recent.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int du_recent_file(char *out, size_t cap)
{
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') home = "/Documents";
    int n = snprintf(out, cap, "%s%s%s", home, home[strlen(home) - 1] == '/' ? "" : "/", DU_RECENT_FILE);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}
static int valid(const char *path, size_t n)
{
    if (!n || n >= DU_RECENT_PATH || path[0] != '/') return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)path[i] < 0x20 || path[i] == 0x7f) return 0;
    return 1;
}
void du_recent_parse(struct du_recent *r, const char *text, size_t length)
{
    r->count = 0;
    size_t at = 0;
    while (at < length && r->count < DU_RECENT_MAX) {
        size_t end = at;
        while (end < length && text[end] != '\n') end++;
        size_t n = end - at;
        if (n && text[at + n - 1] == '\r') n--;
        if (valid(text + at, n)) {
            int duplicate = 0;
            for (unsigned i = 0; i < r->count; i++)
                if (strlen(r->paths[i]) == n && !memcmp(r->paths[i], text + at, n)) duplicate = 1;
            if (!duplicate) { memcpy(r->paths[r->count], text + at, n); r->paths[r->count][n] = 0; r->count++; }
        }
        at = end + 1;
    }
}
int du_recent_push(struct du_recent *r, const char *path)
{
    size_t n = strlen(path);
    if (!valid(path, n)) return -1;
    unsigned at = r->count < DU_RECENT_MAX ? r->count : DU_RECENT_MAX - 1;
    for (unsigned i = 0; i < r->count; i++) if (!strcmp(r->paths[i], path)) { at = i; break; }
    if (at == r->count) r->count++;
    memmove(r->paths[1], r->paths[0], (size_t)at * sizeof(r->paths[0]));
    memcpy(r->paths[0], path, n + 1);
    return 0;
}
size_t du_recent_format(const struct du_recent *r, char *out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    out[0] = 0;
    for (unsigned i = 0; i < r->count; i++) {
        size_t len = strlen(r->paths[i]);
        if (n + len + 2 > cap) break;
        memcpy(out + n, r->paths[i], len); n += len; out[n++] = '\n'; out[n] = 0;
    }
    return n;
}
#define TEXT_CAP (DU_RECENT_MAX * (DU_RECENT_PATH + 1) + 1)   /* on the heap: the SDK limits stack frames to 4 KB */
int du_recent_load(struct du_recent *r)
{
    char path[DU_RECENT_PATH + 32];
    r->count = 0;
    if (du_recent_file(path, sizeof(path))) return -ENAMETOOLONG;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? 0 : -errno;
    char *text = malloc(TEXT_CAP);
    if (!text) { fclose(f); return -ENOMEM; }
    size_t n = fread(text, 1, TEXT_CAP - 1, f);
    int err = ferror(f) ? -EIO : 0;
    fclose(f);
    du_recent_parse(r, text, n);
    free(text);
    return err;
}
static int store(const struct du_recent *r)
{
    char path[DU_RECENT_PATH + 32];
    if (du_recent_file(path, sizeof(path))) return -ENAMETOOLONG;
    char *text = malloc(TEXT_CAP);
    if (!text) return -ENOMEM;
    size_t n = du_recent_format(r, text, TEXT_CAP);
    FILE *f = fopen(path, "wb");
    int err = f ? 0 : -errno;
    if (f) { err = fwrite(text, 1, n, f) == n ? 0 : -EIO; if (fclose(f) && !err) err = -EIO; }
    free(text);
    return err;
}
int du_recent_add(const char *path)
{
    struct du_recent *r = malloc(sizeof(*r));
    if (!r) return -ENOMEM;
    int err = du_recent_load(r);
    if (err) r->count = 0;   /* an unreadable list is rebuilt */
    err = du_recent_push(r, path) ? -EINVAL : store(r);
    free(r);
    return err;
}
int du_recent_clear(void)
{
    struct du_recent *r = calloc(1, sizeof(*r));
    if (!r) return -ENOMEM;
    int err = store(r);
    free(r);
    return err;
}
