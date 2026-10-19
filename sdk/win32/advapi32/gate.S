# SPDX-License-Identifier: LGPL-2.1-or-later
.text
.globl _w32r_gate
_w32r_gate:
 pushl %ebp
 pushl %ebx
 pushl %esi
 pushl %edi
 movl 20(%esp),%eax
 movl 24(%esp),%ebx
 movl 28(%esp),%ecx
 movl 32(%esp),%edx
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
