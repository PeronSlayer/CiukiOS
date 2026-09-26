OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { KEEP(*(.start)) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  module_image_end = .;
  .bss : { _bss_start = .; *(.bss*) *(COMMON) _bss_end = .; }
  . = ALIGN(16);
  . += 8192;
  _stack_top = .;
  module_paragraphs = (_stack_top + 15) / 16;
  ASSERT(_stack_top < 0xef00, "MEDIA.DRV exceeds its segment")
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
