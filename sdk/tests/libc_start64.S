/* Host reference only: retain Linux argv, run on a low-address stack so
 * modeled wire pointers stay u32. Never part of the installed runtime. */
.bss
.balign 16
libc_host_stack: .space 65536
libc_host_stack_top:
.text
.globl _start
_start:
    movq %rsp,%rdi
    leaq libc_host_stack_top(%rip),%rsp
    call ciuki_libc_host_tests
    ud2
.section .note.GNU-stack,"",@progbits
