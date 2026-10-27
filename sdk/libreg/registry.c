/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#ifndef CIUKI_REG_PARSE_ONLY
#include "transport.h"
#endif
#ifndef CIUKI_REG_PARSE_ONLY
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#endif
#include "casefold.h"

int reg_fail(int error) { errno = error; return -1; }
/* Reject invalid UTF-8 before comparison or serialization. */
static uint32_t scalar(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    uint32_t c = *s++; unsigned n; uint32_t min;
    if (c < 0x80) { *text = (const char *)s; return c; }
    if (c >= 0xc2 && c <= 0xdf) { n = 1; c &= 31; min = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { n = 2; c &= 15; min = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 3; c &= 7; min = 0x10000; }
    else return UINT32_MAX;
    while (n--) { if ((*s & 0xc0) != 0x80) return UINT32_MAX; c = (c << 6) | (*s++ & 63); }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return UINT32_MAX;
    *text = (const char *)s; return c;
}
static int utf8(const char *s, unsigned limit, int name)
{
    unsigned n = 0;
    while (*s) {
        uint32_t c = scalar(&s);
        if (c == UINT32_MAX || ++n > limit || (name && (c < 32 || (c >= 127 && c <= 159)))) return 0;
    }
    return 1;
}
struct fold_cursor { const char *text; uint32_t pending[3]; unsigned at; };
static uint32_t folded(struct fold_cursor *f)
{
    if (f->at < 3 && f->pending[f->at]) return f->pending[f->at++];
    if (!*f->text) return 0;
    uint32_t c = scalar(&f->text);
    if (c == UINT32_MAX) { f->text = ""; return c; }
    size_t lo = 0, hi = sizeof(folds) / sizeof(folds[0]);
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (folds[mid].from < c) lo = mid + 1; else hi = mid; }
    memset(f->pending, 0, sizeof(f->pending)); f->at = 1;
    if (lo < sizeof(folds) / sizeof(folds[0]) && folds[lo].from == c) {
        memcpy(f->pending, folds[lo].to, sizeof(f->pending)); return f->pending[0];
    }
    return c;
}
int reg_name_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    struct fold_cursor x = {.text = a}, y = {.text = b};
    for (;;) { uint32_t c = folded(&x), d = folded(&y); if (c != d || c == UINT32_MAX) return 0; if (!c) return 1; }
}
static int name_valid(const char *s)
{
    return s && strlen(s) <= REG_NAME_BYTES && utf8(s, REG_NAME_CHARS, 1);
}
int reg_path_valid(const char *root, const char *path)
{
    if (!path || strlen(path) > REG_PATH_MAX) return 0;
    char name[REG_NAME_BYTES + 1]; unsigned depth = 0;
    const char *s = path;
    for (;;) {
        const char *end = strchr(s, '\\'); size_t n = end ? (size_t)(end - s) : strlen(s);
        if (!n || n > REG_NAME_BYTES || ++depth > REG_DEPTH_MAX) return 0;
        memcpy(name, s, n); name[n] = 0;
        if (!name_valid(name) || name[0] == ' ' || name[n - 1] == ' ' || strchr(name, '[') || strchr(name, ']')) return 0;
        if (depth == 1 && !reg_name_equal(name, root)) return 0;
        if (!end) return 1;
        s = end + 1;
    }
}
static void value_free(struct rv *v) { free(v->name); free(v->data); free(v); }
static void key_free(struct rk *k)
{
    while (k->values) { struct rv *v = k->values; k->values = v->next; value_free(v); }
    free(k->path); free(k);
}
void reg_tree_free(struct rt *t)
{
    if (!t) return;
    while (t->keys) { struct rk *k = t->keys; t->keys = k->next; key_free(k); }
    free(t->comments); free(t);
}
struct rk *reg_key(struct rt *t, const char *path)
{
    for (struct rk *k = t->keys; k; k = k->next) if (reg_name_equal(k->path, path)) return k;
    return NULL;
}
struct rk *reg_ensure(struct rt *t, const char *path)
{
    struct rk *k = reg_key(t, path); if (k) return k;
    if (t->nk >= REG_KEYS_MAX) { reg_fail(E2BIG); return NULL; }
    char *s = strdup(path); if (!s) return NULL;
    char *sep = strrchr(s, '\\');
    if (sep) {
        *sep = 0; struct rk *parent = reg_ensure(t, s); *sep = '\\';
        if (!parent) { free(s); return NULL; }
        size_t n = strlen(parent->path) + strlen(sep + 1) + 2;
        char *canonical = malloc(n); if (!canonical) { free(s); return NULL; }
        snprintf(canonical, n, "%s\\%s", parent->path, sep + 1);
        free(s); s = canonical; /* Preserve parent spelling and canonical root. */
    }
    if (t->nk >= REG_KEYS_MAX) { free(s); reg_fail(E2BIG); return NULL; }
    k = calloc(1, sizeof(*k)); if (!k) { free(s); return NULL; }
    k->path = s; struct rk **p = &t->keys; while (*p) p = &(*p)->next; *p = k; t->nk++; return k;
}
struct rt *reg_tree_new(const char *name)
{
    struct rt *t = calloc(1, sizeof(*t));
    if (t && !reg_ensure(t, name)) { reg_tree_free(t); return NULL; }
    return t;
}
static struct rv *value_find(struct rk *k, const char *name)
{
    if (k) for (struct rv *v = k->values; v; v = v->next) if (reg_name_equal(v->name, name)) return v;
    return NULL;
}
int reg_put(struct rt *t, const char *key, const char *name, enum reg_type type, const void *data, size_t n)
{
    if (!name_valid(name) || (n && !data)) return reg_fail(EINVAL);
    if ((type == REG_STRING && n > REG_DATA_MAX + 1) ||
        (type == REG_BINARY && n > REG_DATA_MAX)) return reg_fail(E2BIG);
    if (type == REG_STRING && (!n || ((const char *)data)[n - 1] ||
        strlen(data) + 1 != n || !utf8(data, REG_DATA_MAX, 0))) return reg_fail(EINVAL);
    if ((type == REG_INT32 || type == REG_BOOL) && n != sizeof(int32_t)) return reg_fail(EINVAL);
    struct rk *k = reg_ensure(t, key); if (!k) return -1;
    struct rv *v = value_find(k, name);
    if (!v && t->nv >= REG_VALUES_MAX) return reg_fail(E2BIG);
    void *copy = malloc(n ? n : 1); if (!copy) return -1;
    if (n) memcpy(copy, data, n);
    if (!v) {
        v = calloc(1, sizeof(*v));
        if (!v) { free(copy); return -1; }
        v->name = strdup(name); if (!v->name) { free(copy); free(v); return -1; }
        struct rv **p = &k->values; while (*p) p = &(*p)->next; *p = v; t->nv++;
    }
    free(v->data); v->data = copy; v->size = n; v->type = type; return 0;
}
struct buffer { char *s; size_t n, cap; int error; };
static void append(struct buffer *b, const char *s, size_t n)
{
    if (b->error) return;
    if (n > REG_TEXT_MAX - b->n) { b->error = E2BIG; return; }
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256; while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->s, cap); if (!p) { b->error = ENOMEM; return; } b->s = p; b->cap = cap;
    }
    memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static void add(struct buffer *b, const char *s) { append(b, s, strlen(s)); }
