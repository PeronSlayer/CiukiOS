/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors.
 * Shared F5/F6 PE mechanism. LGPL-2.1-or-later permits GPL-2.0-only use
 * under LGPL section 3; preserve this notice and the original license. */
#ifndef CIUKI_PE_MECHANISM_H
#define CIUKI_PE_MECHANISM_H
#include <stdint.h>
#include <stddef.h>
struct pe_image;
struct pe_ops {
    void *context;
    int (*admit)(void *, struct pe_image *);
    void (*diagnostic)(void *, const char *);
    int (*writable)(void *, struct pe_image *, uint32_t, uint32_t);
    /* Allocator returns a privately owned view and its actual PE32 VA.
     * Consumer owns rollback, lifetime, mapping permissions and residency. */
    void *(*allocate)(void *, uint32_t preferred, uint32_t bytes, uint32_t *base);
    void (*release)(void *, void *, uint32_t);
};
struct pe_error {
    uint32_t code;
    const char *reason;
    char module[1024], symbol[256];
};
struct pe_section {
    uint32_t rva, span, raw, offset, flags;
};
struct pe_directory {
    uint32_t rva, size;
};
struct pe_image {
    const uint8_t *file;
    uint32_t file_size, preferred, base, size, headers, entry, section_alignment;
    uint16_t flags, subsystem, section_count, subsystem_major, subsystem_minor;
    struct pe_section sections[96];
    struct pe_directory dirs[16];
    uint8_t *image;
    struct pe_error *error;
    const struct pe_ops *ops;
};

static inline uint16_t pe_u16(const void *v) {
    const uint8_t *p=v; return (uint16_t)(p[0] | (uint16_t)p[1]<<8);
}
static inline uint32_t pe_u32(const void *v) {
    const uint8_t *p=v; return pe_u16(p) | (uint32_t)pe_u16(p+2)<<16;
}
static inline void pe_put16(void *v, uint16_t n) {
    uint8_t *p=v; p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8);
}
static inline void pe_put32(void *v, uint32_t n) {
    uint8_t *p=v; pe_put16(p,(uint16_t)n); pe_put16(p+2,(uint16_t)(n>>16));
}
int pe_fail(struct pe_image *, const char *);
int pe_parse(struct pe_image *, const void *, uint32_t, struct pe_error *, const struct pe_ops *);
int pe_copy_image(struct pe_image *, void *);
int pe_allocate_image(struct pe_image *);
void *pe_span(struct pe_image *, uint32_t, uint32_t);
const char *pe_string(struct pe_image *, uint32_t);
int pe_relocate(struct pe_image *, uint32_t);
int pe_validate_mechanism(struct pe_image *);
int pe_write(struct pe_image *, uint32_t, uint32_t);
/* descriptor callback runs even for an empty thunk list, preserving DLL
 * dependencies. name==NULL denotes ordinal; all PE fields are 32-bit. */
struct pe_import {
    const char *module, *name;
    uint32_t descriptor, iat;
    uint16_t ordinal, hint;
};
typedef int (*pe_import_fn)(void *, const struct pe_import *);
int pe_walk_imports(struct pe_image *, void *, pe_import_fn descriptor, pe_import_fn symbol);
#endif
