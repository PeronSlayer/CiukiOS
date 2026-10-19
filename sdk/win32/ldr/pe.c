/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Original bounded PE32 parser. */
#include <w32/ldr.h>
static int fail(struct w32_pe *p, const char *why) {
    if (!p->error || !p->error->reason) {
        if (p->error) {
            p->error->code = 193;
            p->error->reason = why;
        }
        w32_diag("load", "image", why);
    }
    return 0;
}
static int within(ULONG start, ULONG n, ULONG limit) {
    return start <= limit && n <= limit - start;
}
static int power2(ULONG n) {
    return n && !(n & (n - 1));
}
#define CHECK(c, r)                                                                                \
    do {                                                                                           \
        if (!(c))                                                                                  \
            return fail(p, r);                                                                     \
    } while (0)
int w32_pe_parse(struct w32_pe *p, const void *bytes, ULONG size, struct w32_error *e) {
    w32_fill(p, 0, sizeof(*p));
    p->file = bytes;
    p->file_size = size;
    p->error = e;
    if (e) {
        e->code = 0;
        e->reason = 0;
    }
    const uint8_t *f = bytes;
    CHECK(size >= 64, "dos-header-truncated");
    CHECK(f[0] == 'M' && f[1] == 'Z', "dos-magic");
    ULONG off = w32_u32(f + 60);
    CHECK(!(off & 3), "pe-offset-alignment");
    CHECK(off >= 64 && within(off, 24, size), "pe-header-bounds");
    CHECK(w32_u32(f + off) == 0x4550, "pe-signature");
    CHECK(w32_u16(f + off + 4) == 0x14c, "machine-not-i386");
    p->section_count = w32_u16(f + off + 6);
    CHECK(p->section_count && p->section_count <= 96, "section-count");
    p->flags = w32_u16(f + off + 22);
    CHECK(p->flags & 2, "not-executable-image");
    ULONG o = off + 24, olen = w32_u16(f + off + 20);
    CHECK(olen >= 96 && within(o, olen, size), "optional-header-bounds");
    CHECK(w32_u16(f + o) == 0x10b, "optional-magic-not-pe32");
    ULONG nd = w32_u32(f + o + 92);
    CHECK(nd <= 16 && olen >= 96 + nd * 8, "directory-count");
    p->preferred = p->base = w32_u32(f + o + 28);
    p->entry = w32_u32(f + o + 16);
    ULONG sa = w32_u32(f + o + 32), fa = w32_u32(f + o + 36);
    p->section_alignment = sa;
    CHECK(power2(fa) && ((fa >= 512 && fa <= 65536) || (sa < 4096 && fa == sa)), "file-alignment");
    CHECK(power2(sa) && sa >= fa, "section-alignment");
    p->size = w32_u32(f + o + 56);
    p->headers = w32_u32(f + o + 60);
    CHECK(p->size && p->size <= 0x20000000 && !(p->size & (sa - 1)), "image-size");
    CHECK((uint64_t)p->preferred + p->size <= 0x100000000ull, "image-address-wrap");
    ULONG table = o + olen;
    CHECK(within(table, p->section_count * 40, size), "section-table-bounds");
    CHECK(p->headers >= table + p->section_count * 40 && p->headers <= size &&
              p->headers <= p->size,
          "headers-size");
    p->subsystem = w32_u16(f + o + 68);
    CHECK(p->subsystem == 2 || p->subsystem == 3, "unsupported-subsystem");
    if (w32_u16(f + o + 48) > 5)
        w32_diag("load", "image", "newer-subsystem-version");
    CHECK(p->entry < p->size, "entrypoint-bounds");
    for (ULONG i = 0; i < p->section_count; i++) {
        const uint8_t *t = f + table + i * 40;
        struct w32_section *s = &p->sections[i];
        ULONG vs = w32_u32(t + 8);
        s->rva = w32_u32(t + 12);
        s->raw = w32_u32(t + 16);
        s->offset = w32_u32(t + 20);
        s->flags = w32_u32(t + 36);
        s->span = vs > s->raw ? vs : s->raw;
        CHECK(within(s->rva, s->span, p->size), "section-image-bounds");
        CHECK(!(s->rva & (sa - 1)), "section-rva-alignment");
        CHECK(!s->raw || !(s->offset & (fa - 1)), "section-raw-alignment");
        CHECK((uint64_t)s->offset + s->raw <= 0x100000000ull, "section-raw-wrap");
        CHECK(!s->span || s->rva >= p->headers, "section-header-overlap");
        for (ULONG j = 0; j < i; j++) {
            struct w32_section *a = &p->sections[j];
            CHECK(!s->span || !a->span || s->rva >= a->rva + a->span || s->rva + s->span <= a->rva,
                  "section-virtual-overlap");
        }
        if (s->raw && (uint64_t)s->offset + s->raw > size)
            w32_diag("load", "image", "raw-data-zero-filled");
    }
    for (ULONG i = 0; i < nd; i++) {
        struct w32_directory *d = &p->dirs[i];
        d->rva = w32_u32(f + o + 96 + i * 8);
        d->size = w32_u32(f + o + 100 + i * 8);
        if (d->size) {
            if (i == 4)
                CHECK(!(d->rva & 7) && within(d->rva, d->size, size), "certificate-file-bounds");
            else
                CHECK(d->rva && within(d->rva, d->size, p->size), "directory-image-bounds");
        }
    }
    return 1;
}
int w32_pe_copy_image(struct w32_pe *p, void *image) {
    p->image = image;
    if (!image)
        return fail(p, "image-map-failed");
    w32_fill(image, 0, p->size);
    w32_copy(image, p->file, p->headers);
    for (ULONG i = 0; i < p->section_count; i++) {
        struct w32_section *s = &p->sections[i];
        ULONG n = s->raw;
        if (s->offset >= p->file_size)
            n = 0;
        else if (n > p->file_size - s->offset)
            n = p->file_size - s->offset;
        if (n)
            w32_copy(p->image + s->rva, p->file + s->offset, n);
    }
    return 1;
}
void *w32_pe_span(struct w32_pe *p, ULONG rva, ULONG n) {
    if (!within(rva, n, p->size)) {
        fail(p, "rva-bounds");
        return 0;
    }
    ULONG at = rva, left = n;
    while (left) {
        ULONG take = 0;
        if (at < p->headers)
            take = p->headers - at;
        else
            for (ULONG i = 0; i < p->section_count; i++) {
                struct w32_section *s = &p->sections[i];
                if (at >= s->rva && at - s->rva < s->span) {
                    take = s->span - (at - s->rva);
                    break;
                }
            }
        if (!take) {
            fail(p, "rva-unmapped");
            return 0;
        }
        if (take > left)
            take = left;
        at += take;
        left -= take;
    }
    return p->image + rva;
}
const char *w32_pe_string(struct w32_pe *p, ULONG rva) {
    for (ULONG i = 0; i < 4096; i++) {
        const char *s = w32_pe_span(p, rva + i, 1);
        if (!s)
            return 0;
        if (!*s)
            return (const char *)p->image + rva;
    }
    fail(p, "string-unterminated");
    return 0;
}
static int thunks(struct w32_pe *p, ULONG rva, int va, ULONG *count) {
    CHECK(!(rva & 3), "import-thunk-alignment");
    for (ULONG i = 0; i < 65536; i++) {
        uint8_t *t = w32_pe_span(p, rva + i * 4, 4);
        if (!t)
            return 0;
        ULONG v = w32_u32(t);
        if (!v) {
            *count = i;
            return 1;
        }
        if (v & 0x80000000)
            CHECK(!(v & 0x7fff0000), "import-ordinal-reserved-bits");
        else {
            if (va) {
                CHECK(v >= p->base && v - p->base < p->size, "delay-thunk-va-bounds");
                v -= p->base;
            }
            if (!w32_pe_span(p, v, 2) || !w32_pe_string(p, v + 2))
                return 0;
        }
    }
    return fail(p, "import-thunks-unterminated");
}
static int imports(struct w32_pe *p, int delay) {
    struct w32_directory d = p->dirs[delay ? 13 : 1];
    ULONG step = delay ? 32 : 20;
    if (!d.size)
        return 1;
    for (ULONG off = 0; off <= d.size && d.size - off >= step && off / step < 65536; off += step) {
        uint8_t *t = w32_pe_span(p, d.rva + off, step);
        if (!t)
            return 0;
        ULONG any = 0;
        for (ULONG i = 0; i < step; i++)
            any |= t[i];
        if (!any)
            return 1;
        ULONG name, iat, ilt, count = 0, va = 0;
        if (delay) {
            ULONG attr = w32_u32(t);
            CHECK(attr <= 1, "delay-attributes");
            va = !attr;
            name = w32_u32(t + 4);
            iat = w32_u32(t + 12);
            ilt = w32_u32(t + 16);
            if (va) {
                name = name ? name - p->base : 0;
                iat = iat ? iat - p->base : 0;
                ilt = ilt ? ilt - p->base : 0;
            }
            CHECK(iat, "import-iat-missing");
            if (ilt) {
                if (!thunks(p, ilt, va, &count))
                    return 0;
            } else {
                count = 1;
                w32_diag("load", "image", "delay-iat-without-names");
            }
        } else {
            ilt = w32_u32(t);
            name = w32_u32(t + 12);
            iat = w32_u32(t + 16);
            CHECK(iat, "import-iat-missing");
            if (ilt) {
                uint8_t *f = w32_pe_span(p, ilt, 4);
                if (!f)
                    return 0;
                if (!w32_u32(f))
                    ilt = 0;
            }
            uint8_t *f = w32_pe_span(p, iat, 4);
            if (!f)
                return 0;
            if (!ilt && w32_u32(t + 4) && w32_u32(f) >= p->size) {
                /* Structural validation accepts the f5-02 warning; binding
                 * refuses because there are no names left to re-resolve. */
                count = 1;
                w32_diag("load", "image", "bound-iat-without-names");
            } else if (!thunks(p, ilt ? ilt : iat, 0, &count))
                return 0;
        }
        if (!w32_pe_span(p, iat, (count + 1) * 4) || !w32_pe_string(p, name))
            return 0;
    }
    return fail(p, "import-descriptors-unterminated");
}
static const char *bound_name(struct w32_pe *p, struct w32_directory d, ULONG off) {
    if (off >= d.size) {
        fail(p, "bound-name-bounds");
        return 0;
    }
    for (ULONG i = 0; i < 4096 && i < d.size - off; i++) {
        char *c = w32_pe_span(p, d.rva + off + i, 1);
        if (!c)
            return 0;
        if (!*c)
            return (const char *)p->image + d.rva + off;
    }
    fail(p, "bound-name-unterminated");
    return 0;
}
static int bound(struct w32_pe *p) {
    struct w32_directory d = p->dirs[11];
    if (!d.size)
        return 1;
    for (ULONG off = 0, count = 0; off <= d.size && d.size - off >= 8 && count < 65536; count++) {
        uint8_t *t = w32_pe_span(p, d.rva + off, 8);
        if (!t)
            return 0;
        ULONG ts = w32_u32(t), name = w32_u16(t + 4), refs = w32_u16(t + 6);
        if (!(ts | name | refs))
            return 1;
        CHECK(refs <= (d.size - off - 8) / 8, "bound-forwarder-bounds");
        for (ULONG j = 0; j < refs; j++) {
            uint8_t *f = w32_pe_span(p, d.rva + off + 8 + j * 8, 8);
            if (!f || !bound_name(p, d, w32_u16(f + 4)))
                return 0;
        }
        if (!bound_name(p, d, name))
            return 0;
        off += 8 + refs * 8;
    }
    return fail(p, "bound-descriptors-unterminated");
}
static int exports(struct w32_pe *p) {
    struct w32_directory d = p->dirs[0];
    if (!d.size)
        return 1;
    CHECK(d.size >= 40, "export-directory-size");
    uint8_t *t = w32_pe_span(p, d.rva, 40);
    if (!t)
        return 0;
    ULONG nf = w32_u32(t + 20), nn = w32_u32(t + 24), fr = w32_u32(t + 28), nr = w32_u32(t + 32),
          or = w32_u32(t + 36);
    CHECK(nn <= nf && nf <= 65536, "export-count");
    uint8_t *names = w32_pe_span(p, nr, nn * 4);
    if (!names)
        return 0;
    uint8_t *ords = w32_pe_span(p, or, nn * 2);
    if (!ords)
        return 0;
    uint8_t *funcs = w32_pe_span(p, fr, nf * 4);
    if (!funcs)
        return 0;
    for (ULONG i = 0; i < nn; i++) {
        CHECK(w32_u16(ords + i * 2) < nf, "export-ordinal-index");
        if (!w32_pe_string(p, w32_u32(names + i * 4)))
            return 0;
    }
    for (ULONG i = 0; i < nf; i++) {
        ULONG a = w32_u32(funcs + i * 4);
        if (!a)
            continue;
        CHECK(a < p->size, "export-address-bounds");
        if (a >= d.rva && a - d.rva < d.size) {
            const char *f = w32_pe_string(p, a);
            if (!f)
                return 0;
            int dot = 0;
            for (ULONG j = 0; f[j]; j++)
                if (f[j] == '.')
                    dot = 1;
            CHECK(dot, "export-forwarder-format");
        } else if (!w32_pe_span(p, a, 1))
            return 0;
    }
    return 1;
}
static int relocations(struct w32_pe *p, ULONG delta, int apply) {
    struct w32_directory d = p->dirs[5];
    for (ULONG off = 0; off < d.size;) {
        CHECK(d.size - off >= 8, "relocation-block-size");
        uint8_t *head = w32_pe_span(p, d.rva + off, 8);
        if (!head)
            return 0;
        ULONG page = w32_u32(head), n = w32_u32(head + 4);
        CHECK(n >= 8 && !(n & 1) && n <= d.size - off, "relocation-block-size");
        uint8_t *entries = w32_pe_span(p, d.rva + off + 8, n - 8);
        if (!entries)
            return 0;
        for (ULONG i = 0; i < (n - 8) / 2; i++) {
            ULONG e = w32_u16(entries + i * 2), kind = e >> 12, at = e & 4095;
            CHECK(kind <= 4, "relocation-type");
            if (!kind)
                continue;
            CHECK(page <= p->size && at <= p->size - page &&
                      (kind == 3 ? 4u : 2u) <= p->size - page - at,
                  "relocation-target-bounds");
            uint8_t *target = w32_pe_span(p, page + at, kind == 3 ? 4 : 2);
            if (!target)
                return 0;
            int16_t add = 0;
            if (kind == 4) {
                CHECK(i + 1 < (n - 8) / 2, "relocation-highadj-pair");
                add = (int16_t)w32_u16(entries + (++i) * 2);
            }
            if (apply && !w32_pe_write(p,page+at,kind==3?4:2)) return 0;
            if (apply)
                switch (kind) {
                case 1:
                    w32_put16(target, w32_u16(target) + (delta >> 16));
                    break;
                case 2:
                    w32_put16(target, w32_u16(target) + (delta & 65535));
                    break;
                case 3:
                    w32_put32(target, w32_u32(target) + delta);
                    break;
                case 4:
                    w32_put16(target,
                              (((ULONG)w32_u16(target) << 16) + (LONG)add + delta + 0x8000) >> 16);
                    break;
                }
        }
        off += n;
    }
    return 1;
}
int w32_pe_relocate(struct w32_pe *p, ULONG base) {
    if (base != p->base) {
        CHECK(!(p->flags & 1) && p->dirs[5].size, "relocation-base-collision");
        if (!relocations(p, base - p->base, 1))
            return 0;
    }
    p->base = base;
    return 1;
}
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
    return resources(p, 0) && imports(p, 0) && imports(p, 1) && bound(p) && exports(p) &&
           relocations(p, 0, 0) && tls(p);
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

/* The kernel backend supplies shared COW sections. Permit only the pages
 * actually patched; final section protection is restored by pe_protect. */
int w32_pe_write(struct w32_pe *p, ULONG rva, ULONG bytes) {
    const struct w32_services *s=w32_get_services();
    if(!w32_pe_span(p,rva,bytes))return 0;
    if(!s || !s->image)return 1;
    ULONG start=rva&~4095u, end=(rva+bytes+4095)&~4095u;
    return s->protect(s->opaque,p->image+start,end-start,7) ? 1 : fail(p,"image-protect-failed");
}
