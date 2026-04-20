/*
 * OPENGEM-044 Task B — legacy PM32 placeholder body.
 *
 * Stage 1 contract: write a deterministic marker to port 0xE9 and
 * return. This body does not install descriptors, does not load TR,
 * and does not enter v86 yet.
 */

    .section .rodata
    .global legacy_v86_pm32_sentinel
legacy_v86_pm32_sentinel:
    .asciz "OPENGEM-044-B"

    .section .text
    .code32
    .global legacy_v86_pm32_body
    .type legacy_v86_pm32_body, @function
legacy_v86_pm32_body:
    /* entry marker: lowercase 'b' on 0xE9 */
    movb    $'b', %al
    outb    %al, $0xE9

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
    movb    $'B', %al
    outb    %al, %dx

    /* exit marker: lowercase 'r' on 0xE9 just before retl */
    movb    $'r', %al
    outb    %al, $0xE9

    retl

    .code64
    .size legacy_v86_pm32_body, .-legacy_v86_pm32_body

    .section .note.GNU-stack,"",@progbits