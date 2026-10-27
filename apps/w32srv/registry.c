/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Single-threaded authenticated owner. */
#define _POSIX_C_SOURCE 200809L
#include "registry.h"
#include "../desktop/session_profile.h"
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
uint32_t wr_remove_value(struct wr_server *s, struct wr_key *k, const char *name)
{
    if (k->hive > 1) return 5;
    struct wr_value **at = &k->values; while (*at && !wr_equal((*at)->name, name)) at = &(*at)->next;
    if (!*at) return 2;
    struct wr_value *v = *at; *at = v->next; s->hive[k->hive].text -= value_cost(v); v->next = NULL; free_values(v); dirty(s, k->hive);
    return 0;
}
struct wr_key *wr_walk(struct wr_server *s, unsigned hive, const char *path)
{
    struct wr_key *k = hive < 4 ? s->roots[hive] : NULL; char part[W32R_NAME];
    while (k && path && *path) {
        const char *e = strchr(path, '\\'); size_t n = e ? (size_t)(e - path) : strlen(path);
        if (!n || n >= sizeof(part)) return NULL;
        memcpy(part, path, n); part[n] = 0; k = wr_child(s, k, part);
        if (!e) break; path = e + 1;
    }
    return k;
}
uint32_t wr_client_join(struct wr_server *s,uint32_t pid)
{
    if(!pid)return 87;
    for(unsigned i=0;i<s->live_count;i++)if(s->live[i]==pid)return 0;
    if(s->live_count>=WR_LIVE)return 8;
    /* A process-local identity in the existing durable SERVICE.CRS store
     * survives a service crash even for a program with no USER windows.
     * Reconcile checks the OS process snapshot before restoring it. */
    struct w32r_request a={.op=W32R_ATOM_FIND,.index=1};struct w32r_reply r={0};uint8_t bytes[W32R_NAME];
    uint32_t e=wr_directory_call(s,pid,&a,WR_CLIENT_MARKER,NULL,&r,bytes);
    if(e==6){a.op=W32R_ATOM_ADD;e=wr_directory_call(s,pid,&a,WR_CLIENT_MARKER,NULL,&r,bytes);}
    if(e)return e;
    s->live[s->live_count++]=pid;return 0;
}
void wr_destroy(struct wr_server *s)
{
    free(s->ending); free(s->power); free(s->power_query);
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
int wr_init_secure(struct wr_server *s,const struct wr_io *io,uint64_t now)
{
    memset(s,0,sizeof(*s));s->io=*io;s->now=now;s->secured=1;
    strcpy(s->user_branch,".DEFAULT"); /* SYSTEM logon/template context only */
    if(!wr_load(s)){wr_destroy(s);return 0;}
    return 1;
}
uint32_t wr_flush(struct wr_server *s)
{
    if(s->session_only)return 0;
    for (unsigned h = 0; h < 2; h++) { uint32_t e = wr_commit(s, h); if (e) return e; }
    return 0;
}
uint32_t wr_tick(struct wr_server *s, uint64_t now)
{
    s->now = now;
    if(s->session_only)return 0;
    for (unsigned h = 0; h < 2; h++) if (s->hive[h].dirty && now >= s->hive[h].dirty_since && now - s->hive[h].dirty_since >= 2000) {
        uint32_t e = wr_commit(s, h); if (e) return e;
    }
    return 0;
}
static void collect_deleted(struct wr_server *);
uint32_t wr_exit(struct wr_server *s, uint32_t pid)
{
    if (s->profile_owner==pid) s->profile_owner=0;
    if (s->shell_exit) s->shell_exit(s->shell, pid);
    /* A requester that dies before the desktop consumes its request still
     * ends the session: Windows proceeds once ExitWindowsEx has returned,
     * so s->session is deliberately not tied to the requester's lifetime. */
    for (unsigned i = 0; i < s->live_count; i++) if (s->live[i] == pid) { s->live[i] = s->live[--s->live_count]; break; }
    uint32_t e = wr_flush(s);
    struct wr_handle **at = &s->handles;
    while (*at) { struct wr_handle *h = *at; if (h->pid == pid) { *at = h->next; free(h); } else at = &h->next; }
    collect_deleted(s);
    uint32_t d = s->directory&&!s->secured?wr_directory_exit(s, pid):0; return e ? e : d;
}
static int wr_administrator(const struct wr_server *s)
{
    if(!s->subject.uid)return 1;
    for(unsigned i=0;i<s->subject.group_count&&i<8;i++)if(s->subject.groups[i]==544)return 1;
    return 0;
}
uint32_t wr_key_policy(struct wr_server *s,struct wr_key *k,uint32_t access)
{
    if(!s->secured)return 0;
    if(!k||s->subject.group_count>8)return 5;
    int write=!!(access&~0x20019u);
    if(k->hive>1)return write?5:0;
    if(k->hive==0){
        struct wr_key *top=k;while(top->parent&&top->parent->parent)top=top->parent;
        if(top->parent&&(wr_equal(top->name,"SAM")||wr_equal(top->name,"SECURITY")))return 5;
        if(top->parent&&wr_equal(top->name,"HARDWARE"))return write?5:0;
        return write&&!wr_administrator(s)?5:0;
    }
    struct wr_key *top=k;while(top->parent&&top->parent->parent)top=top->parent;
    if(!top->parent)return 0; /* prospective child is checked before traversal */
    if(wr_equal(top->name,".DEFAULT"))return s->subject.uid?5:0;
    if(!wr_equal(top->name,s->user_branch)&&!wr_administrator(s))return 5;
    return 0;
}
static uint32_t resolve(struct wr_server *s, uint32_t pid, uint32_t id, uint32_t access, struct wr_key **key)
{
    if (id == W32R_HKLM) *key = s->roots[0];
    else if (id == W32R_HKU) *key = s->roots[1];
    else if (id == W32R_HKCU) *key = wr_child(s, s->roots[1], s->secured?s->user_branch:".DEFAULT");
    else if (id == W32R_HKCR) {
        struct wr_key *software = wr_child(s, s->roots[0], "Software");
        *key = software ? wr_child(s, software, "Classes") : NULL;
    } else if (id == W32R_HKCC) *key = s->roots[2];
    else if (id == W32R_HKDD) *key = s->roots[3];
    else {
        for (struct wr_handle *h = s->handles; h; h = h->next) if (h->id == id && h->pid == pid) {
            if (h->key->deleted) return 1018;
            if ((h->access & access) != access) return 5;
            *key = h->key; return wr_key_policy(s,*key,access);
        }
        return 6;
    }
    return *key ? wr_key_policy(s,*key,access) : 2;
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
        struct wr_key prospective={.parent=k,.name=component,.hive=k->hive};
        uint32_t denied=wr_key_policy(s,&prospective,create?4u:0u);if(denied)return denied;
        struct wr_key *next = wr_child(s, k, component);
        if(s->secured&&!next&&k==s->roots[1])return 2; /* only broker-loaded hives */
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
           (s->secured&&k==wr_child(s,s->roots[1],s->user_branch)) ||
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
#include "session_impl.h"
/* f5-07B end-session requests and the desktop-driven virtual restart.
 * The requester is any authenticated Win32 process; the restart actions
 * belong to the supervising desktop, the only authority that can close the
 * personality's processes (compat_writer_pid, f5-09B). */
static uint32_t shutdown_call(struct wr_server *s, uint32_t pid, const struct w32r_request *a, const char *path,
                              struct w32r_reply *r, uint8_t *bytes, size_t cap)
{
    if (a->key || a->data_bytes) return 87;
    if (a->index == W32R_SHUTDOWN_REQUEST) {
        if (a->flags & ~0x1fu || a->type > 315360000u || strlen(path) >= sizeof(s->session.message)) return 87;
        if (s->session.pid) return 1115; /* ERROR_SHUTDOWN_IN_PROGRESS */
        s->session.pid = pid; s->session.flags = a->flags;
        s->session.due = s->now + (uint64_t)a->type * 1000;
        memcpy(s->session.message, path, strlen(path) + 1);
        return 0;
    }
    if (a->index == W32R_SHUTDOWN_ABORT) {
        if (a->flags || a->type || *path) return 87;
        if (!s->session.pid || s->session.due <= s->now) return 1116; /* ERROR_NO_SHUTDOWN_IN_PROGRESS */
        memset(&s->session, 0, sizeof(s->session)); return 0;
    }
    if (pid != s->compat_writer_pid || !pid) return 5;
    if (a->index == W32R_SHUTDOWN_POLL) {
        if ((a->flags & ~1u) || a->type || *path) return 87;
        if ((a->flags & 1u) && s->shell_call) { /* pending-work check: stats WININIT.INI */
            struct w32r_request status = {.version = W32R_VERSION, .op = W32R_SHUTDOWN, .index = W32R_SHUTDOWN_STATUS, .path_bytes = 1, .name_bytes = 1};
            struct w32r_reply probe = {.version = W32R_VERSION};
            if (!s->shell_call(s->shell, pid, &status, "", "", &probe, bytes, cap)) r->disposition = probe.disposition;
        }
        if (!s->session.pid) return 0;
        if (s->session.due > s->now) { r->count = (uint32_t)(s->session.due - s->now); r->key = s->session.pid; r->type = s->session.flags; return 0; }
        size_t n = strlen(s->session.message) + 1;
        if (cap < n) return 87;
        r->key = s->session.pid; r->type = s->session.flags; r->data_bytes = (uint32_t)n; memcpy(bytes, s->session.message, n);
        memset(&s->session, 0, sizeof(s->session)); return 0;
    }
    if (a->index == W32R_SHUTDOWN_LIST) {
        if (a->flags || a->type || *path) return 87;
        if (cap < 4u * WR_LIVE) return 87;
        for (unsigned i = 0; i < s->live_count; i++)
            for (unsigned b = 0; b < 4; b++) bytes[i * 4 + b] = (uint8_t)(s->live[i] >> (8 * b));
        r->count = s->live_count; r->data_bytes = s->live_count * 4;
        return 0;
    }
    if (a->index < W32R_SHUTDOWN_STATUS || a->index > W32R_SHUTDOWN_END) return 87;
    return s->shell_call ? s->shell_call(s->shell, pid, a, path, "", r, bytes, cap) : 120;
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
    if(s->session_only&&(a.op<=W32R_FLUSH||a.op==DESK_W32_PROFILE)){r.error=5;goto done;}
    if(s->session_only&&a.op==W32R_COMPAT){
        if(!s->compat_rpc){r.error=1062;goto done;}
        return s->compat_rpc(input,n,output,cap,used);
    }
    if (a.op==DESK_W32_PROFILE) {
        if (s->secured || pid!=s->compat_writer_pid) r.error=5;
        else if (a.key || a.index || a.type || a.access || a.flags || a.data_bytes) r.error=87;
        else r.error=wr_select_profile(s,name,path);
        goto done;
    }
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
    if (a.op >= W32R_FONT_GLYPHS && a.op <= W32R_FONT_STATS) {
        r.error = s->font_call ? s->font_call(s->fonts, pid, &a, path, data, &r, bytes, cap - sizeof(r)) : 120;
        goto done;
    }
    if (a.op == W32R_CLIENT) {
        if (a.key || a.index || a.type || a.flags || a.data_bytes || *path || *name) r.error=87;
        else if(s->ending && !wr_end_live(s,pid))r.error=1115;
        else if(s->live_count==WR_LIVE && !wr_end_live(s,pid)) r.error=8;
        else r.error=wr_client_join(s,pid);
        goto done;
    }
    if (a.op == W32R_ENDSESSION) {r.error=wr_end_call(s,pid,&a,path,name,data,&r);goto done;}
    if (a.op >= W32R_PROGMAN_EXECUTE && a.op <= W32R_PROGMAN_OPEN) {
        r.error = s->shell_call ? s->shell_call(s->shell, pid, &a, path, name, &r, bytes, cap - sizeof(r)) : 120;
        goto done;
    }
    if (a.op == W32R_SHUTDOWN) { r.error = shutdown_call(s, pid, &a, path, &r, bytes, cap - sizeof(r)); goto done; }
    if (a.op >= W32R_ATOM_ADD) {
        if (cap < sizeof(r) + W32R_DATA + W32R_NAME) { r.error = 87; goto done; }
        r.error = wr_directory_call(s, pid, &a, name, data, &r, bytes); goto done;
    }
    if (a.op == W32R_USERNAME) {
        if (!s->user_name[0]) { r.error=1245; goto done; } /* ERROR_NOT_LOGGED_ON, Windows 9x */
        size_t len=strlen(s->user_name)+1;
        if (cap < sizeof(r)+len) { r.error=87; goto done; }
        memcpy(bytes,s->user_name,len); r.data_bytes=(uint32_t)len; goto done;
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
    if(s->secured&&k==s->roots[1]&&a.op!=W32R_OPEN&&a.op!=W32R_CREATE&&required&~0x20019u){r.error=5;goto done;}
    if((s->secured ? !wr_administrator(s) : pid!=s->compat_writer_pid) &&
       (a.op==W32R_CREATE||a.op==W32R_SET||a.op==W32R_DELETE_VALUE||a.op==W32R_DELETE_KEY)&&
       compat_protected(s,k,(a.op==W32R_CREATE||a.op==W32R_DELETE_KEY)?path:"",a.op==W32R_DELETE_KEY)){
        r.error=5;goto done;
    }
    if (a.op == W32R_OPEN || a.op == W32R_CREATE) {
        if (a.flags || a.type || (a.access & ~0x000f003fu)) { r.error = 87; goto done; }
        r.error=wr_key_policy(s,k,a.access);if(r.error)goto done;
        r.error = walk(s, k, path, a.op == W32R_CREATE, &k, &r.disposition); if (r.error) goto done;
        r.error=wr_key_policy(s,k,a.access);if(r.error)goto done;
        if(s->secured&&k==s->roots[1]&&(a.access&~0x20019u)){r.error=5;goto done;}
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
    } else if (a.op == W32R_DELETE_VALUE) r.error = wr_remove_value(s, k, name);
    else if (a.op == W32R_DELETE_KEY) {
        /* f5-20B (directive f5-20 amendment 1, item 3): an empty name
         * deletes the key of an open handle (NtDeleteKey), which `resolve`
         * checked for DELETE access. A predefined key (a root constant, or a
         * handle opened on the key a root names) is never deleted. */
        if ((!*path && ((a.key & 0x80000000u) || predefined(s, k))) || k->hive > 1) { r.error = 5; goto done; }
        uint32_t d; r.error = walk(s, k, path, 0, &k, &d); if (r.error) goto done;
        if(s->secured&&predefined(s,k)){r.error=5;goto done;}
        for (struct wr_key *x = s->keys; x; x = x->next) if (!x->deleted && descendant(x, k)) {
            s->hive[x->hive].text -= key_path_size(x) + 3;
            for (struct wr_value *v = x->values; v; v = v->next) s->hive[x->hive].text -= value_cost(v);
            x->deleted = 1;
        }
        dirty(s, k->hive); collect_deleted(s);
    } else if (a.op == W32R_ENUM_KEY) {
        struct wr_key *found = NULL; uint32_t i = 0;
        for (struct wr_key *x = s->keys; x; x = x->next) if (!x->deleted && x->parent == k && !wr_key_policy(s,x,0) && i++ == a.index) { found = x; break; }
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
        for (struct wr_key *x = s->keys; x; x = x->next) if (!x->deleted && x->parent == k && !wr_key_policy(s,x,0)) { r.count++; uint32_t len = characters(x->name, a.flags != 0); if (len > r.max_name) r.max_name = len; }
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
