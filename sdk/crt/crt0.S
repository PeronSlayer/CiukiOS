.section .text.start,"ax",@progbits
.globl _start
.type _start,@function
_start:
    xorl %ebp,%ebp
    pushl %esp
    call __ciuki_start
    ud2
.size _start,.-_start
.section .note.GNU-stack,"",@progbits
