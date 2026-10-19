/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors.
 * Microsoft i386 compiler helpers that NT NTDLL exports and the reused
 * Wine 10.0 DLLs import: _alldiv/_allrem/_aulldiv take two 64-bit operands
 * on the stack, return EDX:EAX, pop 16 bytes and preserve EBX/ESI/EDI/EBP.
 * A zero divisor executes DIV by zero, so the caller receives the hardware
 * integer-divide exception exactly as with an inline DIV. _chkstk is the
 * stack-allocation probe shared with the f5-07A facades. */
#ifdef _WIN32
#define SYM(x) _##x
#else
#define SYM(x) x
#endif
.text
.globl SYM(_alldiv)
SYM(_alldiv):
    movl $0,%ecx
    jmp 1f
.globl SYM(_allrem)
SYM(_allrem):
    movl $1,%ecx
    jmp 1f
.globl SYM(_aulldiv)
SYM(_aulldiv):
    movl $2,%ecx
1:
    movl 12(%esp),%eax
    orl 16(%esp),%eax
    jnz 2f
    xorl %edx,%edx
    divl %eax
2:
    pushl %ecx
    pushl 20(%esp)
    pushl 20(%esp)
    pushl 20(%esp)
    pushl 20(%esp)
    call SYM(w32_divmod64)
    addl $20,%esp
    retl $16
#include "../services/stack.S"
