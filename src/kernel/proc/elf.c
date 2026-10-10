/* ELF32 static ET_EXEC profile. All arithmetic is checked before mapping.
 * ELF specification: https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.pheader.html
 * Ciuki restrictions: execution-abi.md, "ELF loading and initial state".
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/process.h>

struct elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct elf_phdr { uint32_t type, offset, va, pa, filesz, memsz, flags, align; };
struct elf_shdr { uint32_t name, type, flags, addr, offset, size, link, info, align, entsize; };
_Static_assert(sizeof(struct elf_header) == 52, "ELF32 header");
_Static_assert(sizeof(struct elf_phdr) == 32, "ELF32 phdr");
_Static_assert(sizeof(struct elf_shdr) == 40, "ELF32 shdr");

static bool file_range(const struct ciuki_file *f, uint32_t off, uint32_t bytes)
{
    return off <= f->bytes && bytes <= f->bytes - off;
}

static int read_at(const struct ciuki_file *f, uint32_t off, void *dst, uint32_t bytes)
{
    struct process *caller = proc_current();
    if (caller && caller->state == PROC_STOPPING)
        return -EINTR;
    if (!file_range(f, off, bytes))
        return -ENOEXEC;
    return f->read(f->cookie, off, dst, bytes);
}

int elf_validate(const struct ciuki_file *f, struct elf_image *image)
{
    struct elf_header h;
    memset(image, 0, sizeof(*image));
    if (!f->read || f->bytes > CIUKI_ELF_BYTES_MAX)
        return -ENOEXEC;
    int err = read_at(f, 0, &h, sizeof(h));
    if (err)
        return err;
    if (memcmp(h.ident, "\177ELF", 4) || h.ident[4] != 1 || h.ident[5] != 1 ||
        h.ident[6] != 1 || h.ident[7] || h.ident[8] || h.type != 2 || h.machine != 3 ||
        h.version != 1 || h.flags || h.ehsize != sizeof(h) ||
        h.phentsize != sizeof(struct elf_phdr) || !h.phnum || h.phnum > CIUKI_ELF_PHDR_MAX ||
        !file_range(f, h.phoff, h.phnum * sizeof(struct elf_phdr)))
        return -ENOEXEC;
    if (h.shoff) {
        if (!h.shnum || h.shentsize != sizeof(struct elf_shdr) ||
            (h.shstrndx && h.shstrndx >= h.shnum) ||
            !file_range(f, h.shoff, h.shnum * sizeof(struct elf_shdr)))
            return -ENOEXEC;
        for (unsigned i = 0; i < h.shnum; i++) {
            struct elf_shdr s;
            err = read_at(f, h.shoff + i * sizeof(s), &s, sizeof(s));
            if (err)
                return err;
            if (s.type == 6 || (s.type != 0 && s.type != 8 && !file_range(f, s.offset, s.size)))
                return -ENOEXEC;
        }
    } else if (h.shnum || h.shstrndx) {
        return -ENOEXEC;
    }
    bool entry = false;
    for (unsigned i = 0; i < h.phnum; i++) {
        struct elf_phdr p;
        err = read_at(f, h.phoff + i * sizeof(p), &p, sizeof(p));
        if (err)
            return err;
        if (p.type == 0)
            continue;                       /* PT_NULL fields are undefined */
        if (!file_range(f, p.offset, p.filesz) || p.filesz > p.memsz ||
            p.memsz > UINT32_MAX - p.va)
            return -ENOEXEC;
        if (p.type == 4 || p.type == 6)     /* bounded NOTE/PHDR metadata */
            continue;
        if (p.type == 0x6474e551) {        /* PT_GNU_STACK */
            if (p.flags & 1)
                return -ENOEXEC;
            continue;
        }
        if (p.type != 1)                  /* includes INTERP, DYNAMIC, TLS */
            return -ENOEXEC;
        if (!p.memsz)
            continue;
        if (p.align != CIUKI_PAGE_SIZE || (p.va & (CIUKI_PAGE_SIZE - 1)) ||
            (p.offset & (CIUKI_PAGE_SIZE - 1)) || !(p.flags & 4) || (p.flags & ~7u) ||
            (p.flags & 3) == 3 || p.va < CIUKI_IMAGE_BASE || p.va >= CIUKI_IMAGE_LIMIT ||
            p.memsz > CIUKI_IMAGE_LIMIT - p.va)
            return -ENOEXEC;
        uint32_t bytes = PAGE_ALIGN_UP(p.memsz), end = p.va + bytes;
        if (bytes > CIUKI_ELF_BYTES_MAX - image->bytes)
            return -ENOEXEC;
        for (unsigned j = 0; j < image->count; j++) {
            const struct elf_segment *s = &image->segments[j];
            if (p.va < s->address + s->bytes && end > s->address)
                return -ENOEXEC;
        }
        image->segments[image->count++] = (struct elf_segment){ p.va, p.offset, p.filesz, bytes,
            PROT_READ | ((p.flags & 2) ? PROT_WRITE : 0) | ((p.flags & 1) ? PROT_EXEC : 0) };
        image->bytes += bytes;
        if (end > image->end)
            image->end = end;
        if ((p.flags & 3) == 1 && h.entry >= p.va && h.entry - p.va < p.filesz)
            entry = true;
    }
    if (!entry)
        return -ENOEXEC;
    image->entry = h.entry;
    return 0;
}

int elf_load(const struct ciuki_file *file, const struct elf_image *image, struct uaddr *u)
{
    /* The caller owns a provisional address space; every failure destroys
     * it through proc_discard, so no partially loaded child is published. */
    uint8_t buffer[512];
    for (unsigned i = 0; i < image->count; i++) {
        struct process *caller = proc_current();
        if (caller && caller->state == PROC_STOPPING)
            return -EINTR;
        const struct elf_segment *s = &image->segments[i];
        int err = ua_map_at(u, s->address, s->bytes, s->prot, s->prot, UA_IMAGE, 0);
        if (err)
            return err;
        for (uint32_t off = 0; off < s->filesz;) {
            struct process *caller = proc_current();
            if (caller && caller->state == PROC_STOPPING)
                return -EINTR;
            uint32_t n = s->filesz - off;
            if (n > sizeof(buffer))
                n = sizeof(buffer);
            err = read_at(file, s->offset + off, buffer, n);
            if (!err)
                err = ua_write(u, s->address + off, buffer, n);
            if (err)
                return err;
            off += n;
        }
    }
    u->heap_base = u->brk = image->end;
    return 0;
}
