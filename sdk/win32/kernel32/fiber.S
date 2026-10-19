/* SPDX-License-Identifier: LGPL-2.1-or-later */
.text
.globl _k32_fiber_switch
_k32_fiber_switch:
    movl 4(%esp),%eax
    movl %ebx,0(%eax)
    movl %esi,4(%eax)
    movl %edi,8(%eax)
    movl %ebp,12(%eax)
    movl %esp,16(%eax)
    fnstcw 20(%eax)
    movl 8(%esp),%eax
    movl 0(%eax),%ebx
    movl 4(%eax),%esi
    movl 8(%eax),%edi
    movl 12(%eax),%ebp
    fldcw 20(%eax)
    movl 16(%eax),%esp
    ret
