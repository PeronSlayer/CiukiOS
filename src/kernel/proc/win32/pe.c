/* Initial PE recognition/seed mapping; import binding belongs to ntdll.
 * Validation follows the f5-02 structural fixture parser.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/w32_objects.h>
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
    /* MZ candidates go through the named Win32 refusal path, including DOS
     * executables and malformed PE headers. Native ELF recognition is unchanged. */
    uint8_t magic[2];
    return !read_file(f,0,magic,2) && pe16(magic)==0x5a4d;
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

int w32_image_map(struct process *p,const struct ciuki_file *f,const struct w32_image *im,bool builtin)
{
    return w32_image_section_map(p,f,im,builtin);
}

/* Bootstrap export lookup on the already validated seed view. Full export
 * binding/forwarders remain in ntdll. Never execute the DLL's DllMain as the
 * process bootstrap. */
int w32_bootstrap_entry(struct process *p,const struct w32_image *im,uint32_t *entry)
{
    uint32_t r=im->directories[0],n=im->directories[1],head[10];
    if(n<40 || r>im->bytes-40 || ua_read(p->memory,head,im->base+r,40))return -ENOEXEC;
    if(head[5]>65536 || head[6]>65536 || head[7]>im->bytes || head[8]>im->bytes || head[9]>im->bytes ||
       head[5]>(im->bytes-head[7])/4 || head[6]>(im->bytes-head[8])/4 || head[6]>(im->bytes-head[9])/2)return -ENOEXEC;
    for(uint32_t i=0;i<head[6];i++){
        uint32_t name;uint16_t ordinal;char text[18];
        if(ua_read(p->memory,&name,im->base+head[8]+i*4,4)||name>im->bytes-sizeof(text)||
           ua_read(p->memory,text,im->base+name,sizeof(text)))return -ENOEXEC;
        if(memcmp(text,"CiukiProcessStart",18))continue;
        if(ua_read(p->memory,&ordinal,im->base+head[9]+i*2,2)||ordinal>=head[5]||
           ua_read(p->memory,&name,im->base+head[7]+ordinal*4,4)||name>=im->bytes||
           (name>=r&&name-r<n)||!ua_range(p->memory,im->base+name,1,PROT_EXEC))return -ENOEXEC;
        *entry=im->base+name;return 0;
    }
    return -ENOEXEC;
}
