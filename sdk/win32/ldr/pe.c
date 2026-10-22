/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. F5 policy over shared PE mechanism.
 * LGPL-2.1-or-later (section 3) permits the GPL-2.0-only kernel consumer. */
#include <w32/ldr.h>
static int fail(struct w32_pe *p,const char *why) { return pe_fail(p,why); }
static int within(ULONG start,ULONG n,ULONG limit) { return start<=limit && n<=limit-start; }
#define CHECK(c,r) do { if(!(c)) return fail(p,r); } while(0)
static void diagnostic(void *ctx,const char *why) { (void)ctx; w32_diag("load","image",why); }
static int user_policy(void *ctx,struct pe_image *p) {
    (void)ctx; return p->subsystem==2 || p->subsystem==3 || fail(p,"unsupported-subsystem");
}
static int writable(void *ctx,struct pe_image *p,ULONG rva,ULONG bytes) {
    (void)ctx;
    const struct w32_services *s=w32_get_services();
    if(!s || !s->image) return 1;
    ULONG start=rva&~4095u, end=(rva+bytes+4095)&~4095u;
    return s->protect(s->opaque,p->image+start,end-start,7) ? 1 : fail(p,"image-protect-failed");
}
static const struct pe_ops user_ops={.admit=user_policy,.diagnostic=diagnostic,.writable=writable};
int w32_pe_parse(struct w32_pe *p,const void *bytes,ULONG size,struct w32_error *e) {
    return pe_parse(p,bytes,size,e,&user_ops);
}
int w32_pe_copy_image(struct w32_pe *p,void *image) { return pe_copy_image(p,image); }
void *w32_pe_span(struct w32_pe *p,ULONG rva,ULONG n) { return pe_span(p,rva,n); }
const char *w32_pe_string(struct w32_pe *p,ULONG rva) { return pe_string(p,rva); }
int w32_pe_relocate(struct w32_pe *p,ULONG base) { return pe_relocate(p,base); }
int w32_pe_write(struct w32_pe *p,ULONG rva,ULONG n) { return pe_write(p,rva,n); }
static int tls(struct w32_pe *p) {
    struct w32_directory d = p->dirs[9];
    if (!d.size)
        return 1;
    CHECK(d.size >= 24, "tls-directory-size");
    uint8_t *t = w32_pe_span(p, d.rva, 24);
    if (!t)
        return 0;
    ULONG start = w32_u32(t), end = w32_u32(t + 4), index = w32_u32(t + 8),
          callbacks = w32_u32(t + 12);
    CHECK(end >= start &&
              (!start || (start >= p->base && (uint64_t)end <= (uint64_t)p->base + p->size)),
          "tls-template-bounds");
    CHECK(index >= p->base && index - p->base <= p->size - 4 && !(index & 3), "tls-index-bounds");
    if (!w32_pe_span(p, index - p->base, 4))
        return 0;
    if (start && !w32_pe_span(p, start - p->base, end - start))
        return 0;
    if (callbacks) {
        CHECK(callbacks >= p->base && !(callbacks & 3), "tls-callback-alignment");
        for (ULONG i = 0; i < 4096; i++) {
            uint8_t *f = w32_pe_span(p, callbacks - p->base + i * 4, 4);
            if (!f)
                return 0;
            ULONG a = w32_u32(f);
            if (!a)
                return 1;
            CHECK(a >= p->base && a - p->base < p->size, "tls-callback-bounds");
            if (!w32_pe_span(p, a - p->base, 1))
                return 0;
        }
        return fail(p, "tls-callbacks-unterminated");
    }
    return 1;
}
/* Resource traversal and selection share one bounded walker. */
struct resource_query {
    struct w32_resource_key key[2];
    ULONG language;
    const void *data;
    ULONG size;
    int found;
};
static uint8_t *relative(struct w32_pe *p, ULONG off, ULONG n) {
    struct w32_directory d = p->dirs[2];
    if (!within(off, n, d.size)) {
        fail(p, "resource-directory-bounds");
        return 0;
    }
    return w32_pe_span(p, d.rva + off, n);
}
static int key_matches(struct w32_pe *p, ULONG value, struct w32_resource_key k) {
    if (!(value & 0x80000000))
        return !k.name && value == k.id;
    uint8_t *s = relative(p, value & 0x7fffffff, 2);
    if (!s)
        return 0;
    ULONG n = w32_u16(s);
    s = relative(p, (value & 0x7fffffff) + 2, n * 2);
    if (!s)
        return 0;
    if (!k.name)
        return 0;
    ULONG i = 0;
    while (i < n && k.name[i] == w32_u16(s + i * 2))
        i++;
    return i == n && !k.name[i];
}
static int resource_walk(struct w32_pe *p, ULONG off, ULONG depth, ULONG *active, ULONG *budget,
                         struct resource_query *q, int match) {
    for (ULONG i = 0; i < depth; i++)
        CHECK(active[i] != off, "resource-cycle");
    CHECK(depth < 3, "resource-depth");
    active[depth] = off;
    uint8_t *head = relative(p, off, 16);
    if (!head)
        return 0;
    ULONG count = (ULONG)w32_u16(head + 12) + w32_u16(head + 14);
    uint8_t *entries = relative(p, off + 16, count * 8);
    if (!entries)
        return 0;
    for (ULONG i = 0; i < count; i++) {
        CHECK(*budget, "resource-count-limit");
        (*budget)--;
        ULONG name = w32_u32(entries + i * 8), ptr = w32_u32(entries + i * 8 + 4);
        if (name & 0x80000000) {
            ULONG at = name & 0x7fffffff;
            uint8_t *s = relative(p, at, 2);
            if (!s)
                return 0;
            ULONG n = w32_u16(s);
            s = relative(p, at + 2, n * 2);
            if (!s)
                return 0;
            for (ULONG j = 0; j < n; j++) {
                ULONG c = w32_u16(s + j * 2);
                if (c >= 0xd800 && c <= 0xdbff) {
                    CHECK(j + 1 < n, "resource-name-encoding");
                    ULONG low = w32_u16(s + (++j) * 2);
                    CHECK(low >= 0xdc00 && low <= 0xdfff, "resource-name-encoding");
                } else
                    CHECK(c < 0xdc00 || c > 0xdfff, "resource-name-encoding");
            }
        }
        int selected =
            match && (!q || (depth < 2 ? key_matches(p, name, q->key[depth])
                                       : !(name & 0x80000000) &&
                                             (q->language == 0xffffffffu || q->language == name)));
        if (ptr & 0x80000000) {
            if (!resource_walk(p, ptr & 0x7fffffff, depth + 1, active, budget, q, selected))
                return 0;
        } else {
            CHECK(depth == 2, "resource-leaf-depth");
            uint8_t *d = relative(p, ptr, 16);
            if (!d)
                return 0;
            ULONG r = w32_u32(d), n = w32_u32(d + 4);
            void *data = w32_pe_span(p, r, n);
            if (!data)
                return 0;
            if (q && selected && !q->found) {
                q->data = data;
                q->size = n;
                q->found = 1;
            }
        }
    }
    return 1;
}
static int resources(struct w32_pe *p, struct resource_query *q) {
    if (!p->dirs[2].size)
        return 1;
    ULONG active[3], budget = 4096;
    return resource_walk(p, 0, 0, active, &budget, q, 1);
}
int w32_pe_resource(struct w32_pe *p, struct w32_resource_key type, struct w32_resource_key name,
                    ULONG lang, const void **data, ULONG *size) {
    struct resource_query q = {{type, name}, lang, 0, 0, 0};
    if (!resources(p, &q) || !q.found)
        return 0;
    *data = q.data;
    *size = q.size;
    return 1;
}
/* Direct lookup for the Ldr resource exports. The whole tree was validated
 * (bounds, depth, cycles) when the image was mapped; every read here is still
 * bounded. levels 1-2 return the matching subdirectory, level 3 the
 * IMAGE_RESOURCE_DATA_ENTRY. A key with name 0 and id 0xffffffff selects the
 * first entry of its level. */
