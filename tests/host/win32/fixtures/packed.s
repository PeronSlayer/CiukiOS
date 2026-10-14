# Self-modifying code: change immediate byte then jump to patched instruction.
# SPDX-License-Identifier: MIT
.section .text,"xw"
.globl _entry
_entry:
 movb $42, _patched+1
 jmp _patched
_patched:
 movl $0, %eax
 retl
