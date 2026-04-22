/*
 * OPENGEM-044 Stage 3B — dedicated PM32 smoke body.
 *
 * Contract: write "OPENGEM-044-RT" to debugcon port 0xE9 and return
 * cleanly to the mode-switch trampoline.
 */

    .section .rodata
    .global mstest_pm32_sentinel
mstest_pm32_sentinel:
    .asciz "OPENGEM-044-RT"

    .section .text
    .code32
    .global mstest_pm32_body
    .type mstest_pm32_body, @function
mstest_pm32_body:
    movw    $0x00E9, %dx

    movb    $'O', %al
    outb    %al, %dx
    movb    $'P', %al
    outb    %al, %dx
    movb    $'E', %al
    outb    %al, %dx
    movb    $'N', %al
    outb    %al, %dx
    movb    $'G', %al
    outb    %al, %dx
    movb    $'E', %al
    outb    %al, %dx
    movb    $'M', %al
    outb    %al, %dx
    movb    $'-', %al
    outb    %al, %dx
    movb    $'0', %al
    outb    %al, %dx
    movb    $'4', %al
    outb    %al, %dx
    movb    $'4', %al
    outb    %al, %dx
    movb    $'-', %al
    outb    %al, %dx
    movb    $'R', %al
    outb    %al, %dx
    movb    $'T', %al
    outb    %al, %dx

    xorl    %eax, %eax
    retl

    .code64
    .size mstest_pm32_body, .-mstest_pm32_body

    .section .note.GNU-stack,"",@progbits