/* SPDX-License-Identifier: MIT */
#include "ne.h"

struct table { uint32_t start, end; };
struct parser { struct ne_image *n; struct ne_error *e; };

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p + 2) << 16); }
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }
static void zero(void *p, uint32_t count) { uint8_t *q = p; while (count--) *q++ = 0; }
static void copy(uint8_t *d, const uint8_t *s, uint32_t count) { while (count--) *d++ = *s++; }
static int fits(uint32_t off, uint32_t count, uint32_t end) { return off <= end && count <= end - off; }
static int fail(struct ne_error *e, uint32_t rule, uint32_t off, uint32_t seg, uint32_t rec)
{
    if (e) { e->rule = rule; e->file_offset = off; e->segment = seg; e->record = rec; }
    return -1;
}
static int pf(struct parser *p, uint32_t rule, uint32_t off) { return fail(p->e, rule, off, 0, 0); }
const char *ne_rule_name(uint32_t rule)
{
    switch (rule) {
#define NE_CASE(id, name) case NE_##id: return name;
        NE_RULES(NE_CASE)
#undef NE_CASE
    default: return "unknown-rule";
    }
}
static int shifted(struct parser *p, uint32_t v, uint32_t shift, uint32_t *out, uint32_t at)
{
    if (shift > 31) return pf(p, NE_SHIFT, at);
    if (v > (UINT32_MAX >> shift)) return pf(p, NE_ARITHMETIC, at);
    *out = v << shift;
    return 0;
}
static int same(const struct ne_image *n, struct ne_string a, struct ne_string b)
{
    uint32_t i;
    if (a.length != b.length) return 0;
    for (i = 0; i < a.length; ++i) if (n->bytes[a.offset+i] != n->bytes[b.offset+i]) return 0;
    return 1;
}
static int same_id(const struct ne_image *n, struct ne_id a, struct ne_id b)
{
    return a.integer || b.integer ? a.integer == b.integer : same(n, a.text, b.text);
}
static int string_at(struct parser *p, uint32_t off, uint32_t end, struct ne_string *s, uint32_t rule)
{
    uint32_t len;
    if (!fits(off, 1, end)) return pf(p, rule, off);
    len = p->n->bytes[off];
    if (!len || !fits(off + 1, len, end)) return pf(p, rule, off);
    s->offset = off + 1; s->length = len;
    return 0;
}
static int overlap(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
    /* All spans are already checked against the file size; no end overflow. */
    return alen && blen && a < b + blen && b < a + alen;
}
static uint32_t table_end(const struct ne_image *n, const uint32_t *starts, uint32_t start)
{
    uint32_t i, end = n->size;
    /* Relative offsets cannot address beyond a 64 KiB NE metadata region. */
    if (n->size - n->ne_offset > 65536u) end = n->ne_offset + 65536u;
    for (i = 0; i < 7; ++i) if (starts[i] > start && starts[i] < end) end = starts[i];
    for (i = 0; i < n->segment_count; ++i) {
        uint32_t pos = n->segments[i].file_offset;
        if (n->segments[i].sector && pos > start && pos < end) end = pos;
    }
    return end;
}
static int segments(struct parser *p, uint32_t off)
{
    struct ne_image *n = p->n;
    uint32_t i;
    if (!fits(off, n->segment_count * 8u, n->size)) return pf(p, NE_TABLE_BOUNDS, off);
    for (i = 0; i < n->segment_count; ++i) {
        const uint8_t *b = n->bytes + off + i*8;
        struct ne_segment *s = &n->segments[i];
        uint32_t pos, end, expanded = 0;
        s->sector = u16(b); s->flags = u16(b+4);
        s->allocation = u16(b+6) ? u16(b+6) : 65536u;
        /* DPL (0C00), debug/conforming (0200) and discard priority bits are
         * recorded only. Loader-owned runtime bits and 32-bit (D/B=1) code are
         * refused by name; 0800 is a DPL bit here, never a self-load marker. */
        if (s->flags & NE_SEG_LOADER_BITS) return fail(p->e, NE_SEG_FLAGS, off+i*8+4, i+1, 0);
        if (s->flags & NE_SEG_FLAG_32BIT) return fail(p->e, NE_SEG_32BIT, off+i*8+4, i+1, 0);
        if (!s->sector) {
            if (s->flags & (NE_SEG_ITERATED | NE_SEG_RELOC))
                return fail(p->e, NE_SEG_ZERO, off+i*8, i+1, 0);
            continue;
        }
        if (shifted(p, s->sector, n->alignment, &s->file_offset, off+i*8)) return -1;
        s->stored_size = u16(b+2) ? u16(b+2) : 65536u;
        if (!fits(s->file_offset, s->stored_size, n->size))
            return fail(p->e, NE_SEG_BOUNDS, s->file_offset, i+1, 0);
        if (!(s->flags & NE_SEG_ITERATED)) expanded = s->stored_size;
        else {
            pos = s->file_offset; end = pos + s->stored_size;
            while (pos < end) {
                uint32_t count, len, amount;
                if (!fits(pos, 4, end)) return fail(p->e, NE_ITERATED, pos, i+1, 0);
                count = u16(n->bytes+pos); len = u16(n->bytes+pos+2); pos += 4;
                if (!count || !len || !fits(pos, len, end)) return fail(p->e, NE_ITERATED, pos-4, i+1, 0);
                amount = count * len; /* 16 * 16 => fits uint32_t. */
                if (amount > 65536u - expanded) return fail(p->e, NE_EXPANSION, pos-4, i+1, 0);
                expanded += amount; pos += len;
            }
        }
        s->initialized_size = expanded;
        if (expanded > s->allocation) s->allocation = expanded;
        s->reloc_first = n->reloc_count;
        if (s->flags & NE_SEG_RELOC) {
            uint32_t j, count;
            pos = s->file_offset + s->stored_size;
            if (!fits(pos, 2, n->size)) return fail(p->e, NE_RELOC_BOUNDS, pos, i+1, 0);
            count = u16(n->bytes+pos); pos += 2;
            if (count > NE_MAX_RELOCS - n->reloc_count) return pf(p, NE_LIMIT, pos-2);
            if (!fits(pos, count*8, n->size)) return fail(p->e, NE_RELOC_BOUNDS, pos, i+1, 0);
            s->reloc_count = count;
            for (j = 0; j < count; ++j) {
                struct ne_reloc *r = &n->relocs[n->reloc_count++];
                r->segment = (uint16_t)(i+1); r->source_type = n->bytes[pos]; r->flags = n->bytes[pos+1];
                r->source = u16(n->bytes+pos+2); r->target1 = u16(n->bytes+pos+4); r->target2 = u16(n->bytes+pos+6);
                pos += 8;
            }
        }
    }
    return 0;
}
static int entries(struct parser *p, uint32_t off, uint32_t len)
{
    struct ne_image *n = p->n;
    uint32_t pos = off, end = off+len;
    if (!len) return 0;
    while (pos < end) {
        uint32_t count = n->bytes[pos++], type, i;
        if (!count) return 0; /* Count alone terminates; remaining bytes are padding. */
        if (!fits(pos, 1, end)) return pf(p, NE_ENTRY_BOUNDS, pos);
        type = n->bytes[pos++];
        if (count > NE_MAX_ENTRIES - n->entry_count) return pf(p, NE_LIMIT, pos-2);
        for (i = 0; i < count; ++i) {
            struct ne_entry *e = &n->entries[n->entry_count++];
            if (!type) continue;
            if (!fits(pos, type == 255 ? 6 : 3, end)) return pf(p, NE_ENTRY_BOUNDS, pos);
            e->flags = n->bytes[pos];
            if (type == 255) {
                if (u16(n->bytes+pos+1) != 0x3fcdu) return pf(p, NE_ENTRY_INT3F, pos+1);
                e->kind = NE_MOVABLE; e->segment = n->bytes[pos+3]; e->offset = u16(n->bytes+pos+4); pos += 6;
            } else {
                e->kind = type == 254 ? NE_CONSTANT : NE_FIXED;
                e->segment = (uint16_t)type; e->offset = u16(n->bytes+pos+1); pos += 3;
            }
            if (e->kind == NE_CONSTANT) continue;
            if (!e->segment || e->segment > n->segment_count) return pf(p, NE_ENTRY_SEGMENT, pos);
            if (e->offset >= n->segments[e->segment-1].allocation) return pf(p, NE_ENTRY_OFFSET, pos);
        }
    }
    return pf(p, NE_ENTRY_TERMINATOR, end);
}
static int names(struct parser *p, uint32_t off, uint32_t end, int nonresident, uint32_t *used)
{
    struct ne_image *n = p->n;
    uint32_t pos = off, first = n->name_count;
    while (pos < end) {
        struct ne_name *nm;
        uint32_t i, len = n->bytes[pos];
        if (!len) { *used = pos+1; return 0; }
        if (!fits(pos, len+3, end)) return pf(p, NE_NAME_BOUNDS, pos);
        if (n->name_count == NE_MAX_NAMES) return pf(p, NE_LIMIT, pos);
        nm = &n->names[n->name_count];
        nm->text.offset = pos+1; nm->text.length = len;
        nm->ordinal = u16(n->bytes+pos+1+len); nm->nonresident = (uint16_t)nonresident;
        if (!nm->ordinal) {
            if (n->name_count != first) return pf(p, NE_NAME_ZERO, pos);
        } else {
            if (nm->ordinal > n->entry_count || n->entries[nm->ordinal-1].kind == NE_UNUSED)
                return pf(p, NE_NAME_ORDINAL, pos);
            for (i = 0; i < n->name_count; ++i)
                if (n->names[i].ordinal && same(n, nm->text, n->names[i].text)) return pf(p, NE_NAME_DUPLICATE, pos);
        }
        ++n->name_count; pos += len+3;
    }
    return pf(p, NE_NAME_TERMINATOR, end);
}
static int resource_id(struct parser *p, uint32_t base, uint32_t strings, uint32_t end, uint16_t id, struct ne_id *out)
{
    if (id & 0x8000u) {
        out->integer = id & 0x7fffu;
        if (!out->integer) return pf(p, NE_RESOURCE_NAME, base);
        return 0;
    }
    if (!fits(base, id, end) || base+id < strings) return pf(p, NE_RESOURCE_NAME, base);
    return string_at(p, base+id, end, &out->text, NE_RESOURCE_NAME);
}
static int resources(struct parser *p, uint32_t off, uint32_t end)
{
    struct ne_image *n = p->n;
    uint32_t pos = off, string_start, i;
    uint16_t types[NE_MAX_RESOURCES], ids[NE_MAX_RESOURCES];
    if (off == end) return 0; /* No resource table. */
    if (!fits(pos, 2, end)) return pf(p, NE_RESOURCE_BOUNDS, pos);
    n->resource_alignment = u16(n->bytes+pos); pos += 2;
    if (n->resource_alignment > 31) return pf(p, NE_SHIFT, off);
    for (;;) {
        uint32_t type, count, j;
        if (!fits(pos, 2, end)) return pf(p, NE_RESOURCE_BOUNDS, pos);
        type = u16(n->bytes+pos); pos += 2;
        if (!type) break;
        if (!fits(pos, 6, end)) return pf(p, NE_RESOURCE_BOUNDS, pos);
        count = u16(n->bytes+pos); pos += 6;
        if (!count) return pf(p, NE_RESOURCE_BOUNDS, pos-6);
        if (count > NE_MAX_RESOURCES - n->resource_count) return pf(p, NE_LIMIT, pos-6);
        if (!fits(pos, count*12, end)) return pf(p, NE_RESOURCE_BOUNDS, pos);
        for (j = 0; j < count; ++j) {
            struct ne_resource *r = &n->resources[n->resource_count];
            types[n->resource_count] = (uint16_t)type; ids[n->resource_count] = u16(n->bytes+pos+6);
            r->flags = u16(n->bytes+pos+4);
            if (shifted(p, u16(n->bytes+pos), n->resource_alignment, &r->file_offset, pos) ||
                shifted(p, u16(n->bytes+pos+2), n->resource_alignment, &r->size, pos+2)) return -1;
            if (!fits(r->file_offset, r->size, n->size)) return pf(p, NE_RESOURCE_DATA, pos);
            ++n->resource_count; pos += 12;
        }
    }
    string_start = pos;
    for (i = 0; i < n->resource_count; ++i) {
        uint32_t j;
        struct ne_resource *r = &n->resources[i];
        if (resource_id(p, off, string_start, end, types[i], &r->type) ||
            resource_id(p, off, string_start, end, ids[i], &r->name)) return -1;
        for (j = 0; j < i; ++j)
            if (same_id(n, r->type, n->resources[j].type) && same_id(n, r->name, n->resources[j].name))
                return pf(p, NE_RESOURCE_DUPLICATE, off);
    }
    return 0;
}
static uint32_t width(uint8_t type)
{
    switch (type & 0x7fu) { case 0: return 1; case 2: case 5: return 2; case 3: case 13: return 4; case 11: return 6; }
    return 0;
}
static int relocations(struct parser *p, uint32_t imp, uint32_t end)
{
    struct ne_image *n = p->n;
    uint32_t i;
    for (i = 0; i < n->reloc_count; ++i) {
        struct ne_reloc *r = &n->relocs[i];
        uint32_t w = width(r->source_type), kind = r->flags & 3;
        if (!w) return fail(p->e, NE_RELOC_TYPE, 0, r->segment, i);
        if (r->flags & ~7u) return fail(p->e, NE_RELOC_FLAGS, 0, r->segment, i);
        if (!fits(r->source, (r->flags & 4) || w > 1 ? w : 2, n->segments[r->segment-1].allocation))
            return fail(p->e, NE_PATCH_BOUNDS, 0, r->segment, i);
        if (kind == 0) {
            if ((r->target1 & 0xffu) == 0xffu) { /* Wine ignores the reserved byte here. */
                if (!r->target2 || r->target2 > n->entry_count || n->entries[r->target2-1].kind == NE_UNUSED)
                    return fail(p->e, NE_RELOC_TARGET, 0, r->segment, i);
            } else if (!r->target1 || r->target1 > n->segment_count || r->target1 > 254 ||
                       r->target2 >= n->segments[r->target1-1].allocation)
                return fail(p->e, NE_RELOC_TARGET, 0, r->segment, i);
        } else if (kind == 1 || kind == 2) {
            struct ne_string name;
            if (!r->target1 || r->target1 > n->module_count) return fail(p->e, NE_MODULE_INDEX, 0, r->segment, i);
            if (kind == 1 && !r->target2) return fail(p->e, NE_RELOC_TARGET, 0, r->segment, i);
            if (kind == 2 && string_at(p, imp+r->target2, end, &name, NE_IMPORT_NAME)) return -1;
        }
    }
    return 0;
}
int ne_parse(const uint8_t *bytes, uint32_t size, struct ne_image *n, struct ne_error *error)
{
    struct parser p = { n, error };
    struct table t[8];
    uint32_t h, i, j, starts[7], entlen, nrlen, used;
    const uint8_t *b;
    if (error) zero(error, sizeof(*error));
    if (!n || !bytes) return fail(error, NE_ARGUMENT, 0, 0, 0);
    zero(n, sizeof(*n)); n->bytes = bytes; n->size = size;
    if (size < 64) return pf(&p, NE_DOS_HEADER, 0);
    if (bytes[0] != 'M' || bytes[1] != 'Z') return pf(&p, NE_DOS_MAGIC, 0);
    h = u32(bytes+60);
    if (h < 64 || !fits(h, 4, size)) return pf(&p, NE_HEADER_BOUNDS, 60);
    b = bytes+h;
    if (b[0] == 'P' && b[1] == 'E' && !b[2] && !b[3]) return pf(&p, NE_IMAGE_PE, h);
    if (b[0] == 'L' && (b[1] == 'E' || b[1] == 'X')) return pf(&p, NE_IMAGE_LX, h);
    if (b[0] != 'N' || b[1] != 'E') return pf(&p, NE_SIGNATURE, h);
    if (!fits(h, 64, size)) return pf(&p, NE_HEADER_BOUNDS, 60);
    n->ne_offset = h;
    n->target = b[54]; n->other_flags = b[55]; n->expected_version = u16(b+62);
    n->linker_version = (uint16_t)(b[2]*256u+b[3]);
    n->flags = u16(b+12); n->autodata = u16(b+14); n->heap = u16(b+16); n->stack = u16(b+18);
    n->ip = u16(b+20); n->cs = u16(b+22); n->sp = u16(b+24); n->ss = u16(b+26);
    n->segment_count = u16(b+28); n->module_count = u16(b+30); nrlen = u16(b+32);
    n->alignment_stored = u16(b+50); entlen = u16(b+6);
    n->alignment = n->alignment_stored ? n->alignment_stored : NE_ALIGN_DEFAULT;
    if (n->target == NE_TGT_OS2) return pf(&p, NE_TARGET_OS2, h+54);
    if (n->target != NE_TGT_WINDOWS && n->target != NE_TGT_WIN386) return pf(&p, NE_TARGET, h+54);
    /* Only link errors and the impossible SINGLE+MULTIPLE data model are refused.
     * 0004..0080 (instance init, protected mode, CPU/EMS hints), 0300 (app
     * type) and 4000 (private DLL / call WEP) are recorded, as are other_flags. */
    if (n->flags & NE_MODULE_LINK_ERROR) return pf(&p, NE_LINK_ERRORS, h+12);
    if ((n->flags & NE_MODULE_DATA_MASK) == NE_MODULE_DATA_MASK) return pf(&p, NE_DATA_MODEL, h+12);
    if (n->alignment > 31) return pf(&p, NE_SHIFT, h+50);
    if (n->segment_count > NE_MAX_SEGMENTS || n->module_count > NE_MAX_MODULES) return pf(&p, NE_LIMIT, h+28);
    starts[0] = h+u16(b+34); starts[1] = h+u16(b+36); starts[2] = h+u16(b+38);
    starts[3] = h+u16(b+40); starts[4] = h+u16(b+42); starts[5] = h+u16(b+4); starts[6] = u32(b+44);
    /* Relative sums are safe: h+64 fits size, but h+65535 may overflow. */
    for (i = 0; i < 6; ++i)
        if (starts[i] < h || starts[i] - h < 64 || starts[i] > size) return pf(&p, NE_TABLE_BOUNDS, h);
    if (!(starts[1] <= starts[2] && starts[2] <= starts[3] && starts[3] <= starts[4])) return pf(&p, NE_TABLE_ORDER, h);
    if (!fits(starts[5], entlen, size) || (nrlen && (!starts[6] || !fits(starts[6], nrlen, size)))) return pf(&p, NE_TABLE_BOUNDS, h);
    if (segments(&p, starts[0])) return -1;
    for (i = 0; i < 6; ++i) { t[i].start = starts[i]; t[i].end = starts[i]; }
    t[0].end += n->segment_count*8;
    t[1].end = starts[2]; /* Resource strings and padding belong to this table. */
    t[3].end += n->module_count*2;
    t[4].end = table_end(n, starts, starts[4]);
    t[5].end += entlen;
    t[6].start = starts[6]; t[6].end = starts[6]+nrlen;
    t[7].start = 0; t[7].end = h+64; /* DOS stub plus NE header. */
    if (t[3].end > starts[4]) return pf(&p, NE_TABLE_BOUNDS, starts[3]);
    if (entries(&p, starts[5], entlen)) return -1;
    if (names(&p, starts[2], table_end(n, starts, starts[2]), 0, &used)) return -1;
    t[2].end = used;
    if (nrlen && names(&p, starts[6], starts[6]+nrlen, 1, &used)) return -1;
    if (resources(&p, starts[1], starts[2])) return -1;
    for (i = 0; i < n->module_count; ++i)
        if (string_at(&p, starts[4]+u16(bytes+starts[3]+i*2), t[4].end, &n->modules[i], NE_IMPORT_NAME)) return -1;
    if (relocations(&p, starts[4], t[4].end)) return -1;
    for (i = 0; i < 8; ++i) {
        if (t[i].end < t[i].start || t[i].end > size) return pf(&p, NE_TABLE_BOUNDS, t[i].start);
        for (j = 0; j < i; ++j)
            if (overlap(t[i].start, t[i].end-t[i].start, t[j].start, t[j].end-t[j].start)) return pf(&p, NE_OVERLAP, t[i].start);
    }
    for (i = 0; i < n->segment_count; ++i) {
        struct ne_segment *s = &n->segments[i];
        uint32_t len = s->stored_size + ((s->flags & NE_SEG_RELOC) ? 2+s->reloc_count*8 : 0);
        if (!s->sector) continue;
        for (j = 0; j < 8; ++j)
            if (overlap(s->file_offset, len, t[j].start, t[j].end-t[j].start)) return pf(&p, NE_OVERLAP, s->file_offset);
        for (j = 0; j < i; ++j) {
            struct ne_segment *prev = &n->segments[j];
            uint32_t plen = prev->stored_size + ((prev->flags & NE_SEG_RELOC) ? 2+prev->reloc_count*8 : 0);
            if (prev->sector && overlap(s->file_offset, len, prev->file_offset, plen)) return pf(&p, NE_OVERLAP, s->file_offset);
        }
    }
    for (i = 0; i < n->resource_count; ++i) {
        struct ne_resource *r = &n->resources[i];
        for (j = 0; j < 8; ++j)
            if (overlap(r->file_offset, r->size, t[j].start, t[j].end-t[j].start)) return pf(&p, NE_OVERLAP, r->file_offset);
        for (j = 0; j < n->segment_count; ++j) {
            struct ne_segment *s = &n->segments[j];
            uint32_t len = s->stored_size + ((s->flags & NE_SEG_RELOC) ? 2+s->reloc_count*8 : 0);
            if (s->sector && overlap(r->file_offset, r->size, s->file_offset, len)) return pf(&p, NE_OVERLAP, r->file_offset);
        }
        for (j = 0; j < i; ++j) {
            struct ne_resource *prev = &n->resources[j];
            if (overlap(r->file_offset, r->size, prev->file_offset, prev->size) &&
                !(r->file_offset == prev->file_offset && r->size == prev->size)) return pf(&p, NE_OVERLAP, r->file_offset);
        }
    }
    if (n->autodata > n->segment_count || (n->autodata && !(n->segments[n->autodata-1].flags & NE_SEG_DATA)) ||
        n->cs > n->segment_count || (n->cs && ((n->segments[n->cs-1].flags & NE_SEG_DATA) || n->ip >= n->segments[n->cs-1].allocation)) ||
        n->ss > n->segment_count || (n->ss && (!(n->segments[n->ss-1].flags & NE_SEG_DATA) ||
        (n->sp && n->sp > n->segments[n->ss-1].allocation)))) return pf(&p, NE_STARTUP, h+14);
    if (n->autodata) {
        /* SP=0 asks the loader to place the stack in DGROUP (Appendix K). */
        n->autodata_bytes = n->segments[n->autodata-1].allocation + n->heap +
                            ((n->ss == n->autodata && !n->sp) ? n->stack : 0u);
        if (n->autodata_bytes > NE_SEGMENT_BYTES) return pf(&p, NE_AUTODATA, h+14);
    }
    if (n->flags & NE_MODULE_SELFLOAD) {
        const struct ne_segment *s = n->segment_count ? &n->segments[0] : 0;
        n->selfload_state = 2;
        if (s && s->sector && !(s->flags & NE_SEG_ITERATED) && s->initialized_size >= 12u &&
            s->allocation >= NE_SELFLOAD_BYTES) {
            const uint8_t *q = bytes + s->file_offset;
            n->selfload_state = 1;
            n->selfload_version = u16(q);
            n->selfload_boot = u32(q+4); n->selfload_loadseg = u32(q+8);
            n->selfload_exit = s->initialized_size >= 28u ? u32(q+24) : 0u;
        }
    }
    n->valid = 1;
    return 0;
}
uint32_t ne_image_bytes(void) { return (uint32_t)sizeof(struct ne_image); }
int ne_load_policy(const struct ne_image *n, struct ne_error *error)
{
    if (error) zero(error, sizeof(*error));
    if (!n || !n->valid) return fail(error, NE_ARGUMENT, 0, 0, 0);
    /* Recognised and validated first, then refused until 19b's guarded
     * BootApp/LoadAppSeg bridge exists. Every case names its own rule. */
    if (n->selfload_state == 2) return fail(error, NE_SELFLOAD_HEADER, n->ne_offset+12, 1, 0);
    if (n->selfload_state == 1 && n->selfload_version != NE_SELFLOAD_MAGIC)
        return fail(error, NE_SELFLOAD_VERSION, n->segments[0].file_offset, 1, 0);
    if (n->selfload_state) return fail(error, NE_SELFLOAD, n->ne_offset+12, 0, 0);
    return 0;
}
int ne_load_segment(const struct ne_image *n, uint16_t number, struct ne_buffer out, struct ne_error *error)
{
    const struct ne_segment *s;
    uint32_t pos, done = 0;
    if (ne_load_policy(n, error)) return -1;
    if (!number || number > n->segment_count) return fail(error, NE_ARGUMENT, 0, number, 0);
    s = &n->segments[number-1];
    if (!out.bytes || out.size < s->allocation) return fail(error, NE_STORE, 0, number, 0);
    zero(out.bytes, s->allocation);
    if (!s->sector) return 0;
    if (!(s->flags & NE_SEG_ITERATED)) { copy(out.bytes, n->bytes+s->file_offset, s->stored_size); return 0; }
    pos = s->file_offset;
    while (pos < s->file_offset+s->stored_size) {
        uint32_t count = u16(n->bytes+pos), len = u16(n->bytes+pos+2);
        pos += 4;
        while (count--) { copy(out.bytes+done, n->bytes+pos, len); done += len; }
        pos += len;
    }
    return 0;
}
static int address(const struct ne_image *n, const struct ne_reloc *r, const struct ne_buffer *store,
                   ne_resolver resolve, void *arg, struct ne_address *a, struct ne_error *error, uint32_t rec)
{
    uint32_t kind = r->flags & 3;
    if (kind == 3) return fail(error, NE_OSFIXUP, 0, r->segment, rec);
    if (kind == 1 || kind == 2) {
        struct ne_string name = {0, 0};
        if (kind == 2) {
            uint32_t imp = n->ne_offset+u16(n->bytes+n->ne_offset+42);
            name.length = n->bytes[imp+r->target2]; name.offset = imp+r->target2+1;
        }
        a->offset = 0; a->selector = 0;
        if (!resolve || resolve(arg, n, r->target1, kind == 1 ? r->target2 : 0, name, a) || !a->selector)
            return fail(error, NE_IMPORT_MISSING, 0, r->segment, rec);
        return 0;
    }
    if ((r->target1 & 0xffu) == 0xffu) {
        const struct ne_entry *e = &n->entries[r->target2-1];
        a->offset = e->offset; a->selector = e->kind == NE_CONSTANT ? 0xffffu : store[e->segment-1].selector;
    } else { a->offset = r->target2; a->selector = store[r->target1-1].selector; }
    return 0;
}
int ne_relocate_segment(const struct ne_image *n, uint16_t number, const struct ne_buffer *store,
                       uint32_t store_count, ne_resolver resolve, void *arg,
                       struct ne_workspace *work, struct ne_error *error)
{
    const struct ne_segment *s;
    uint32_t i;
    if (ne_load_policy(n, error)) return -1;
    if (!number || number > n->segment_count || !store || !work) return fail(error, NE_ARGUMENT, 0, number, 0);
    if (store_count < n->segment_count) return fail(error, NE_STORE, 0, number, 0);
    for (i = 0; i < n->segment_count; ++i) {
        if (!store[i].bytes || store[i].size < n->segments[i].allocation || !store[i].selector || store[i].selector == 0xffffu)
            return fail(error, NE_STORE, 0, i+1, 0);
    }
    s = &n->segments[number-1];
    copy(work->bytes, store[number-1].bytes, s->allocation); zero(work->seen, sizeof(work->seen));
    for (i = s->reloc_first; i < s->reloc_first+s->reloc_count; ++i) {
        const struct ne_reloc *r = &n->relocs[i];
        struct ne_address a;
        uint32_t pos = r->source, w = width(r->source_type), additive = r->flags & 4;
        uint32_t visits = 0;
        if (address(n, r, store, resolve, arg, &a, error, i)) return -1;
        if (((r->source_type & 0x7f) == 3 || (r->source_type & 0x7f) == 5) && a.offset > 65535u)
            return fail(error, NE_RELOC_TARGET, 0, number, i);
        for (;;) {
            uint32_t k, footprint = (!additive && w == 1) ? 2 : w;
            uint16_t next = 0xffffu;
            uint8_t *dest;
            if (!fits(pos, footprint, s->allocation)) return fail(error, NE_PATCH_BOUNDS, 0, number, i);
            if (++visits > s->allocation) return fail(error, NE_CHAIN_CYCLE, 0, number, i);
            for (k = 0; k < footprint; ++k) {
                uint32_t at = pos+k;
                if (work->seen[at/8] & (1u << (at%8))) return fail(error, NE_CHAIN_CYCLE, 0, number, i);
                work->seen[at/8] |= (uint8_t)(1u << (at%8));
            }
            dest = work->bytes+pos;
            if (!additive) next = u16(dest); /* Capture link before overwriting. */
            switch (r->source_type & 0x7f) {
            case 0: dest[0] = (uint8_t)((additive ? dest[0] : 0)+a.offset); break;
            case 2:
                if (additive && u16(dest)) return fail(error, NE_ADDITIVE_SELECTOR, 0, number, i);
                w16(dest, a.selector); break;
            case 3:
                w16(dest, (uint16_t)((additive ? u16(dest) : 0)+a.offset)); w16(dest+2, a.selector); break;
            case 5: w16(dest, (uint16_t)((additive ? u16(dest) : 0)+a.offset)); break;
            case 11: w32(dest, (additive ? u32(dest) : 0)+a.offset); w16(dest+4, a.selector); break;
            case 13: w32(dest, (additive ? u32(dest) : 0)+a.offset); break;
            default: return fail(error, NE_RELOC_TYPE, 0, number, i);
            }
            if (additive || next == 0xffffu) break;
            pos = next;
        }
    }
    copy(store[number-1].bytes, work->bytes, s->allocation);
    return 0;
}
