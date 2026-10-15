/* Initial PE recognition/seed mapping; import binding belongs to ntdll.
 * Validation follows the f5-02 structural fixture parser.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/win32.h>
static uint16_t pe16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t pe32(const uint8_t *p) { return (uint32_t)pe16(p) | (uint32_t)pe16(p + 2) << 16; }
static bool power2(uint32_t n) { return n && !(n & (n - 1)); }
static int read_file(const struct ciuki_file *f, uint32_t off, void *dst, uint32_t n)
{
    if (off > f->bytes || n > f->bytes - off) return -ENOEXEC;
    return f->read(f->cookie, off, dst, n);
}
bool w32_is_pe(const struct ciuki_file *f)
{
    uint8_t dos[64], pe[26];
    if (read_file(f, 0, dos, sizeof(dos)) || pe16(dos) != 0x5a4d) return false;
    uint32_t off = pe32(dos + 60);
    if ((off & 3) || off < 64 || read_file(f, off, pe, sizeof(pe))) return false;
    return pe32(pe) == 0x4550 && pe16(pe + 4) == 0x14c && pe16(pe + 24) == 0x10b;
}
#define PE_CHECK(test, reason) do { if (!(test)) { im->failure = reason; return -ENOEXEC; } } while (0)
int w32_pe_validate(const struct ciuki_file *f, struct w32_image *im)
{
    uint8_t dos[64], pe[24], op[96], sec[40], directory[8];
    memset(im, 0, sizeof(*im));
    PE_CHECK(!read_file(f, 0, dos, 64), "dos-header-truncated");
    PE_CHECK(pe16(dos) == 0x5a4d, "dos-magic");
    uint32_t off = pe32(dos + 60);
    PE_CHECK(!(off & 3), "pe-offset-alignment");
    PE_CHECK(off >= 64 && !read_file(f, off, pe, 24), "pe-header-bounds");
    PE_CHECK(pe32(pe) == 0x4550, "pe-signature");
    PE_CHECK(pe16(pe + 4) == 0x14c, "machine-not-i386");
    im->count = pe16(pe + 6); im->flags = pe16(pe + 22);
    PE_CHECK(im->count && im->count <= 96, "section-count");
    PE_CHECK(im->flags & 2, "not-executable-image");
    uint32_t olen = pe16(pe + 20), o = off + 24;
    PE_CHECK(olen >= 96 && olen <= f->bytes - o && !read_file(f, o, op, 96), "optional-header-bounds");
    PE_CHECK(pe16(op) == 0x10b, "optional-magic-not-pe32");
    uint32_t nd = pe32(op + 92);
    PE_CHECK(nd <= 16 && olen >= 96 + nd * 8, "directory-count");
    im->base = pe32(op + 28); im->entry = pe32(op + 16);
    uint32_t sa = pe32(op + 32), fa = pe32(op + 36);
    PE_CHECK(power2(fa) && ((fa >= 512 && fa <= 65536) || (sa < PAGE_SIZE && fa == sa)), "file-alignment");
    PE_CHECK(power2(sa) && sa >= fa, "section-alignment");
    im->bytes = pe32(op + 56); im->headers = pe32(op + 60);
    PE_CHECK(im->bytes && im->bytes <= 512u * 1024 * 1024 && !(im->bytes % sa), "image-size");
    PE_CHECK(im->base <= UINT32_MAX - im->bytes, "image-address-wrap");
    uint32_t table = o + olen;
    PE_CHECK(table <= f->bytes && im->count * 40 <= f->bytes - table, "section-table-bounds");
    PE_CHECK(table + im->count * 40 <= im->headers && im->headers <= f->bytes && im->headers <= im->bytes, "headers-size");
    PE_CHECK(pe16(op + 68) == 2 || pe16(op + 68) == 3, "unsupported-subsystem");
    PE_CHECK(im->entry < im->bytes, "entrypoint-bounds");
    im->stack_reserve = pe32(op + 72); im->stack_commit = pe32(op + 76);
    for (unsigned i = 0; i < im->count; i++) {
        int err = read_file(f, table + i * 40, sec, 40);
        if (err) return err;
        struct w32_section *s = &im->section[i];
        uint32_t vs = pe32(sec + 8);
        s->rva = pe32(sec + 12); s->raw = pe32(sec + 16); s->offset = pe32(sec + 20);
        s->bytes = vs > s->raw ? vs : s->raw; s->flags = pe32(sec + 36);
        PE_CHECK(s->rva <= im->bytes && s->bytes <= im->bytes - s->rva, "section-image-bounds");
        PE_CHECK(!(s->rva % sa), "section-rva-alignment");
        PE_CHECK(!s->raw || !(s->offset % fa), "section-raw-alignment");
        PE_CHECK(s->offset <= UINT32_MAX - s->raw, "section-raw-wrap");
        PE_CHECK(!s->bytes || s->rva >= im->headers, "section-header-overlap");
        for (unsigned j = 0; j < i; j++) {
            const struct w32_section *prev = &im->section[j];
            PE_CHECK(!s->bytes || !prev->bytes || s->rva >= prev->rva + prev->bytes ||
                     s->rva + s->bytes <= prev->rva, "section-virtual-overlap");
        }
    }
    for (unsigned i = 0; i < nd; i++) {
        int err = read_file(f, o + 96 + i * 8, directory, 8);
        if (err) return err;
        uint32_t r = pe32(directory), n = pe32(directory + 4);
        if (n) {
            if (i == 4) PE_CHECK(!(r & 7) && r <= f->bytes && n <= f->bytes - r, "certificate-file-bounds");
            else PE_CHECK(r && r <= im->bytes && n <= im->bytes - r, "directory-image-bounds");
        }
        im->directories[i * 2] = r; im->directories[i * 2 + 1] = n;
    }
    if (pe16(op + 48) > 5) klog("[win32] load warning=newer-subsystem-version major=%u", pe16(op + 48));
    return 0;
}
#undef PE_CHECK

static int seed_bytes(struct uaddr *u, uint32_t address, const struct ciuki_file *f, uint32_t off, uint32_t bytes)
{
    uint8_t buffer[256];
    if (off >= f->bytes) return 0; /* bounded raw tail, already zero committed */
    if (bytes > f->bytes - off) bytes = f->bytes - off;
    while (bytes) {
        uint32_t n = bytes < sizeof(buffer) ? bytes : sizeof(buffer);
        int err = read_file(f, off, buffer, n);
        if (!err) err = ua_write(u, address, buffer, n);
        if (err) return err;
        bytes -= n; off += n; address += n;
        kpreempt_point();
    }
    return 0;
}

