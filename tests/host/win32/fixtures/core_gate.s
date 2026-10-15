# Self-authored builtin-gate and i386 exception-dispatch bridge.
# SPDX-License-Identifier: MIT
.text
.globl _core_call
_core_call:
 pushl %ebp
 pushl %ebx
 pushl %esi
 pushl %edi
 movl 20(%esp),%ebx
 movl 24(%esp),%ecx
 movl $96,%eax
 xorl %edx,%edx
 xorl %esi,%esi
 xorl %edi,%edi
 xorl %ebp,%ebp
 pushl $1f
 pushl $0x7ffe0100
 retl
1:
 popl %edi
 popl %esi
 popl %ebx
 popl %ebp
 retl
.globl _core_dispatch
_core_dispatch:
 pushl 8(%esp)
 pushl 8(%esp)
 calll _core_dispatch_c
 ud2
.globl _core_stack_grow
_core_stack_grow:
 subl $4092,%esp
 movb $0,(%esp)
 calll _core_stack_grow
 ud2
