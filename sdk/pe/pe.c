/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. Extracted from sdk/win32/ldr/pe.c.
 * LGPL section 3 permits use in the GPL-2.0-only kernel. */
#include "pe.h"
static void pe_fill(void *v, int value, size_t n) {
    uint8_t *p=v; while(n--) *p++=(uint8_t)value;
}
static void pe_copy(void *v, const void *source, size_t n) {
    uint8_t *p=v; const uint8_t *s=source; while(n--) *p++=*s++;
}
static void pe_note(struct pe_image *p, const char *why) {
    if(p->ops && p->ops->diagnostic) p->ops->diagnostic(p->ops->context,why);
}
int pe_fail(struct pe_image *p, const char *why) {
    if (!p->error || !p->error->reason) {
        if (p->error) {
            p->error->code = 193;
            p->error->reason = why;
        }
        if (p->ops && p->ops->diagnostic) p->ops->diagnostic(p->ops->context, why);
    }
    return 0;
}
static int within(uint32_t start, uint32_t n, uint32_t limit) {
    return start <= limit && n <= limit - start;
}
static int power2(uint32_t n) {
    return n && !(n & (n - 1));
}
#define CHECK(c, r)                                                                                \
    do {                                                                                           \
        if (!(c))                                                                                  \
            return pe_fail(p, r);                                                                     \
    } while (0)
