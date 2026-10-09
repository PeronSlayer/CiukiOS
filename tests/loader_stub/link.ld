/* Exercise virtual ELF e_entry -> physical PT_LOAD translation. */
OUTPUT_FORMAT(elf32-i386)
ENTRY(stub_entry)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0xC0100000;
  .text : AT(ADDR(.text) - 0xC0000000) { *(.text) *(.rodata) } :text
  . = ALIGN(4096);
  .data : AT(ADDR(.data) - 0xC0000000) { *(.data) } :data
  .bss (NOLOAD) : AT(ADDR(.bss) - 0xC0000000) { *(.bss) } :data
  /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
