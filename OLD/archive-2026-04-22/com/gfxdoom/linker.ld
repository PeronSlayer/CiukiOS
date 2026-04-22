ENTRY(com_main)

SECTIONS
{
    . = 0x600100;

    .text ALIGN(16) : {
        *(.text*)
    }

    .rodata ALIGN(16) : {
        *(.rodata*)
    }

    .data ALIGN(16) : {
        LONG(0xC1CEED17);
        *(.data*)
        *(COMMON)
        *(.bss*)
        . = ALIGN(16);
    }
}