int w32_image_map(struct process *p, const struct ciuki_file *f, const struct w32_image *im, bool builtin)
{
    struct uaddr *u = p->memory;
    uint32_t limit = builtin ? CIUKI_W32_BUILTIN_LIMIT : CIUKI_W32_APP_LIMIT;
    if (im->base < (builtin ? CIUKI_W32_APP_LIMIT : CIUKI_W32_LOW) || (im->base & 65535) ||
        im->bytes > limit - im->base || im->base >= limit) return -ENOEXEC;
    uint32_t bytes = PAGE_ALIGN_UP(im->bytes);
    if (ua_find(u, im->base)) return -ENOEXEC;
    /* A builtin seed uses an internal fixed-range reservation. */
    int err = ua_map_at(u, im->base, bytes, 0, 7, UA_W32, 0);
    if (err) return err;
    struct ua_extent *e = ua_find(u, im->base);
    e->w32_allocation = im->base; e->w32_allocation_protect = W32_PAGE_READONLY;
    e->w32_state = W32_MEM_RESERVE; e->w32_type = W32_MEM_IMAGE;
    err = ua_w32_change(u, im->base, bytes, W32_VM_ALLOC, W32_PAGE_NOACCESS, 0, true);
    if (!err) err = seed_bytes(u, im->base, f, 0, im->headers);
    for (unsigned i = 0; !err && i < im->count; i++)
        err = seed_bytes(u, im->base + im->section[i].rva, f, im->section[i].offset, im->section[i].raw);
    /* Low-alignment sections may share pages. Union their access bits. */
    for (uint32_t page = 0; !err && page < bytes; page += PAGE_SIZE) {
        uint32_t prot = page < im->headers ? PROT_READ : 0;
        for (unsigned i = 0; i < im->count; i++) {
            const struct w32_section *s = &im->section[i];
            if (!s->bytes || page >= s->rva + s->bytes || page + PAGE_SIZE <= s->rva) continue;
            if (s->flags & 0x40000000) prot |= PROT_READ;
            if (s->flags & 0x80000000) prot |= PROT_READ | PROT_WRITE;
            if (s->flags & 0x20000000) prot |= PROT_READ | PROT_EXEC;
        }
        uint32_t wp = !prot ? W32_PAGE_NOACCESS : prot & PROT_EXEC ?
            (prot & PROT_WRITE ? W32_PAGE_EXECUTE_READWRITE : W32_PAGE_EXECUTE_READ) :
            (prot & PROT_WRITE ? W32_PAGE_READWRITE : W32_PAGE_READONLY);
        err = ua_w32_change(u, im->base + page, PAGE_SIZE, W32_VM_PROTECT, wp, prot, true);
    }
    if (!err && !ua_range(u, im->base + im->entry, 1, PROT_EXEC)) err = -ENOEXEC;
    if (err) { ua_w32_release(u, im->base, true); return err; }
    if (builtin)
        for (e = ua_find(u, im->base); e && e->w32_allocation == im->base; e = e->next) e->w32_locked = true;
    return 0;
}