int w32_pe_resource_lookup(struct w32_pe *p, const struct w32_resource_key *keys, ULONG levels,
                           void **out) {
    if (!p->dirs[2].size || !levels || levels > 3)
        return 0;
    ULONG off = 0;
    for (ULONG level = 0; level < levels; level++) {
        uint8_t *head = relative(p, off, 16);
        if (!head)
            return 0;
        ULONG named = w32_u16(head + 12), count = named + w32_u16(head + 14);
        uint8_t *entries = relative(p, off + 16, count * 8);
        if (!entries)
            return 0;
        ULONG found = 0xffffffffu;
        for (ULONG i = 0; i < count && found == 0xffffffffu; i++) {
            ULONG name = w32_u32(entries + i * 8);
            if ((!keys[level].name && keys[level].id == 0xffffffffu) ||
                key_matches(p, name, keys[level]))
                found = w32_u32(entries + i * 8 + 4);
        }
        if (found == 0xffffffffu)
            return 0;
        int directory = (found & 0x80000000) != 0;
        if (directory != (level + 1 < 3))
            return fail(p, "resource-leaf-depth");
        off = found & 0x7fffffff;
        if (!relative(p, off, 16))
            return 0;
    }
    *out = p->image + p->dirs[2].rva + off;
    return 1;
}
int w32_pe_validate(struct w32_pe *p) {
    return resources(p, 0) && pe_validate_mechanism(p) && tls(p);
}
int w32_pe_protect(struct w32_pe *p, const struct w32_services *s) {
    ULONG total = (p->size + 4095) & ~4095u;
    for (ULONG page = 0; page < total; page += 4096) {
        ULONG protection = page < p->headers ? W32_PROT_READ : 0;
        for (ULONG i = 0; i < p->section_count; i++) {
            struct w32_section *a = &p->sections[i];
            if (a->span && page < a->rva + a->span && page + 4096 > a->rva) {
                if (a->flags & 0x40000000)
                    protection |= W32_PROT_READ;
                if (a->flags & 0x80000000)
                    protection |= W32_PROT_WRITE;
                if (a->flags & 0x20000000)
                    protection |= W32_PROT_EXEC;
            }
        }
        if (!s->protect(s->opaque, p->image + page, 4096, protection))
            return fail(p, "image-protect-failed");
    }
    return 1;
}