int pe_parse(struct pe_image *p, const void *bytes, uint32_t size, struct pe_error *e, const struct pe_ops *ops) {
    pe_fill(p, 0, sizeof(*p));
    p->file = bytes;
    p->file_size = size;
    p->error = e;
    p->ops = ops;
    if (e) {
        e->code = 0;
        e->reason = 0;
    }
    const uint8_t *f = bytes;
    CHECK(bytes && size >= 64, "dos-header-truncated");
    CHECK(f[0] == 'M' && f[1] == 'Z', "dos-magic");
    uint32_t off = pe_u32(f + 60);
    /* fx-classaudit: as the kernel validator and Wine's get_image_params,
     * an unaligned e_lfanew is read bytewise; bounds and signature decide. */
    if (off & 3) pe_note(p, "pe-offset-unaligned-accepted");
    CHECK(off >= 64 && within(off, 24, size), "pe-header-bounds");
    CHECK(pe_u32(f + off) == 0x4550, "pe-signature");
    CHECK(pe_u16(f + off + 4) == 0x14c, "machine-not-i386");
    p->section_count = pe_u16(f + off + 6);
    CHECK(p->section_count && p->section_count <= 96, "section-count");
    p->flags = pe_u16(f + off + 22);
    CHECK(p->flags & 2, "not-executable-image");
    uint32_t o = off + 24, olen = pe_u16(f + off + 20);
    CHECK(olen >= 96 && within(o, olen, size), "optional-header-bounds");
    CHECK(pe_u16(f + o) == 0x10b, "optional-magic-not-pe32");
    uint32_t nd = pe_u32(f + o + 92);
    CHECK(nd <= 16 && olen >= 96 + nd * 8, "directory-count");
    CHECK(nd<=14||!pe_u32(f+o+96+14*8+4),"unsupported-runtime");
    p->preferred = p->base = pe_u32(f + o + 28);
    p->entry = pe_u32(f + o + 16);
    uint32_t sa = pe_u32(f + o + 32), fa = pe_u32(f + o + 36);
    p->section_alignment = sa;
    CHECK(power2(fa) && ((fa >= 512 && fa <= 65536) || (sa < 4096 && fa == sa)), "file-alignment");
    CHECK(power2(sa) && sa >= fa, "section-alignment");
    p->size = pe_u32(f + o + 56);
    p->headers = pe_u32(f + o + 60);
    CHECK(p->size && p->size <= 0x20000000 && !(p->size & (sa - 1)), "image-size");
    CHECK((uint64_t)p->preferred + p->size <= 0x100000000ull, "image-address-wrap");
    uint32_t table = o + olen;
    CHECK(within(table, p->section_count * 40, size), "section-table-bounds");
    CHECK(p->headers >= table + p->section_count * 40 && p->headers <= size &&
              p->headers <= p->size,
          "headers-size");
    p->subsystem = pe_u16(f + o + 68);
    /* Subsystem admission belongs to the consumer. */
    /* MajorSubsystemVersion/MinorSubsystemVersion: optional-header offsets
     * 48 and 50 (Microsoft PE format, "Optional Header Windows-Specific
     * Fields"). The loader logs a newer version with the module name. */
    p->subsystem_major = pe_u16(f + o + 48);
    p->subsystem_minor = pe_u16(f + o + 50);
    CHECK(p->entry < p->size, "entrypoint-bounds");
    for (uint32_t i = 0; i < p->section_count; i++) {
        const uint8_t *t = f + table + i * 40;
        struct pe_section *s = &p->sections[i];
        uint32_t vs = pe_u32(t + 8);
        s->rva = pe_u32(t + 12);
        s->raw = pe_u32(t + 16);
        s->offset = pe_u32(t + 20);
        s->flags = pe_u32(t + 36);
        s->span = vs > s->raw ? vs : s->raw;
        CHECK(within(s->rva, s->span, p->size), "section-image-bounds");
        CHECK(!(s->rva & (sa - 1)), "section-rva-alignment");
        CHECK(!s->raw || !(s->offset & (fa - 1)), "section-raw-alignment");
        CHECK((uint64_t)s->offset + s->raw <= 0x100000000ull, "section-raw-wrap");
        CHECK(!s->span || s->rva >= p->headers, "section-header-overlap");
        for (uint32_t j = 0; j < i; j++) {
            struct pe_section *a = &p->sections[j];
            CHECK(!s->span || !a->span || s->rva >= a->rva + a->span || s->rva + s->span <= a->rva,
                  "section-virtual-overlap");
        }
        if (s->raw && (uint64_t)s->offset + s->raw > size)
            pe_note(p, "raw-data-zero-filled");
    }
    for (uint32_t i = 0; i < nd; i++) {
        struct pe_directory *d = &p->dirs[i];
        d->rva = pe_u32(f + o + 96 + i * 8);
        d->size = pe_u32(f + o + 100 + i * 8);
        if (d->size) {
            if (i == 4)
                CHECK(!(d->rva & 7) && within(d->rva, d->size, size), "certificate-file-bounds");
            else
                CHECK(d->rva && within(d->rva, d->size, p->size), "directory-image-bounds");
        }
    }
    return !ops || !ops->admit || ops->admit(ops->context, p);
}
int pe_copy_image(struct pe_image *p, void *image) {
    p->image = image;
    if (!image)
        return pe_fail(p, "image-map-failed");
    pe_fill(image, 0, p->size);
    pe_copy(image, p->file, p->headers);
    for (uint32_t i = 0; i < p->section_count; i++) {
        struct pe_section *s = &p->sections[i];
        uint32_t n = s->raw;
        if (s->offset >= p->file_size)
            n = 0;
        else if (n > p->file_size - s->offset)
            n = p->file_size - s->offset;
        if (n)
            pe_copy(p->image + s->rva, p->file + s->offset, n);
    }
    return 1;
}
void *pe_span(struct pe_image *p, uint32_t rva, uint32_t n) {
    if (!within(rva, n, p->size)) {
        pe_fail(p, "rva-bounds");
        return 0;
    }
    uint32_t at = rva, left = n;
    while (left) {
        uint32_t take = 0;
        if (at < p->headers)
            take = p->headers - at;
        else
            for (uint32_t i = 0; i < p->section_count; i++) {
                struct pe_section *s = &p->sections[i];
                if (at >= s->rva && at - s->rva < s->span) {
                    take = s->span - (at - s->rva);
                    break;
                }
            }
        if (!take) {
            pe_fail(p, "rva-unmapped");
            return 0;
        }
        if (take > left)
            take = left;
        at += take;
        left -= take;
    }
    return p->image + rva;
}
const char *pe_string(struct pe_image *p, uint32_t rva) {
    for (uint32_t i = 0; i < 4096; i++) {
        const char *s = pe_span(p, rva + i, 1);
        if (!s)
            return 0;
        if (!*s)
            return (const char *)p->image + rva;
    }
    pe_fail(p, "string-unterminated");
    return 0;
}
static int thunks(struct pe_image *p, uint32_t rva, int va, uint32_t *count) {
    CHECK(!(rva & 3), "import-thunk-alignment");
    for (uint32_t i = 0; i < 65536; i++) {
        uint8_t *t = pe_span(p, rva + i * 4, 4);
        if (!t)
            return 0;
        uint32_t v = pe_u32(t);
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
            if (!pe_span(p, v, 2) || !pe_string(p, v + 2))
                return 0;
        }
    }
    return pe_fail(p, "import-thunks-unterminated");
}
static int imports(struct pe_image *p, int delay) {
    struct pe_directory d = p->dirs[delay ? 13 : 1];
    uint32_t step = delay ? 32 : 20;
    if (!d.size)
        return 1;
    for (uint32_t off = 0; off <= d.size && d.size - off >= step && off / step < 65536; off += step) {
        uint8_t *t = pe_span(p, d.rva + off, step);
        if (!t)
            return 0;
        uint32_t any = 0;
        for (uint32_t i = 0; i < step; i++)
            any |= t[i];
        if (!any)
            return 1;
        uint32_t name, iat, ilt, count = 0, va = 0;
        if (delay) {
            uint32_t attr = pe_u32(t);
            CHECK(attr <= 1, "delay-attributes");
            va = !attr;
            name = pe_u32(t + 4);
            iat = pe_u32(t + 12);
            ilt = pe_u32(t + 16);
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
                pe_note(p, "delay-iat-without-names");
            }
        } else {
            ilt = pe_u32(t);
            name = pe_u32(t + 12);
            iat = pe_u32(t + 16);
            CHECK(iat, "import-iat-missing");
            if (ilt) {
                uint8_t *f = pe_span(p, ilt, 4);
                if (!f)
                    return 0;
                if (!pe_u32(f))
                    ilt = 0;
            }
            uint8_t *f = pe_span(p, iat, 4);
            if (!f)
                return 0;
            if (!ilt && pe_u32(t + 4) && pe_u32(f) >= p->size) {
                /* Structural validation accepts the f5-02 warning; binding
                 * refuses because there are no names left to re-resolve. */
                count = 1;
                pe_note(p, "bound-iat-without-names");
            } else if (!thunks(p, ilt ? ilt : iat, 0, &count))
                return 0;
        }
        if (!pe_span(p, iat, (count + 1) * 4) || !pe_string(p, name))
            return 0;
    }
    return pe_fail(p, "import-descriptors-unterminated");
}
static const char *bound_name(struct pe_image *p, struct pe_directory d, uint32_t off) {
    if (off >= d.size) {
        pe_fail(p, "bound-name-bounds");
        return 0;
    }
    for (uint32_t i = 0; i < 4096 && i < d.size - off; i++) {
        char *c = pe_span(p, d.rva + off + i, 1);
        if (!c)
            return 0;
        if (!*c)
            return (const char *)p->image + d.rva + off;
    }
    pe_fail(p, "bound-name-unterminated");
    return 0;
}
static int bound(struct pe_image *p) {
    struct pe_directory d = p->dirs[11];
    if (!d.size)
        return 1;
    for (uint32_t off = 0, count = 0; off <= d.size && d.size - off >= 8 && count < 65536; count++) {
        uint8_t *t = pe_span(p, d.rva + off, 8);
        if (!t)
            return 0;
        uint32_t ts = pe_u32(t), name = pe_u16(t + 4), refs = pe_u16(t + 6);
        if (!(ts | name | refs))
            return 1;
        CHECK(refs <= (d.size - off - 8) / 8, "bound-forwarder-bounds");
        for (uint32_t j = 0; j < refs; j++) {
            uint8_t *f = pe_span(p, d.rva + off + 8 + j * 8, 8);
            if (!f || !bound_name(p, d, pe_u16(f + 4)))
                return 0;
        }
        if (!bound_name(p, d, name))
            return 0;
        off += 8 + refs * 8;
    }
    return pe_fail(p, "bound-descriptors-unterminated");
}
static int exports(struct pe_image *p) {
    struct pe_directory d = p->dirs[0];
    if (!d.size)
        return 1;
    CHECK(d.size >= 40, "export-directory-size");
    uint8_t *t = pe_span(p, d.rva, 40);
    if (!t)
        return 0;
    uint32_t nf = pe_u32(t + 20), nn = pe_u32(t + 24), fr = pe_u32(t + 28), nr = pe_u32(t + 32),
          or = pe_u32(t + 36);
    CHECK(nn <= nf && nf <= 65536, "export-count");
    uint8_t *names = pe_span(p, nr, nn * 4);
    if (!names)
        return 0;
    uint8_t *ords = pe_span(p, or, nn * 2);
    if (!ords)
        return 0;
    uint8_t *funcs = pe_span(p, fr, nf * 4);
    if (!funcs)
        return 0;
    for (uint32_t i = 0; i < nn; i++) {
        CHECK(pe_u16(ords + i * 2) < nf, "export-ordinal-index");
        if (!pe_string(p, pe_u32(names + i * 4)))
            return 0;
    }
    for (uint32_t i = 0; i < nf; i++) {
        uint32_t a = pe_u32(funcs + i * 4);
        if (!a)
            continue;
        CHECK(a < p->size, "export-address-bounds");
        if (a >= d.rva && a - d.rva < d.size) {
            const char *f = pe_string(p, a);
            if (!f)
                return 0;
            int dot = 0;
            for (uint32_t j = 0; f[j]; j++)
                if (f[j] == '.')
                    dot = 1;
            CHECK(dot, "export-forwarder-format");
        } else if (!pe_span(p, a, 1))
            return 0;
    }
    return 1;
}
static int relocations(struct pe_image *p, uint32_t delta, int apply) {
    struct pe_directory d = p->dirs[5];
    for (uint32_t off = 0; off < d.size;) {
        CHECK(d.size - off >= 8, "relocation-block-size");
        uint8_t *head = pe_span(p, d.rva + off, 8);
        if (!head)
            return 0;
        uint32_t page = pe_u32(head), n = pe_u32(head + 4);
        CHECK(n >= 8 && !(n & 1) && n <= d.size - off, "relocation-block-size");
        uint8_t *entries = pe_span(p, d.rva + off + 8, n - 8);
        if (!entries)
            return 0;
        for (uint32_t i = 0; i < (n - 8) / 2; i++) {
            uint32_t e = pe_u16(entries + i * 2), kind = e >> 12, at = e & 4095;
            CHECK(kind <= 4, "relocation-type");
            if (!kind)
                continue;
            CHECK(page <= p->size && at <= p->size - page &&
                      (kind == 3 ? 4u : 2u) <= p->size - page - at,
                  "relocation-target-bounds");
            uint8_t *target = pe_span(p, page + at, kind == 3 ? 4 : 2);
            if (!target)
                return 0;
            int16_t add = 0;
            if (kind == 4) {
                CHECK(i + 1 < (n - 8) / 2, "relocation-highadj-pair");
                add = (int16_t)pe_u16(entries + (++i) * 2);
            }
            if (apply && !pe_write(p,page+at,kind==3?4:2)) return 0;
            if (apply)
                switch (kind) {
                case 1:
                    pe_put16(target, pe_u16(target) + (delta >> 16));
                    break;
                case 2:
                    pe_put16(target, pe_u16(target) + (delta & 65535));
                    break;
                case 3:
                    pe_put32(target, pe_u32(target) + delta);
                    break;
                case 4:
                    pe_put16(target,
                              (((uint32_t)pe_u16(target) << 16) + (int32_t)add + delta + 0x8000) >> 16);
                    break;
                }
        }
        off += n;
    }
    return 1;
}
int pe_relocate(struct pe_image *p, uint32_t base) {
    CHECK((uint64_t)base + p->size <= 0x100000000ull, "image-address-wrap");
    if (!relocations(p, 0, 0)) return 0;
    if (base != p->base) {
        CHECK(!(p->flags & 1) && p->dirs[5].size, "relocation-base-collision");
        if (!relocations(p, base - p->base, 1))
            return 0;
    }
    p->base = base;
    return 1;
}

