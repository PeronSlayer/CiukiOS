/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Single-threaded authenticated owner. */
#define _POSIX_C_SOURCE 200809L
#include "registry.h"
#include <stdlib.h>
#include <string.h>
#include "../../sdk/libreg/casefold.h"

static uint32_t scalar(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s++, min; unsigned n;
    if (c < 128) { *p = (const char *)s; return c; }
    if (c >= 0xc2 && c <= 0xdf) { c &= 31; n = 1; min = 128; }
    else if (c >= 0xe0 && c <= 0xef) { c &= 15; n = 2; min = 2048; }
    else if (c >= 0xf0 && c <= 0xf4) { c &= 7; n = 3; min = 65536; }
    else return UINT32_MAX;
    while (n--) { if ((*s & 192) != 128) return UINT32_MAX; c = (c << 6) | (*s++ & 63); }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return UINT32_MAX;
    *p = (const char *)s; return c;
}
int wr_name(const char *s)
{
    unsigned count = 0;
    if (!s || strlen(s) >= W32R_NAME) return 0;
    while (*s) if (scalar(&s) == UINT32_MAX || ++count > 255) return 0;
    return 1;
}
struct cursor { const char *s; uint32_t pending[3]; unsigned at; };
static uint32_t fold(struct cursor *f)
{
    if (f->at < 3 && f->pending[f->at]) return f->pending[f->at++];
    if (!*f->s) return 0;
    uint32_t c = scalar(&f->s);
    if (c == UINT32_MAX) { f->s = ""; return c; }
    size_t lo = 0, hi = sizeof(folds) / sizeof(*folds);
    while (lo < hi) { size_t m = (lo + hi) / 2; if (folds[m].from < c) lo = m + 1; else hi = m; }
    memset(f->pending, 0, sizeof(f->pending)); f->at = 1;
    if (lo < sizeof(folds) / sizeof(*folds) && folds[lo].from == c) {
        memcpy(f->pending, folds[lo].to, sizeof(f->pending)); return f->pending[0];
    }
    return c;
}
int wr_equal(const char *a, const char *b)
{
    struct cursor x = {.s = a}, y = {.s = b};
    for (;;) { uint32_t c = fold(&x), d = fold(&y); if (c != d || c == UINT32_MAX) return 0; if (!c) return 1; }
}
uint32_t wr_crc(const void *v, size_t n)
{
    const uint8_t *s = v; uint32_t crc = UINT32_MAX;
    while (n--) { crc ^= *s++; for (unsigned b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ~crc;
}
/* Escaped text accounting is exact, so failure at 4 MiB precedes mutation. */
static size_t escaped(const char *s)
{
    size_t n = 0;
    for (; *s; s++) { unsigned c = (unsigned char)*s; n += c < 32 || c == 127 || c == '\\' || c == '[' || c == ']' || c == '=' || c == ':' ? 4 : 1; }
    return n;
}
static size_t key_path_size(struct wr_key *k)
{
    size_t n = escaped(k->name);
    while (k->parent) { k = k->parent; n += 4 + escaped(k->name); }
    return n;
}
struct wr_key *wr_child(struct wr_server *s, struct wr_key *parent, const char *name)
{
    for (struct wr_key *k = s->keys; k; k = k->next)
        if (!k->deleted && k->parent == parent && wr_equal(k->name, name)) return k;
    return NULL;
}
static void dirty(struct wr_server *s, unsigned h)
{
    if (h > 1) return;
    if (!s->hive[h].dirty) s->hive[h].dirty_since = s->now;
    s->hive[h].dirty = 1;
}
struct wr_key *wr_add_key(struct wr_server *s, struct wr_key *parent, const char *name, unsigned h)
{
    if (!wr_name(name) || strchr(name, '\\')) return NULL;
    size_t length = strlen(name); unsigned depth = 1;
    for (struct wr_key *p = parent; p; p = p->parent) {
        length += strlen(p->name) + 1;
        if (++depth > 255 || length >= W32R_PATH) return NULL;
    }
    size_t cost = escaped(name) + (parent ? key_path_size(parent) + 4 : 0) + 3;
    if (h < 2 && s->hive[h].text + cost > W32R_TEXT) return NULL;
    struct wr_key *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->name = strdup(name);
    if (!k->name) { free(k); return NULL; }
    k->parent = parent; k->hive = h;
    struct wr_key **tail = &s->keys; while (*tail) tail = &(*tail)->next; *tail = k;
    if (h < 2) s->hive[h].text += cost;
    dirty(s, h); return k;
}
static struct wr_value *value(struct wr_key *k, const char *name)
{
    for (struct wr_value *v = k->values; v; v = v->next) if (wr_equal(v->name, name)) return v;
    return NULL;
}
static size_t value_cost(struct wr_value *v) { return escaped(v->name) + 11 + (size_t)v->bytes * 2; }
static uint32_t characters(const char *text, int wide)
{
    uint32_t n = 0;
    while (*text) { uint32_t c = scalar(&text); n += wide && c > 65535 ? 2 : 1; }
    return n;
}
uint32_t wr_put(struct wr_server *s, struct wr_key *k, const char *name, uint32_t type, const void *data, uint32_t n)
{
    if (!wr_name(name) || n > W32R_DATA || (!data && n)) return 87;
    if (type != 0 && type != 1 && type != 2 && type != 3 && type != 4 && type != 7) return 87;
    if (type == 4 && n != 4) return 87;
    struct wr_value *v = value(k, name);
    size_t cost = escaped(name) + 11 + (size_t)n * 2, old = v ? value_cost(v) : 0;
    if (k->hive < 2 && s->hive[k->hive].text - old + cost > W32R_TEXT) return 8;
    uint8_t *copy = malloc(n ? n : 1);
    if (!copy) return 8;
    if (n) memcpy(copy, data, n);
    if (!v) {
        v = calloc(1, sizeof(*v));
        if (!v) { free(copy); return 8; }
        v->name = strdup(name);
        if (!v->name) { free(v); free(copy); return 8; }
        struct wr_value **tail = &k->values; while (*tail) tail = &(*tail)->next; *tail = v;
    }
    free(v->data); v->data = copy; v->type = type; v->bytes = n;
    if (k->hive < 2) s->hive[k->hive].text = s->hive[k->hive].text - old + cost;
    dirty(s, k->hive); return 0;
}
static void free_values(struct wr_value *v)
{
    while (v) { struct wr_value *next = v->next; free(v->name); free(v->data); free(v); v = next; }
}
void wr_destroy(struct wr_server *s)
{
    while (s->handles) { struct wr_handle *h = s->handles; s->handles = h->next; free(h); }
    while (s->keys) { struct wr_key *k = s->keys; s->keys = k->next; free_values(k->values); free(k->name); free(k); }
    wr_directory_free(s); memset(s, 0, sizeof(*s));
}
int wr_init(struct wr_server *s, const struct wr_io *io, uint64_t now)
{
    memset(s, 0, sizeof(*s)); s->io = *io; s->now = now;
    if (!wr_load(s) || !wr_directory_init(s)) { wr_destroy(s); return 0; }
    return 1;
}
uint32_t wr_flush(struct wr_server *s)
{
    for (unsigned h = 0; h < 2; h++) { uint32_t e = wr_commit(s, h); if (e) return e; }
    return 0;
}
uint32_t wr_tick(struct wr_server *s, uint64_t now)
{
    s->now = now;
    for (unsigned h = 0; h < 2; h++) if (s->hive[h].dirty && now >= s->hive[h].dirty_since && now - s->hive[h].dirty_since >= 2000) {
        uint32_t e = wr_commit(s, h); if (e) return e;
    }
    return 0;
}
static void collect_deleted(struct wr_server *);
uint32_t wr_exit(struct wr_server *s, uint32_t pid)
{
    if (s->profile_owner==pid) s->profile_owner=0;
    uint32_t e = wr_flush(s);
    struct wr_handle **at = &s->handles;
    while (*at) { struct wr_handle *h = *at; if (h->pid == pid) { *at = h->next; free(h); } else at = &h->next; }
    collect_deleted(s);
    uint32_t d = wr_directory_exit(s, pid); return e ? e : d;
}
static uint32_t resolve(struct wr_server *s, uint32_t pid, uint32_t id, uint32_t access, struct wr_key **key)
{
    if (id == W32R_HKLM) *key = s->roots[0];
    else if (id == W32R_HKU) *key = s->roots[1];
    else if (id == W32R_HKCU) *key = wr_child(s, s->roots[1], ".DEFAULT");
    else if (id == W32R_HKCR) {
        struct wr_key *software = wr_child(s, s->roots[0], "Software");
        *key = software ? wr_child(s, software, "Classes") : NULL;
    } else if (id == W32R_HKCC) *key = s->roots[2];
    else if (id == W32R_HKDD) *key = s->roots[3];
    else {
        for (struct wr_handle *h = s->handles; h; h = h->next) if (h->id == id && h->pid == pid) {
            if (h->key->deleted) return 1018;
            if ((h->access & access) != access) return 5;
            *key = h->key; return 0;
        }
        return 6;
    }
    return *key ? 0 : 2;
}
static uint32_t walk(struct wr_server *s, struct wr_key *k, const char *path, int create, struct wr_key **out, uint32_t *disposition)
{
    char component[W32R_NAME];
    if (*path == '\\') path++;
    *disposition = 2;
    while (*path) {
        const char *end = strchr(path, '\\'); size_t n = end ? (size_t)(end - path) : strlen(path);
        if (!n || n >= sizeof(component)) return 87;
        memcpy(component, path, n); component[n] = 0;
        if (!wr_name(component)) return 87;
        struct wr_key *next = wr_child(s, k, component);
        if (!next) {
            if (!create) return 2;
            if (k->hive > 1) return 5;
            next = wr_add_key(s, k, component, k->hive);
            if (!next) return 8;
            if (!end || !end[1]) *disposition = 1;
        }
        k = next; if (!end) break; path = end + 1;
    }
    *out = k; return 0;
}
static int descendant(struct wr_key *k, struct wr_key *root)
{
    for (; k; k = k->parent) if (k == root) return 1;
    return 0;
}
static void collect_deleted(struct wr_server *s)
{
    /* Keep all ancestors of open deleted handles; no dangling parent links. */
    struct wr_key **at = &s->keys;
    while (*at) {
        struct wr_key *k = *at; int keep = !k->deleted;
        for (struct wr_handle *h = s->handles; !keep && h; h = h->next) keep = descendant(h->key, k);
        if (keep) at = &k->next;
        else { *at = k->next; free_values(k->values); free(k->name); free(k); }
    }
}
/* The keys the predefined handles name (HKLM, HKU, HKCU, HKCR, HKCC, HKDD). */
static int predefined(struct wr_server *s, struct wr_key *k)
{
    struct wr_key *software = wr_child(s, s->roots[0], "Software");
    return !k->parent || k == s->roots[2] || k == s->roots[3] ||
           k == wr_child(s, s->roots[1], ".DEFAULT") ||
           (software && k == wr_child(s, software, "Classes"));
}
static int compat_protected(struct wr_server *s,struct wr_key *k,const char *path,int ancestor)
{
    if(k->hive)return 0;
    const struct wr_key *chain[128];unsigned count=0;
    for(struct wr_key *p=k;p&&p->parent;p=p->parent){if(count==128)return 1;chain[count++]=p;}
    const char *parts[]={"Software","CiukiOS","Win32","AppCompat"};unsigned depth=0;
    while(count){if(!wr_equal(chain[--count]->name,parts[depth++]))return 0;if(depth==4)return 1;}
    while(*path=='\\')path++;
    while(*path){
        const char *end=strchr(path,'\\');size_t n=end?(size_t)(end-path):strlen(path);
        char component[W32R_NAME];if(!n||n>=sizeof(component))return 0;
        memcpy(component,path,n);component[n]=0;if(!wr_equal(component,parts[depth++]))return 0;
        if(depth==4)return 1;if(!end)break;path=end+1;
    }
    (void)s;return ancestor&&depth<4;
}
uint32_t wr_dispatch(struct wr_server *s, uint32_t pid, const void *input, size_t n, void *output, size_t cap, size_t *used)
{
    struct w32r_request a; struct w32r_reply r = {.version = W32R_VERSION};
    if (cap < sizeof(r)) return 87;
    uint8_t *bytes = (uint8_t *)output + sizeof(r); *used = sizeof(r);
    if (n < sizeof(a)) { r.error = 87; goto done; }
    memcpy(&a, input, sizeof(a));
    if (a.version != W32R_VERSION || !pid || a.path_bytes == 0 || a.path_bytes > W32R_PATH ||
        a.name_bytes == 0 || a.name_bytes > W32R_NAME || a.data_bytes > W32R_DATA ||
        n != sizeof(a) + (size_t)a.path_bytes + a.name_bytes + a.data_bytes) { r.error = 87; goto done; }
    const char *path = (const char *)input + sizeof(a), *name = path + a.path_bytes;
    const void *data = name + a.name_bytes;
    if (path[a.path_bytes - 1] || name[a.name_bytes - 1] ||
        memchr(path, 0, a.path_bytes - 1) || memchr(name, 0, a.name_bytes - 1) || !wr_name(name)) { r.error = 87; goto done; }
    if(a.op==W32R_COMPAT){
        struct wr_key *k=s->roots[0];
        const char *parts[]={"Software","CiukiOS","Win32","AppCompat"};
        for(unsigned i=0;k&&i<4;i++)k=wr_child(s,k,parts[i]);
        struct wr_value *v=k?value(k,"Database"):NULL;
        if(!v)r.error=2;
        else if(v->type!=3||v->bytes>65536||cap<sizeof(r)+v->bytes)r.error=13;
        else {r.type=3;r.data_bytes=v->bytes;memcpy(bytes,v->data,v->bytes);}goto done;
    }
    if (a.op==W32R_PROFILE_ACQUIRE || a.op==W32R_PROFILE_RELEASE) {
        /* One global INI writer transaction. The service loop serializes all
         * authenticated clients; disconnect/death releases the lease. */
        if (*path || *name || a.key || a.index || a.type || a.access || a.flags || a.data_bytes) r.error=87;
        else if (a.op==W32R_PROFILE_ACQUIRE) {
            if (s->profile_owner) r.error=170;
            else s->profile_owner=pid;
        } else if (s->profile_owner!=pid) r.error=5;
        else s->profile_owner=0;
        goto done;
    }
    if (a.op >= W32R_ATOM_ADD) {
        if (cap < sizeof(r) + W32R_DATA + W32R_NAME) { r.error = 87; goto done; }
        r.error = wr_directory_call(s, pid, &a, name, data, &r, bytes); goto done;
    }
    if (a.op == W32R_USERNAME) {
        if (cap < sizeof(r) + 6) { r.error = 87; goto done; }
        memcpy(bytes, "Ciuki", 6); r.data_bytes = 6; goto done;
    }
    struct wr_key *k = NULL;
    uint32_t required = a.op == W32R_SET || a.op == W32R_DELETE_VALUE ? 2 :
                        a.op == W32R_QUERY || a.op == W32R_ENUM_VALUE || a.op == W32R_INFO ? 1 :
                        a.op == W32R_ENUM_KEY ? 8 : a.op == W32R_CREATE ? 4 : a.op == W32R_DELETE_KEY ? 0x10000 : 0;
    if (a.op == W32R_CLOSE) {
        if (a.key & 0x80000000u) { r.error = resolve(s, pid, a.key, 0, &k); goto done; }
        struct wr_handle **at = &s->handles;
        while (*at && ((*at)->pid != pid || (*at)->id != a.key)) at = &(*at)->next;
        if (!*at) r.error = 6;
        else { struct wr_handle *h = *at; *at = h->next; free(h); collect_deleted(s); }
        goto done;
    }
    r.error = resolve(s, pid, a.key, required, &k); if (r.error) goto done;
    if(pid!=s->compat_writer_pid &&
       (a.op==W32R_CREATE||a.op==W32R_SET||a.op==W32R_DELETE_VALUE||a.op==W32R_DELETE_KEY)&&
       compat_protected(s,k,(a.op==W32R_CREATE||a.op==W32R_DELETE_KEY)?path:"",a.op==W32R_DELETE_KEY)){
        r.error=5;goto done;
    }
    if (a.op == W32R_OPEN || a.op == W32R_CREATE) {
        if (a.flags || a.type || (a.access & ~0x000f003fu)) { r.error = 87; goto done; }
        if (a.op == W32R_OPEN && !*path && (a.key & 0x80000000u)) { r.key = a.key; goto done; }
        r.error = walk(s, k, path, a.op == W32R_CREATE, &k, &r.disposition); if (r.error) goto done;
        struct wr_handle *h = calloc(1, sizeof(*h)); if (!h) { r.error = 8; goto done; }
        h->id = wr_new_id(s);
        if (!h->id) { free(h); r.error = 1016; goto done; }
        h->pid = pid; h->key = k; h->access = a.access; h->next = s->handles; s->handles = h; r.key = h->id;
    } else if (a.op == W32R_FLUSH) r.error = wr_flush(s);
    else if (a.op == W32R_SET) r.error = k->hive > 1 ? 5 : wr_put(s, k, name, a.type, data, a.data_bytes);
    else if (a.op == W32R_QUERY) {
        struct wr_value *v = value(k, name);
        if (!v) r.error = 2;
        else if (cap < sizeof(r) + v->bytes) r.error = 87;
        else { r.type = v->type; r.data_bytes = v->bytes; if (v->bytes) memcpy(bytes, v->data, v->bytes); }
    } else if (a.op == W32R_DELETE_VALUE) {
        if (k->hive > 1) { r.error = 5; goto done; }
        struct wr_value **at = &k->values; while (*at && !wr_equal((*at)->name, name)) at = &(*at)->next;
        if (!*at) r.error = 2;
        else { struct wr_value *v = *at; *at = v->next; s->hive[k->hive].text -= value_cost(v); v->next = NULL; free_values(v); dirty(s, k->hive); }
    } else if (a.op == W32R_DELETE_KEY) {
        /* f5-20B (directive f5-20 amendment 1, item 3): an empty name
         * deletes the key of an open handle (NtDeleteKey), which `resolve`
         * checked for DELETE access. A predefined key (a root constant, or a
         * handle opened on the key a root names) is never deleted. */
        if ((!*path && ((a.key & 0x80000000u) || predefined(s, k))) || k->hive > 1) { r.error = 5; goto done; }
        uint32_t d; r.error = walk(s, k, path, 0, &k, &d); if (r.error) goto done;
        for (struct wr_key *x = s->keys; x; x = x->next) if (!x->deleted && descendant(x, k)) {
            s->hive[x->hive].text -= key_path_size(x) + 3;
            for (struct wr_value *v = x->values; v; v = v->next) s->hive[x->hive].text -= value_cost(v);
            x->deleted = 1;
        }
        dirty(s, k->hive); collect_deleted(s);
    } else if (a.op == W32R_ENUM_KEY) {
        struct wr_key *found = NULL; uint32_t i = 0;
        for (struct wr_key *x = s->keys; x; x = x->next) if (!x->deleted && x->parent == k && i++ == a.index) { found = x; break; }
        if (!found) r.error = 259;
        else { r.name_bytes = strlen(found->name); if (cap < sizeof(r) + r.name_bytes + 1) r.error = 87; else memcpy(bytes, found->name, r.name_bytes + 1); }
    } else if (a.op == W32R_ENUM_VALUE) {
        struct wr_value *v = k->values; for (uint32_t i = 0; v && i < a.index; i++) v = v->next;
        if (!v) r.error = 259;
        else {
            r.name_bytes = strlen(v->name); r.type = v->type; r.data_bytes = v->bytes;
            if (cap < sizeof(r) + r.name_bytes + 1 + v->bytes) r.error = 87;
            else { memcpy(bytes, v->name, r.name_bytes + 1); if (v->bytes) memcpy(bytes + r.name_bytes + 1, v->data, v->bytes); }
        }
    } else if (a.op == W32R_INFO) {
        for (struct wr_key *x = s->keys; x; x = x->next) if (!x->deleted && x->parent == k) { r.count++; uint32_t len = characters(x->name, a.flags != 0); if (len > r.max_name) r.max_name = len; }
        for (struct wr_value *v = k->values; v; v = v->next) {
            r.disposition++; uint32_t len = characters(v->name, a.flags != 0); if (len > r.name_bytes) r.name_bytes = len;
            uint32_t bytes = !a.flags && (v->type == 1 || v->type == 2 || v->type == 7) ? (v->bytes + 1) / 2 : v->bytes;
            if (bytes > r.max_data) r.max_data = bytes;
        }
        /* INFO name_bytes is metadata, not an output tail. */
        r.key = r.name_bytes; r.name_bytes = 0;
    } else r.error = 120;
done:
    if (r.error) r.data_bytes = r.name_bytes = 0;
    *used = sizeof(r) + (r.name_bytes || (n >= sizeof(a) && a.op == W32R_ENUM_VALUE && !r.error) ? r.name_bytes + 1 : 0) + r.data_bytes;
    memcpy(output, &r, sizeof(r)); return 0;
}
