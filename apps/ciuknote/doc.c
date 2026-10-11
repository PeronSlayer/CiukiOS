/* SPDX-License-Identifier: MIT */
#include "doc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cn_decode(const char *raw, size_t n, char *out, size_t cap, size_t *len, int *crlf)
{
    size_t w = 0; int seen = 0;
    for (size_t i = 0; i < n;) {
        char c = raw[i];
        if (c == '\r' && i + 1 < n && raw[i + 1] == '\n') { if (!seen) { seen = 1; *crlf = 1; } c = '\n'; i += 2; }
        else { if (c == '\n' && !seen) { seen = 1; *crlf = 0; } i++; }
        if (w >= cap) return -EFBIG;
        out[w++] = c;
    }
    *len = w;
    return 0;
}
size_t cn_encoded_size(const char *text, size_t n, int crlf)
{
    size_t size = n;
    if (crlf) for (size_t i = 0; i < n; i++) size += text[i] == '\n';
    return size;
}
size_t cn_encode(const char *text, size_t n, int crlf, char *out)
{
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\n' && crlf) out[w++] = '\r';
        out[w++] = text[i];
    }
    return w;
}
int cn_is_log(const char *text, size_t n)
{ return n >= 4 && !memcmp(text, ".LOG", 4) && (n == 4 || text[4] == '\n'); }
void cn_timestamp(char *out, size_t cap, const struct tm *tm)
{
    int hour = tm->tm_hour % 12;
    snprintf(out, cap, "%d:%02d %s %d/%d/%d", hour ? hour : 12, tm->tm_min, tm->tm_hour >= 12 ? "PM" : "AM",
             tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
}

const char *cn_basename(const char *path)
{ const char *slash = strrchr(path, '/'); return slash ? slash + 1 : path; }
int cn_has_extension(const char *name)
{ const char *dot = strrchr(name, '.'); return dot && dot != name && dot[1]; }
int cn_is_text_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || name[n - 4] != '.') return 0;
    const char *e = name + n - 3;
    return (e[0] | 32) == 't' && (e[1] | 32) == 'x' && (e[2] | 32) == 't';
}
int cn_join(char *out, const char *dir, const char *name)
{
    size_t d = strlen(dir), n = strlen(name);
    if (d + 1 + n + 1 > CN_PATH) return -ENAMETOOLONG;
    memcpy(out, dir, d);
    if (d && dir[d - 1] != '/') out[d++] = '/';
    memcpy(out + d, name, n + 1);
    return 0;
}
/* Appends the components of `path` to the normalised path in `out`, resolving "." and "..". */
static int push_path(char *out, size_t *w, size_t *starts, size_t *depth, const char *path)
{
    for (const char *p = path; *p;) {
        while (*p == '/') p++;
        const char *q = p;
        while (*q && *q != '/') q++;
        size_t n = (size_t)(q - p);
        if (n == 1 && p[0] == '.') { }
        else if (n == 2 && p[0] == '.' && p[1] == '.') { if (*depth) *w = starts[--*depth]; }
        else if (n) {
            if (*w + 1 + n + 1 > CN_PATH || *depth == CN_PATH / 2) return -ENAMETOOLONG;
            starts[(*depth)++] = *w; out[(*w)++] = '/'; memcpy(out + *w, p, n); *w += n;
        }
        p = q;
    }
    return 0;
}
int cn_resolve(char *out, const char *dir, const char *input)
{
    if (!input[0]) return -ENOENT;
    if (strlen(input) >= CN_PATH || strlen(dir) >= CN_PATH) return -ENAMETOOLONG;
    if (strchr(input, '\\') || strchr(input, ':')) return -EINVAL;
    size_t starts[CN_PATH / 2], depth = 0, w = 0;
    int r = input[0] == '/' ? 0 : push_path(out, &w, starts, &depth, dir);
    if (!r) r = push_path(out, &w, starts, &depth, input);
    if (r) return r;
    if (!w) out[w++] = '/';
    out[w] = 0;
    return 0;
}

static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }
static int name_compare(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned char x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) break;
    }
    return strcmp(a, b);
}
struct collect { struct cn_listing *list; int text_only; };
static int collect_entry(const char *name, const struct cn_stat *st, void *arg)
{
    struct collect *c = arg;
    if (!st->dir && c->text_only && !cn_is_text_name(name)) return 0;
    struct cn_listing *l = c->list;
    if (l->count >= CN_ENTRIES) { l->truncated = 1; return 0; }
    if (l->count == l->capacity) {
        unsigned capacity = l->capacity ? l->capacity * 2 : 64;
        struct cn_entry *grown = realloc(l->entries, capacity * sizeof(*grown));
        if (!grown) return -ENOMEM;
        l->entries = grown; l->capacity = capacity;
    }
    size_t len = strlen(name) + 1;
    char *copy = malloc(len);
    if (!copy) return -ENOMEM;
    memcpy(copy, name, len);
    l->entries[l->count++] = (struct cn_entry){copy, st->dir != 0, st->size, st->mtime};
    return 0;
}
int cn_list(const struct cn_fs *fs, const char *dir, int text_only, struct cn_listing *out)
{
    memset(out, 0, sizeof(*out));
    struct collect c = {out, text_only};
    int r = fs->list(fs->ctx, dir, collect_entry, &c);
    if (r) { cn_listing_free(out); return r; }
    return 0;
}
static const char *extension(const struct cn_entry *e)
{ const char *dot = strrchr(e->name, '.'); return e->dir ? "" : dot && dot != e->name ? dot + 1 : ""; }
static unsigned sort_column; static int sort_descending;
static int entry_compare(const void *pa, const void *pb)
{
    const struct cn_entry *a = pa, *b = pb;
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    int r = 0;
    if (sort_column == 1) r = a->size < b->size ? -1 : a->size > b->size;
    else if (sort_column == 2) r = name_compare(extension(a), extension(b));
    else if (sort_column == 3) r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
    if (!r) r = name_compare(a->name, b->name);
    return sort_descending ? -r : r;
}
void cn_listing_sort(struct cn_listing *l, unsigned column, int descending)
{
    /* qsort is not stable: the name is the final tie-breaker, so the order is total. */
    sort_column = column; sort_descending = descending;
    if (l->count > 1) qsort(l->entries, l->count, sizeof(*l->entries), entry_compare);
}
void cn_listing_free(struct cn_listing *l)
{
    for (unsigned i = 0; i < l->count; i++) free(l->entries[i].name);
    free(l->entries); memset(l, 0, sizeof(*l));
}
