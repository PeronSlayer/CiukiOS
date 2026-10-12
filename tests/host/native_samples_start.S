/* Linux host adapter entry, never shipped in a target sample. */
.text
.globl _start
_start:
    pushl %esp
    call samples_host_start
    ud2
.section .note.GNU-stack,"",@progbits
