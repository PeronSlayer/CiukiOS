OUTPUT_FORMAT(elf64-x86-64)
ENTRY(com_main)

SECTIONS
{
  . = 0;

  .text : {
    *(.text*)
    *(.rodata*)
  }

  .data : {
    *(.data*)
  }

  .bss : {
    *(.bss*)
    *(COMMON)
  }
}