#ifndef CIUKI_REG_PARSE_ONLY
static void quote(struct buffer *b, const char *s)
{
    add(b, "\"");
    for (; *s; s++) {
        switch (*s) {
        case '\\': add(b, "\\\\"); break; case '"': add(b, "\\\""); break;
        case '\n': add(b, "\\n"); break; case '\r': add(b, "\\r"); break; case '\t': add(b, "\\t"); break;
        default: append(b, s, 1); break;
        }
    }
    add(b, "\"");
}
static void literal(struct buffer *b, struct rv *v)
{
    char number[32];
    if (v->type == REG_STRING) quote(b, v->data);
    else if (v->type == REG_BOOL) add(b, *(int32_t *)v->data ? "true" : "false");
    else if (v->type == REG_INT32) { snprintf(number, sizeof(number), "%ld", (long)*(int32_t *)v->data); add(b, number); }
    else {
        static const char hex[] = "0123456789abcdef"; add(b, "hex:");
        for (size_t i = 0; i < v->size; i++) { unsigned c = ((unsigned char *)v->data)[i]; char pair[2] = {hex[c >> 4], hex[c & 15]}; append(b, pair, 2); }
    }
}
static int descendant(const char *parent, const char *key)
{
    /* Unicode folding can change byte counts, so compare a component-boundary prefix. */
    if (reg_name_equal(parent, key)) return 1;
    char *prefix = strdup(key); if (!prefix) return -1;
    int found = 0;
    for (char *s = prefix; *s; s++) if (*s == '\\') { *s = 0; if (reg_name_equal(parent, prefix)) found = 1; *s = '\\'; if (found) break; }
    free(prefix); return found;
}
char *reg_serialise(struct rt *t, const char *branch, size_t *size)
{
    struct buffer b = {0}; char number[80];
    snprintf(number, sizeof(number), "; ciuki-registry generation=%llu\n", (unsigned long long)t->generation); add(&b, number);
    if (t->comments) add(&b, t->comments);
    for (struct rk *k = t->keys; k; k = k->next) {
        int include = branch ? descendant(branch, k->path) : 1;
        if (include < 0) { b.error = ENOMEM; break; } if (!include) continue;
        add(&b, "\n["); add(&b, k->path); add(&b, "]\n");
        for (struct rv *v = k->values; v; v = v->next) { quote(&b, v->name); add(&b, " = "); literal(&b, v); add(&b, "\n"); }
    }
    if (b.error) { free(b.s); reg_fail(b.error); return NULL; }
    *size = b.n; return b.s;
}
#endif
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}
static char *unquote(char **cursor)
{
    char *s = *cursor;
    if (*s != '"') return NULL;
    char *out = malloc(strlen(s) + 1); if (!out) return NULL;
    size_t n = 0; s++;
    while (*s && *s != '"') {
        char c = *s++;
        if (c == '\\') {
            c = *s++;
            if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
            else if (c != '\\' && c != '"') { free(out); reg_fail(EINVAL); return NULL; }
        }
        out[n++] = c;
    }
    if (*s != '"') { free(out); reg_fail(EINVAL); return NULL; }
    out[n] = 0; *cursor = s + 1; return out;
}
static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1;
}
static int parse_literal(struct rt *t, const char *key, const char *name, char *text)
{
    text = trim(text); int r;
    if (*text == '"') {
        char *s = unquote(&text); if (!s) return -1;
        if (*trim(text)) { free(s); return reg_fail(EINVAL); }
        r = reg_put(t, key, name, REG_STRING, s, strlen(s) + 1); free(s); return r;
    }
    if (!strncmp(text, "hex:", 4)) {
        text += 4; size_t n = strlen(text);
        if (n & 1) return reg_fail(EINVAL); if (n / 2 > REG_DATA_MAX) return reg_fail(E2BIG);
        unsigned char *s = malloc(n / 2 + 1); if (!s) return -1;
        for (size_t i = 0; i < n; i += 2) {
            int a = hex_digit((unsigned char)text[i]), b = hex_digit((unsigned char)text[i + 1]);
            if (a < 0 || b < 0) { free(s); return reg_fail(EINVAL); } s[i / 2] = (unsigned char)((a << 4) | b);
        }
        r = reg_put(t, key, name, REG_BINARY, s, n / 2); free(s); return r;
    }
    int32_t value;
    if (!strcmp(text, "true") || !strcmp(text, "false")) {
        value = !strcmp(text, "true"); return reg_put(t, key, name, REG_BOOL, &value, sizeof(value));
    }
    char *p = text; if (*p == '-' || *p == '+') p++;
    if (!*p) return reg_fail(EINVAL);
    for (; *p; p++) if (*p < '0' || *p > '9') return reg_fail(EINVAL);
    char *end; errno = 0; long long v = strtoll(text, &end, 10);
    if (errno || *end || v < INT32_MIN || v > INT32_MAX) return reg_fail(EINVAL);
    value = (int32_t)v; return reg_put(t, key, name, REG_INT32, &value, sizeof(value));
}
struct rt *reg_parse(const char *root, const char *text, size_t size)
{
    if (size > REG_TEXT_MAX) { reg_fail(E2BIG); return NULL; }
    if (memchr(text, 0, size)) { reg_fail(EINVAL); return NULL; }
    char *copy = malloc(size + 1); if (!copy) return NULL;
    memcpy(copy, text, size); copy[size] = 0;
    struct rt *t = reg_tree_new(root); if (!t) { free(copy); return NULL; }
    struct buffer comments = {0}; struct rk *key = NULL; int seen_generation = 0;
    char *line = copy;
    while (line && *line) {
        char *next = strchr(line, '\n'); if (next) *next++ = 0;
        char *s = trim(line); char *comment = NULL; int quoted = 0, escaped = 0;
        char *scan = *s == '[' ? strchr(s, ']') : s;
        if (!scan) goto invalid;
        if (*s == '[') scan++;
        for (char *p = scan; *p; p++) {
            if (escaped) { escaped = 0; continue; }
            if (quoted && *p == '\\') { escaped = 1; continue; }
            if (*p == '"') quoted = !quoted;
            else if (!quoted && *p == ';') { comment = p; break; }
        }
        if (comment) {
            static const char prefix[] = "; ciuki-registry generation=";
            if (!strncmp(comment, prefix, sizeof(prefix) - 1)) {
                char *n = trim(comment + sizeof(prefix) - 1), *end;
                if (seen_generation++ || !*n) goto invalid;
                for (char *p = n; *p; p++) if (*p < '0' || *p > '9') goto invalid;
                errno = 0; unsigned long long g = strtoull(n, &end, 10);
                if (errno || *end || g == UINT64_MAX) goto invalid;
                t->generation = (uint64_t)g;
            } else { add(&comments, comment); add(&comments, "\n"); }
            *comment = 0; s = trim(s);
        }
        if (!*s) { line = next; continue; }
        if (*s == '[') {
            size_t n = strlen(s); if (n < 3 || s[n - 1] != ']') goto invalid;
            s[n - 1] = 0; s++;
            if (!reg_path_valid(root, s)) goto invalid;
            key = reg_ensure(t, s); if (!key) goto failed;
        } else {
            if (!key) goto invalid;
            char *name;
            if (*s == '"') { name = unquote(&s); if (!name) goto failed; s = trim(s); if (*s != '=') { free(name); goto invalid; } }
            else { char *equal = strchr(s, '='); if (!equal) goto invalid; *equal = 0; name = strdup(trim(s)); s = equal; }
            if (!name) goto failed;
            if (!name_valid(name)) { free(name); goto invalid; }
            int result = parse_literal(t, key->path, name, s + 1); free(name); if (result) goto failed;
        }
        line = next;
    }
    if (comments.error) { reg_fail(comments.error); goto failed; }
    if (comments.s && !utf8(comments.s, REG_TEXT_MAX, 0)) goto invalid;
    t->comments = comments.s; free(copy); return t;
invalid: reg_fail(EINVAL);
failed: { int e = errno; free(comments.s); reg_tree_free(t); free(copy); errno = e; return NULL; }
}
#ifndef CIUKI_REG_PARSE_ONLY
static char *root_file(struct reg_root *r)
{
    size_t n = strlen(r->directory) + strlen(r->name) + 7; char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s.reg", r->directory, r->name); return p;
}
int reg_load(struct reg_root *r)
{
    struct rt *t;
    if (!r) return reg_fail(EINVAL);
    if (r->remote) {
        int result=reg_remote_load(r->remote,&r->tree);
        if(result){reg_tree_free(r->tree);r->tree=NULL;}
        return result;
    }
    if (!strcmp(r->name, "Hardware")) {
        t = reg_hardware(r->tree ? r->tree->generation : REG_GENERATION_ERROR);
        if (!t && !errno && r->tree) return 0;
    }
    else {
        char *path = root_file(r), *text = NULL; size_t size;
        if (!path) return -1;
        if (reg_recover(path, r->name)) { free(path); return -1; }
        int read = reg_file_read(path, &text, &size); free(path);
        if (read && errno != ENOENT) return -1;
        if (!read && r->snapshot && size == r->snapshot_size && !memcmp(text, r->snapshot, size)) { free(text); return 0; }
        t = read ? reg_tree_new(r->name) : reg_parse(r->name, text, size);
        if (t) { free(r->snapshot); r->snapshot = read ? NULL : text; r->snapshot_size = read ? 0 : size; }
        if (read || !t) free(text);
    }
    if (!t) return -1;
    reg_tree_free(r->tree); r->tree = t; return 0;
}
struct reg_root *reg_open_root_at(const char *name, const char *directory)
{
    static const char *const names[] = {"System", "Desktop", "Programs", "Users", "Hardware"};
    const char *canonical = NULL;
    for (unsigned i = 0; i < 5; i++) if (reg_name_equal(names[i], name)) canonical = names[i];
    if (!canonical || !directory || *directory != '/') { reg_fail(EINVAL); return NULL; }
#if defined(__CIUKIOS__) && !defined(__linux__)
    if(!strcmp(canonical,"Desktop")&&!strcmp(directory,REG_DIRECTORY)){
        directory=getenv("CIUKI_PROFILE_SETTINGS");
        if(!directory||strncmp(directory,"/Windows/Profiles/",18)){reg_fail(EACCES);return NULL;}
    }
#endif
    struct reg_root *r = calloc(1, sizeof(*r)); if (!r) return NULL;
    r->name = canonical; r->directory = strdup(directory);
    if (r->directory) {
        size_t n = strlen(r->directory);
        while (n > 1 && r->directory[n - 1] == '/') r->directory[--n] = 0;
    }
#if (defined(__CIUKIOS__) && !defined(__linux__)) || defined(CIUKI_REG_REMOTE)
    if(!strcmp(canonical,"System")||!strcmp(canonical,"Programs")||!strcmp(canonical,"Users")){
        r->remote=calloc(1,sizeof(*r->remote));
        if(!r->remote){reg_close_root(r);return NULL;}
        r->remote->fd=-1;r->remote->root=!strcmp(canonical,"System")?1:!strcmp(canonical,"Programs")?2:3;
    }
#endif
    if (!r->directory || reg_load(r)) { int e = errno; reg_close_root(r); errno = e; return NULL; }
    return r;
}
struct reg_root *reg_open_root(const char *name) { return reg_open_root_at(name, REG_DIRECTORY); }
void reg_close_root(struct reg_root *r) { if (r) { if(r->remote){reg_remote_close(r->remote);free(r->remote);} reg_tree_free(r->tree); free(r->snapshot); free(r->directory); free(r); } }
const char *reg_root_name(const struct reg_root *r) { return r ? r->name : NULL; }
int reg_read_only(const struct reg_root *r) { return r && !strcmp(r->name, "Hardware"); }
uint64_t reg_generation(struct reg_root *r) { return reg_load(r) ? REG_GENERATION_ERROR : r->tree->generation; }
static int writable(struct reg_root *r, const char *key)
{
    if (!r || !reg_path_valid(r->name, key)) return reg_fail(EINVAL);
    if (reg_read_only(r)) return reg_fail(EROFS);
    return reg_load(r);
}
int reg_commit(struct reg_root *r, struct rt *tree, uint64_t generation)
{
    if (generation >= UINT64_MAX - 1) return reg_fail(EOVERFLOW);
    tree->generation = generation + 1;
    if(r->remote)return reg_remote_commit(r->remote,tree,generation);
    size_t n; char *text = reg_serialise(tree, NULL, &n), *path = root_file(r);
    if (!text || !path) { free(text); free(path); return -1; }
    /* Re-read without touching the caller's mutation tree. */
    char *old = NULL; size_t size;
    int result = reg_file_read(path, &old, &size);
    struct rt *disk = result && errno == ENOENT ? reg_tree_new(r->name) : result ? NULL : reg_parse(r->name, old, size);
    free(old);
    if (!disk) result = -1;
    else if (disk->generation != generation) result = reg_fail(EAGAIN);
    else {
        if (mkdir(r->directory, 0755) && errno != EEXIST) result = -1;
        else result = reg_file_write(path, r->name, text, n, tree->generation);
    }
    int error = errno; reg_tree_free(disk); free(text); free(path); errno = error; return result;
}
static int set(struct reg_root *r, const char *key, const char *name, enum reg_type type, const void *data, size_t n)
{
    if (writable(r, key)) return -1;
    uint64_t g = r->tree->generation; free(r->snapshot); r->snapshot = NULL;
    if (reg_put(r->tree, key, name, type, data, n)) return -1;
    return reg_commit(r, r->tree, g);
}
int reg_set_string(struct reg_root *r, const char *k, const char *n, const char *s)
{
    if (!s) return reg_fail(EINVAL); return set(r, k, n, REG_STRING, s, strlen(s) + 1);
}
int reg_set_int32(struct reg_root *r, const char *k, const char *n, int32_t v) { return set(r, k, n, REG_INT32, &v, sizeof(v)); }
int reg_set_bool(struct reg_root *r, const char *k, const char *n, int v)
{
    if (v != 0 && v != 1) return reg_fail(EINVAL); int32_t b = v; return set(r, k, n, REG_BOOL, &b, sizeof(b));
}
int reg_set_binary(struct reg_root *r, const char *k, const char *n, const void *s, size_t size) { return set(r, k, n, REG_BINARY, s, size); }
int reg_set_text(struct reg_root *r, const char *k, const char *n, const char *text)
{
    if (!text) return reg_fail(EINVAL);
    if (strlen(text) > REG_DATA_MAX * 2 + 5) return reg_fail(E2BIG);
    if (writable(r, k)) return -1;
    uint64_t g = r->tree->generation; free(r->snapshot); r->snapshot = NULL; char *copy = strdup(text); if (!copy) return -1;
    int result = parse_literal(r->tree, k, n, copy); free(copy);
    return result ? -1 : reg_commit(r, r->tree, g);
}
int reg_create_key(struct reg_root *r, const char *key)
{
    if (writable(r, key)) return -1;
    uint64_t g = r->tree->generation; if (reg_key(r->tree, key)) return 0;
    free(r->snapshot); r->snapshot = NULL;
    if (!reg_ensure(r->tree, key)) return -1; return reg_commit(r, r->tree, g);
}
static struct rk *read_key(struct reg_root *r, const char *key)
{
    if (!r || !reg_path_valid(r->name, key)) { reg_fail(EINVAL); return NULL; }
    if (reg_load(r)) return NULL;
    struct rk *k = reg_key(r->tree, key); if (!k) reg_fail(ENOENT); return k;
}
static struct rv *read_value(struct reg_root *r, const char *key, const char *name)
{
    if (!name_valid(name)) { reg_fail(EINVAL); return NULL; }
    struct rk *k = read_key(r, key); if (!k) return NULL;
    struct rv *v = value_find(k, name); if (!v) reg_fail(ENOENT); return v;
}
static int copy_value(struct rv *v, enum reg_type type, void *out, size_t *size)
{
    if (!v) return -1;
    if (!size || v->type != type) return reg_fail(EINVAL);
    size_t cap = *size; *size = v->size;
    if (out) { if (cap < v->size) return reg_fail(ERANGE); memcpy(out, v->data, v->size); }
    return 0;
}
int reg_get_string(struct reg_root *r, const char *k, const char *n, char *s, size_t *size) { return copy_value(read_value(r, k, n), REG_STRING, s, size); }
int reg_get_binary(struct reg_root *r, const char *k, const char *n, void *s, size_t *size) { return copy_value(read_value(r, k, n), REG_BINARY, s, size); }
int reg_get_int32(struct reg_root *r, const char *k, const char *n, int32_t *v)
{
    if (!v) return reg_fail(EINVAL); size_t size = sizeof(*v); return copy_value(read_value(r, k, n), REG_INT32, v, &size);
}
int reg_get_bool(struct reg_root *r, const char *k, const char *n, int *v)
{
    if (!v) return reg_fail(EINVAL); int32_t b; size_t size = sizeof(b);
    int result = copy_value(read_value(r, k, n), REG_BOOL, &b, &size); if (!result) *v = (int)b; return result;
}
int reg_get_text(struct reg_root *r, const char *k, const char *n, char *s, size_t *size)
{
    if (!size) return reg_fail(EINVAL); struct rv *v = read_value(r, k, n); if (!v) return -1;
    struct buffer b = {0}; literal(&b, v);
    if (b.error) { free(b.s); return reg_fail(b.error); }
    size_t cap = *size; *size = b.n + 1;
    int result = s && cap < b.n + 1 ? reg_fail(ERANGE) : 0;
    if (!result && s) memcpy(s, b.s, b.n + 1); free(b.s); return result;
}
int reg_enum_keys(struct reg_root *r, const char *key, unsigned index, char *out, size_t cap)
{
    if (!out || !cap) return reg_fail(EINVAL);
    struct rk *parent = read_key(r, key); if (!parent) return -1;
    for (struct rk *k = r->tree->keys; k; k = k->next) {
        if (k == parent) continue;
        char *sep = strrchr(k->path, '\\'); if (!sep) continue;
        *sep = 0; int match = reg_name_equal(parent->path, k->path); *sep = '\\';
        if (match && !index--) { size_t n = strlen(sep + 1) + 1; if (n > cap) return reg_fail(ERANGE); memcpy(out, sep + 1, n); return 0; }
    }
    return reg_fail(ENOENT);
}
int reg_enum_values(struct reg_root *r, const char *key, unsigned index, struct reg_value_info *out)
{
    if (!out) return reg_fail(EINVAL); struct rk *k = read_key(r, key); if (!k) return -1;
    for (struct rv *v = k->values; v; v = v->next) if (!index--) {
        memset(out, 0, sizeof(*out)); strcpy(out->name, v->name); out->type = v->type; out->size = v->size; return 0;
    }
    return reg_fail(ENOENT);
}
int reg_delete(struct reg_root *r, const char *key, const char *name)
{
    if (writable(r, key)) return -1;
    uint64_t g = r->tree->generation; free(r->snapshot); r->snapshot = NULL; struct rk *k = reg_key(r->tree, key); if (!k) return reg_fail(ENOENT);
    if (name) {
        if (!name_valid(name)) return reg_fail(EINVAL);
        struct rv **p = &k->values; while (*p && !reg_name_equal((*p)->name, name)) p = &(*p)->next;
        if (!*p) return reg_fail(ENOENT); struct rv *v = *p; *p = v->next; value_free(v); r->tree->nv--;
    } else {
        if (reg_name_equal(key, r->name)) return reg_fail(EACCES);
        struct rk **p = &r->tree->keys;
        while (*p) {
            int match = descendant(key, (*p)->path); if (match < 0) return -1;
            if (match) {
                k = *p; *p = k->next;
                for (struct rv *v = k->values; v; v = v->next) r->tree->nv--;
                key_free(k); r->tree->nk--;
            } else p = &(*p)->next;
        }
    }
    return reg_commit(r, r->tree, g);
}
int reg_export(struct reg_root *r, const char *key, const char *path)
{
    if (!path || *path != '/' || !r) return reg_fail(EINVAL);
    /* Reject registry directory aliases too: no . or .. components allowed. */
    if (strstr(path, "/../") || strstr(path, "/./") || strstr(path, "//")) return reg_fail(EINVAL);
    size_t dir = strlen(r->directory);
    if (!r->remote && strlen(path) >= dir) {
        char *prefix = strndup(path, dir); if (!prefix) return -1;
        int reserved = reg_name_equal(prefix, r->directory) && (path[dir] == '/' || !path[dir]);
        free(prefix); if (reserved) return reg_fail(EACCES);
    }
    if (!read_key(r, key)) return -1;
    size_t size; char *text = reg_serialise(r->tree, key, &size); if (!text) return -1;
    int result = reg_recover(path, r->name);
    if (!result) result = reg_file_write(path, r->name, text, size, r->tree->generation);
    int e = errno; free(text); errno = e; return result;
}
int reg_import(struct reg_root *r, const char *path)
{
    if (!r || !path || *path != '/') return reg_fail(EINVAL);
    if (writable(r, r->name)) return -1;
    uint64_t g = r->tree->generation; free(r->snapshot); r->snapshot = NULL; char *text; size_t n;
    if (reg_file_read(path, &text, &n)) return -1;
    struct rt *imported = reg_parse(r->name, text, n); free(text); if (!imported) return -1;
    int result = 0;
    for (struct rk *k = imported->keys; k && !result; k = k->next) {
        if (!reg_ensure(r->tree, k->path)) { result = -1; break; }
        for (struct rv *v = k->values; v && !result; v = v->next) result = reg_put(r->tree, k->path, v->name, v->type, v->data, v->size);
    }
    if (!result && imported->comments) {
        size_t a = r->tree->comments ? strlen(r->tree->comments) : 0, b = strlen(imported->comments);
        if (a + b > REG_TEXT_MAX) result = reg_fail(E2BIG);
        else { char *s = realloc(r->tree->comments, a + b + 1); if (!s) result = -1; else { memcpy(s + a, imported->comments, b + 1); r->tree->comments = s; } }
    }
    reg_tree_free(imported); return result ? -1 : reg_commit(r, r->tree, g);
}
#endif /* CIUKI_REG_PARSE_ONLY */
