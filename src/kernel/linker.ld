/* Ciuki VMM linker script: virtual 0xC0100000, physical 0x00100000.
 * docs/design/execution-abi.md, docs/design/boot-memory.md.
 * SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

KERNEL_VBASE = 0xC0000000;

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R W */
}

SECTIONS
{
    . = 0xC0100000;
    __kernel_start = .;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VBASE)
    {
        __text_start = .;
        *(.text.entry)
        *(.text .text.*)
        __text_end = .;
    } :text

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VBASE)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4);
        __f1probes_start = .;
        KEEP(*(.f1probes))
        __f1probes_end = .;
        __rodata_end = .;
    } :rodata

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VBASE)
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :data

    .bss ALIGN(4096) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VBASE)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    } :data

    __kernel_end = .;

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
