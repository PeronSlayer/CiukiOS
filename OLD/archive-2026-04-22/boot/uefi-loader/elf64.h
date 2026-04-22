#ifndef ELF64_H
#define ELF64_H

#include <efi.h>

#define EI_NIDENT   16

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   62

#define PT_LOAD     1

#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
    UINT8   e_ident[EI_NIDENT];
    UINT16  e_type;
    UINT16  e_machine;
    UINT32  e_version;
    UINT64  e_entry;
    UINT64  e_phoff;
    UINT64  e_shoff;
    UINT32  e_flags;
    UINT16  e_ehsize;
    UINT16  e_phentsize;
    UINT16  e_phnum;
    UINT16  e_shentsize;
    UINT16  e_shnum;
    UINT16  e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32  p_type;
    UINT32  p_flags;
    UINT64  p_offset;
    UINT64  p_vaddr;
    UINT64  p_paddr;
    UINT64  p_filesz;
    UINT64  p_memsz;
    UINT64  p_align;
} Elf64_Phdr;

#endif
