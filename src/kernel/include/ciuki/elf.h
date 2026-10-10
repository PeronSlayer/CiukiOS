/* Restricted ELF32 loader and immutable executable snapshot boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ELF_H
#define CIUKI_ELF_H
#include <ciuki/uaddr.h>

struct ciuki_file {
    void *cookie;
    uint32_t bytes;
    int (*read)(void *cookie, uint32_t offset, void *dst, uint32_t bytes);
    void (*close)(void *cookie); /* nonblocking snapshot reference release */
};
/* open must freeze bytes/size against writes/truncation until close, return
 * a regular file and translate lookup errors to the native ABI PATH set.
 * It may block; copied path and retained cwd remain valid throughout. */
struct ciuki_file_ops {
    int (*open)(void *cwd, const char *path, struct ciuki_file *out);
};
struct elf_segment { uint32_t address, offset, filesz, bytes, prot; };
struct elf_image {
    uint32_t entry, end, bytes, count;
    struct elf_segment segments[CIUKI_ELF_PHDR_MAX];
};
int elf_validate(const struct ciuki_file *file, struct elf_image *image);
int elf_load(const struct ciuki_file *file, const struct elf_image *image, struct uaddr *u);

struct proc_strings {
    uint32_t argc, envc, used;
    uint32_t argv[CIUKI_ARGV_MAX], envp[CIUKI_ENVP_MAX];
    uint32_t pages[CIUKI_ARG_MAX / CIUKI_PAGE_SIZE];
};
struct proc_strings *proc_strings_new(void);
void proc_strings_free(struct proc_strings *strings);
int proc_strings_add(struct proc_strings *strings, const char *text, uint32_t bytes, bool env);
int proc_strings_copy(struct proc_strings *strings, uint32_t argv, uint32_t envp);
int proc_stack_build(struct uaddr *u, const struct proc_strings *strings,
                     uint32_t entry, uint32_t tls, uint32_t *esp);
#endif
