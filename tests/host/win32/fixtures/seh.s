# Self-authored FS-linked SEH records; runtime exception dispatch is pending.
# SPDX-License-Identifier: MIT
.text
.globl _entry
_entry:
 pushl %ebp
 movl %esp, %ebp
 pushl $_outer_handler
 pushl %fs:0
 movl %esp, %fs:0
#if NESTED
 pushl $_inner_handler
 pushl %fs:0
 movl %esp, %fs:0
#endif
# UD2 raises STATUS_ILLEGAL_INSTRUCTION. Handler skips exactly two bytes.
 ud2
#if NESTED
 popl %fs:0
 addl $4, %esp
#endif
 popl %fs:0
 addl $4, %esp
 xorl %eax, %eax
 leave
 retl
_inner_handler:
 movl $1, %eax              # ExceptionContinueSearch
 retl
_outer_handler:
 movl 12(%esp), %eax        # CONTEXT pointer
 addl $2, 184(%eax)         # i386 CONTEXT.Eip
 xorl %eax, %eax            # ExceptionContinueExecution
 retl
