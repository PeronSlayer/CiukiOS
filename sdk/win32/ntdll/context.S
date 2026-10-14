/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#ifdef _WIN32
#define CAP _RtlCaptureContext@4
#define RAISE _RtlRaiseException@4
#define RAISE_C _w32_raise_c@8
#define UNWIND _RtlUnwind@16
#define UNWIND_C _w32_unwind_c@20
#else
#define CAP RtlCaptureContext
#define RAISE RtlRaiseException
#define RAISE_C w32_raise_c
#define UNWIND RtlUnwind
#define UNWIND_C w32_unwind_c
#endif
.text
/* No FXSAVE: the baseline includes processors without FXSR. Capture FULL
 * plus the legacy FPU image. Kernel signal delivery supplies extended state. */
.globl CAP
CAP:
    pushl %eax
    movl 8(%esp),%eax
    movl $0x1000f,0(%eax)
    movl %edi,156(%eax)
    movl %esi,160(%eax)
    movl %ebx,164(%eax)
    movl %edx,168(%eax)
    movl %ecx,172(%eax)
    movl (%esp),%edx
    movl %edx,176(%eax)
    movl %ebp,180(%eax)
    movl 4(%esp),%edx
    movl %edx,184(%eax)
    leal 12(%esp),%edx
    movl %edx,196(%eax)
    xorl %edx,%edx
    movw %gs,%dx
    movl %edx,140(%eax)
    movw %fs,%dx
    movl %edx,144(%eax)
    movw %es,%dx
    movl %edx,148(%eax)
    movw %ds,%dx
    movl %edx,152(%eax)
    movw %cs,%dx
    movl %edx,188(%eax)
    movw %ss,%dx
    movl %edx,200(%eax)
    pushfl
    popl 192(%eax)
    fnsave 28(%eax)
    frstor 28(%eax)
    movl $0,136(%eax)
    popl %eax
    ret $4

.globl RAISE
RAISE:
    subl $716,%esp
    pushl %esp
    call CAP
    leal 720(%esp),%eax
    movl %eax,196(%esp)
    movl 716(%esp),%eax
    movl %eax,184(%esp)
    /* Resume after stdcall consumes its one argument. */
    addl $4,196(%esp)
    movl %esp,%eax
    pushl %eax
    pushl 724(%esp)
    call RAISE_C
    ud2

.globl UNWIND
UNWIND:
    subl $716,%esp
    pushl %esp
    call CAP
    leal 736(%esp),%eax
    movl %eax,196(%esp)
    movl 716(%esp),%eax
    movl %eax,184(%esp)
    movl %esp,%eax
    pushl %eax
    /* Each push advances the original argument by four bytes. */
    pushl 736(%esp)
    pushl 736(%esp)
    pushl 736(%esp)
    pushl 736(%esp)
    call UNWIND_C
    ud2
#ifndef _WIN32
.section .note.GNU-stack,"",@progbits
#endif
