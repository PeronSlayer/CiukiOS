ENTRY(com_main)

SECTIONS
{
    . = 0x100;

    .text ALIGN(16) : {
        *(.text*)
    }

    .rodata ALIGN(16) : {
        *(.rodata*)
    }

    .data ALIGN(16) : {
        *(.data*)
    }

    .bss ALIGN(16) : {
        *(COMMON)
        *(.bss*)
    }
}
