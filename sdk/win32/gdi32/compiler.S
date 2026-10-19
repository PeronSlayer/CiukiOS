# SPDX-License-Identifier: LGPL-2.1-or-later
.text
.globl __alldiv
__alldiv:
    jmp _gdi_sdiv@16
.globl __allrem
__allrem:
    jmp _gdi_srem@16
.globl __allmul
__allmul:
    pushl %ebx
    movl 8(%esp),%eax
    mull 16(%esp)
    movl 12(%esp),%ebx
    imull 16(%esp),%ebx
    addl %ebx,%edx
    movl 8(%esp),%ebx
    imull 20(%esp),%ebx
    addl %ebx,%edx
    popl %ebx
    retl $16
