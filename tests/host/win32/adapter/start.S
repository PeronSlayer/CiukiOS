/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
.text
.globl _start
_start:
    cld
    movl %esp,%eax
    andl $-16,%esp
    subl $12,%esp
    pushl %eax
    call host_start
    ud2
.globl host_sigreturn
host_sigreturn:
    movl $173,%eax
    int $0x80
    ud2
.globl host_entry
host_entry:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp,%ebx
    movl 24(%esp),%esp
    andl $-16,%esp
    subl $12,%esp
    pushl %ebx
    call *20(%ebx)
    popl %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
.globl host_resume
host_resume:
    movl 4(%esp),%eax
    /* Restore x87 only when supplied; this never requires SSE/FXSR. */
    testl $8,0(%eax)
    jz 1f
    frstor 28(%eax)
1:
    movl 196(%eax),%edx
    subl $4,%edx
    movl 184(%eax),%ecx
    movl %ecx,(%edx)
    pushl 192(%eax)
    popfl
    movl 156(%eax),%edi
    movl 160(%eax),%esi
    movl 164(%eax),%ebx
    movl 180(%eax),%ebp
    movl %edx,%esp
    movl 168(%eax),%edx
    movl 172(%eax),%ecx
    movl 176(%eax),%eax
    ret
.section .note.GNU-stack,"",@progbits
