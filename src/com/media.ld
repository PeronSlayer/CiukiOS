OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)
SECTIONS {
  . = 0x100;
  .text : { *(.start) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { _bss_start = .; *(.bss*) *(COMMON) _bss_end = .; }
  . = ALIGN(16);
  . += 4096;
  _stack_top = .;
  ASSERT(_stack_top < 0xef00, "MEDIA exceeds COM memory budget")
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