int pe_write(struct pe_image *p, uint32_t rva, uint32_t bytes) {
    if(!pe_span(p,rva,bytes)) return 0;
    return !p->ops || !p->ops->writable ||
        p->ops->writable(p->ops->context,p,rva,bytes);
}
int pe_allocate_image(struct pe_image *p) {
    if(!p->ops || !p->ops->allocate || !p->ops->release)
        return pe_fail(p,"image-allocator-missing");
    uint32_t base=p->preferred;
    void *image=p->ops->allocate(p->ops->context,p->preferred,p->size,&base);
    if(!image) return pe_fail(p,"image-map-failed");
    if(!pe_copy_image(p,image) || !pe_validate_mechanism(p) || !pe_relocate(p,base) || !pe_validate_mechanism(p)) {
        p->ops->release(p->ops->context,image,p->size); p->image=0; return 0;
    }
    return 1;
}
int pe_validate_mechanism(struct pe_image *p) {
    return imports(p,0) && imports(p,1) && bound(p) && exports(p) && relocations(p,0,0);
}
int pe_walk_imports(struct pe_image *p, void *ctx, pe_import_fn descriptor, pe_import_fn symbol) {
    /* Callbacks can bind overlapping IATs; never trust a prevalidation after
     * a visitor has written the private image. Bound every fresh read. */
    if(!imports(p,0)) return 0;
    struct pe_directory d=p->dirs[1];
    if(!d.size) return 1;
    for(uint32_t off=0; off<=d.size && d.size-off>=20 && off/20<65536; off+=20) {
        uint8_t *t=pe_span(p,d.rva+off,20); if(!t) return 0;
        uint32_t any=0; for(unsigned j=0;j<20;j++) any|=t[j]; if(!any) return 1;
        uint32_t ilt=pe_u32(t), iat=pe_u32(t+16), timestamp=pe_u32(t+4), count=0;
        uint32_t module_rva=pe_u32(t+12);
        const char *module=pe_string(p,module_rva); if(!module) return 0;
        CHECK(iat,"import-iat-missing");
        uint8_t *first=pe_span(p,iat,4); if(!first) return 0;
        if(ilt) {
            uint8_t *lookup=pe_span(p,ilt,4); if(!lookup) return 0;
            if(!pe_u32(lookup)) ilt=0;
        }
        if(!ilt && timestamp && pe_u32(first)>=p->size)
            return pe_fail(p,"bound-iat-without-names");
        uint32_t lookup=ilt?ilt:iat;
        if(!thunks(p,lookup,0,&count) || !pe_span(p,iat,(count+1)*4)) return 0;
        struct pe_import item={.module=module,.descriptor=off};
        if(descriptor && !descriptor(ctx,&item)) return 0;
        for(uint32_t i=0;i<count;i++) {
            uint8_t *word=pe_span(p,lookup+i*4,4); if(!word) return 0;
            uint32_t v=pe_u32(word);
            CHECK(v,"import-thunk-changed");
            item.name=0;
            if(v&0x80000000) CHECK(!(v&0x7fff0000),"import-ordinal-reserved-bits");
            else {
                if(!pe_span(p,v,2) || !(item.name=pe_string(p,v+2))) return 0;
            }
            item.ordinal=(uint16_t)v; item.hint=item.name?pe_u16(p->image+v):0xffff;
            item.iat=iat+i*4;
            item.module=pe_string(p,module_rva); if(!item.module) return 0;
            if(!pe_span(p,item.iat,4) || (symbol && !symbol(ctx,&item))) return 0;
        }
    }
    return pe_fail(p,"import-descriptors-unterminated");
}
