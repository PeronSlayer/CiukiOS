/* Native Linux test entry; never used in the target SDK. */
.text
.globl _start
_start:
    call ciuki_pthread_host_tests
    movl %eax,%ebx
    movl $1,%eax
    int $0x80
.section .note.GNU-stack,"",@progbits
