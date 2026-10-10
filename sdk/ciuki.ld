/* abi.ld is generated from ciuki/abi.h by the SDK builder. */
INCLUDE abi.ld
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)
PHDRS {
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
  stack PT_GNU_STACK FLAGS(6);
}
SECTIONS {
  . = CIUKI_IMAGE_BASE;
  .text : { KEEP(*(.text.start)) *(.text .text.*) } :text
  . = ALIGN(CIUKI_PAGE_SIZE);
  .rodata : { *(.rodata .rodata.*) *(.eh_frame*) } :rodata
  . = ALIGN(CIUKI_PAGE_SIZE);
  .data : { *(.data .data.*) } :data
  .preinit_array : { __preinit_array_start = .; KEEP(*(.preinit_array*)) __preinit_array_end = .; } :data
  .init_array : { __init_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .; } :data
  .fini_array : { __fini_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array)) __fini_array_end = .; } :data
  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
  .tdata : { *(.tdata .tdata.*) } :data
  .tbss (NOLOAD) : { *(.tbss .tbss.*) } :data
  /DISCARD/ : { *(.comment) *(.note.GNU-stack) }
  ASSERT(SIZEOF(.tdata) == 0 && SIZEOF(.tbss) == 0, "compiler TLS is excluded")
}
