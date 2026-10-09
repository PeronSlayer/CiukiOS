/* Native Linux x86-64 test entry with a low-address test stack: wire pointers
 * are u32 even when production pthread logic is exercised on this host. */
.bss
.balign 16
host_stack: .space 65536
host_stack_top:
.text
.globl _start
_start:
    leaq host_stack_top(%rip),%rsp
    call ciuki_pthread_host_tests
    movl %eax,%edi
    movl $60,%eax
    syscall
.section .note.GNU-stack,"",@progbits
