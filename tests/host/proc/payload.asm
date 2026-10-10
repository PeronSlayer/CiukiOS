; Interim native ELF fixture, embedded by the canonical kernel build.
; Every public constant/record offset comes from the target abi.h extraction.
; Private result page: +4 done, +8 release, +12 errors, +16 entry ESP,
; +20 entry flags, +24 GS, +28 TLS, +32 argc, +36 gate for worker spins,
; +40 increments, +44 mutex, +48 completed threads, +64 thread IDs[8].
; +128 token state, +132 token value, +136 consumed token count.
; SPDX-License-Identifier: GPL-2.0-only
bits 32
%include "proc_abi.inc"
org CIUKI_IMAGE_BASE - CIUKI_PAGE_SIZE
%define DATA (CIUKI_IMAGE_BASE + CIUKI_PAGE_SIZE)
%define DONE (DATA + 4)
%define RELEASE (DATA + 8)
%define ERRORS (DATA + 12)
%define SCRATCH (DATA + 256)
%macro CALL 1
    mov eax, %1
    int 0x80
%endmacro
%macro EQ 2
    cmp %1, %2
    je %%ok
    inc dword [ERRORS]
%%ok:
%endmacro
    db 0x7f, 'ELF', 1, 1, 1, 0, 0
    times 7 db 0
    dw 2, 3
    dd 1, entry, phdr-$$, 0, 0
    dw 52, 32, 2, 0, 0, 0
phdr:
    dd 1, CIUKI_PAGE_SIZE, CIUKI_IMAGE_BASE, 0, code_end-entry, CIUKI_PAGE_SIZE, 5, CIUKI_PAGE_SIZE
    dd 1, 2*CIUKI_PAGE_SIZE, DATA, 0, 4, CIUKI_PAGE_SIZE+3, 6, CIUKI_PAGE_SIZE
    times CIUKI_PAGE_SIZE-($-$$) db 0
entry:
    pushad
    pushfd
    ; Record errors only after checking untouched BSS/final-page padding.
    xor edx, edx
    mov esi, DATA+4
    mov ecx, (2*CIUKI_PAGE_SIZE-4)/4
.zero:
    cmp dword [esi], 0
    je .zero_ok
    inc edx
.zero_ok:
    add esi, 4
    loop .zero
    mov [ERRORS], edx
    EQ dword [DATA], 0x71717171
    mov esi, esp
    mov ecx, 9
.regs:
    cmp ecx, 9
    je .flags
    cmp ecx, 5                   ; PUSHAD's saved ESP, intentionally nonzero
    je .skip
    EQ dword [esi], 0
    jmp .skip
.flags:
    EQ dword [esi], CIUKI_INITIAL_EFLAGS
    mov eax, [esi]
    mov [DATA+20], eax
.skip:
    add esi, 4
    loop .regs
    add esp, 36
    mov [DATA+16], esp
    xor eax, eax
    mov ax, gs
    mov [DATA+24], eax
    EQ eax, CIUKI_TLS_SELECTOR
    mov eax, [gs:ABI_OFFSETOF_CIUKI_TCB_SELF]
    mov [DATA+28], eax
    EQ [eax+ABI_OFFSETOF_CIUKI_TCB_SELF], eax
    mov eax, [esp]
    mov [DATA+32], eax
    EQ eax, 2
    mov esi, esp
    add esi, 16                  ; argc + 2 argv + argv NULL
    EQ dword [esp+12], 0
    cmp dword [esi], 0
    je .env_empty
    mov eax, [esi]
    EQ byte [eax], 'A'
    add esi, 4
.env_empty:
    EQ dword [esi], 0
    add esi, 4
    EQ dword [esi], AT_PAGESZ
    EQ dword [esi+4], CIUKI_PAGE_SIZE
    EQ dword [esi+8], AT_ENTRY
    EQ dword [esi+12], entry
    EQ dword [esi+16], AT_CIUKI_TLS
    mov eax, [DATA+28]
    EQ dword [esi+20], eax
    EQ dword [esi+24], AT_CIUKI_TLS_SIZE
    EQ dword [esi+28], CIUKI_TLS_SIZE
    EQ dword [esi+32], AT_CIUKI_ABI
    EQ dword [esi+36], CIUKI_ABI_VERSION
    EQ dword [esi+40], AT_NULL
    EQ dword [esi+44], 0
    fnstcw [SCRATCH]
    EQ word [SCRATCH], 0x037f
    fnstsw [SCRATCH]
    EQ word [SCRATCH], 0
    mov esi, [esp+8]
    cmp byte [esi], 'p'
    je park
    cmp byte [esi], 'm'
    je memory
    cmp byte [esi], 't'
    je threads
    cmp byte [esi], 'f'
    je fault
    cmp byte [esi], 'n'
    je mapping_fault
    cmp byte [esi], 'r'
    je mapping_fault
    cmp byte [esi], 's'
    je spawn_child
    jmp done
fault:
    mov dword [0], 1
    jmp bad_exit
mapping_fault:
    mov edi, esi
    mov ebx, mmap_args
    CALL CIUKI_SYS_MMAP
    cmp eax, -CIUKI_SYSCALL_ERROR_MAX
    jae bad_exit
    mov ebp, eax
    mov ebx, ebp
    mov ecx, CIUKI_PAGE_SIZE
    mov edx, PROT_NONE
    cmp byte [edi], 'n'
    je .protect
    mov edx, PROT_READ
.protect:
    CALL CIUKI_SYS_MPROTECT
    test eax, eax
    jnz bad_exit
    mov dword [ebp], 1
    jmp bad_exit
park:
    mov ebx, RELEASE
    xor ecx, ecx
    xor edx, edx
    mov esi, CLOCK_MONOTONIC
    CALL CIUKI_SYS_WAIT_WORD
    cmp dword [RELEASE], 0
    je park
    jmp done
memory:
    mov ebx, mmap_args
    CALL CIUKI_SYS_MMAP
    cmp eax, -CIUKI_SYSCALL_ERROR_MAX
    jae bad_exit
    mov ebp, eax
    EQ dword [ebp], 0
    mov dword [ebp], 0x12345678
    xor edi, edi
.protect:
    mov ebx, ebp
    mov ecx, CIUKI_PAGE_SIZE
    mov edx, [protections+edi*4]
    CALL CIUKI_SYS_MPROTECT
    EQ eax, 0
    inc edi
    cmp edi, 4
    jb .protect
    EQ dword [ebp], 0x12345678
    mov ebx, ebp
    mov ecx, CIUKI_PAGE_SIZE
    CALL CIUKI_SYS_MUNMAP
    EQ eax, 0
    xor ebx, ebx
    CALL CIUKI_SYS_BRK
    mov ebp, eax
    lea ebx, [ebp+CIUKI_PAGE_SIZE+1]
    CALL CIUKI_SYS_BRK
    EQ eax, ebx
    mov dword [ebp], 0x11223344
    mov byte [ebp+CIUKI_PAGE_SIZE], 0xcc
    lea ebx, [ebp+1]
    CALL CIUKI_SYS_BRK
    lea ebx, [ebp+CIUKI_PAGE_SIZE+1]
    CALL CIUKI_SYS_BRK
    EQ byte [ebp], 0x44
    EQ byte [ebp+1], 0
    EQ byte [ebp+CIUKI_PAGE_SIZE], 0
    jmp done
spawn_child:
    ; argv is copied from this process; child variant exits normally.
    mov ebx, spawn_args
    CALL CIUKI_SYS_SPAWN
    cmp eax, -CIUKI_SYSCALL_ERROR_MAX
    jae bad_exit
    mov edi, eax
    mov ebx, edi
    xor ecx, ecx
    mov edx, WNOHANG
    CALL CIUKI_SYS_WAITPID
    EQ eax, 0
    mov dword [DATA+52], 1
    ; The controller releases the child after inspecting private backing.
    mov ebx, edi
    mov ecx, SCRATCH
    xor edx, edx
    CALL CIUKI_SYS_WAITPID
    EQ eax, edi
    EQ dword [SCRATCH], 9472
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    CALL CIUKI_SYS_WAITPID
    EQ eax, -ECHILD
    jmp done
threads:
    ; Writable args copied once from the RO template.
    mov esi, thread_args
    mov edi, SCRATCH
    mov ecx, ABI_SIZEOF_CIUKI_THREAD_ARGS/4
    rep movsd
    xor edi, edi
.create:
    mov [SCRATCH+ABI_OFFSETOF_CIUKI_THREAD_ARGS_ARGUMENT], edi
    mov ebx, SCRATCH
    CALL CIUKI_SYS_THREAD_CREATE
    cmp eax, -CIUKI_SYSCALL_ERROR_MAX
    jae bad_exit
    mov [DATA+64+edi*4], eax
    inc edi
    cmp edi, 8
    jb .create
    xor edi, edi
.join:
    mov ebx, [DATA+64+edi*4]
    mov ecx, SCRATCH+64
    CALL CIUKI_SYS_THREAD_JOIN
    EQ eax, 0
    EQ dword [SCRATCH+64], edi
    inc edi
    cmp edi, 8
    jb .join
    EQ dword [DATA+40], 80000
    EQ dword [DATA+48], 8
    EQ dword [DATA+136], 10000
    ; Mismatch path and a matching expired deadline.
    mov ebx, DATA+44
    mov ecx, 1
    xor edx, edx
    mov esi, CLOCK_MONOTONIC
    CALL CIUKI_SYS_WAIT_WORD
    EQ eax, -EAGAIN
    mov ebx, DATA+44
    xor ecx, ecx
    mov edx, zero_deadline
    mov esi, CLOCK_MONOTONIC
    CALL CIUKI_SYS_WAIT_WORD
    EQ eax, -ETIMEDOUT
    jmp done
worker:
    push ebp
    mov ebp, esp
    sub esp, 16
    mov edi, [ebp+8]              ; argument: independent worker index
    mov esi, [gs:ABI_OFFSETOF_CIUKI_TCB_SELF]
    mov eax, edi
    and eax, 3
    shl eax, 10
    or eax, 0x037f
    mov [esp+4], ax
    fldcw [esp+4]
    lea eax, [edi+100]
    mov [gs:ABI_SIZEOF_CIUKI_TCB], eax
    lea eax, [edi+1]
    mov [esp], eax
    fild dword [esp]
    mov ebx, 10000
.work:
    xor eax, eax
    mov ecx, 1
    lock cmpxchg [DATA+44], ecx
    jne .work
    inc dword [DATA+40]
    mov dword [DATA+44], 0
    dec ebx
    jnz .work
    mov dword [esp+12], 0
    cmp edi, 0
    je .producer
    cmp edi, 1
    je .consumer
.wait_tokens:
    mov ecx, [DATA+136]
    cmp ecx, 10000
    je .tokens_done
    push esi
    mov ebx, DATA+136
    xor edx, edx
    mov esi, CLOCK_MONOTONIC
    CALL CIUKI_SYS_WAIT_WORD
    pop esi
    jmp .wait_tokens
.producer:
    cmp dword [DATA+128], 0
    je .produce
    mov ecx, 1
    call wait_token
    jmp .producer
.produce:
    mov eax, [esp+12]
    mov [DATA+132], eax
    mov dword [DATA+128], 1
    call wake_token
    inc dword [esp+12]
    cmp dword [esp+12], 10000
    jb .producer
    jmp .tokens_done
.consumer:
    cmp dword [DATA+128], 1
    je .consume
    xor ecx, ecx
    call wait_token
    jmp .consumer
.consume:
    mov eax, [esp+12]
    EQ dword [DATA+132], eax
    inc dword [DATA+136]
    mov dword [DATA+128], 0
    call wake_token
    inc dword [esp+12]
    cmp dword [esp+12], 10000
    jb .consumer
    mov ebx, DATA+136
    mov ecx, 0xffffffff
    CALL CIUKI_SYS_WAKE_WORD
.tokens_done:
    lock inc dword [DATA+48]
.spin:
    EQ dword [gs:ABI_OFFSETOF_CIUKI_TCB_SELF], esi
    fnstcw [esp+8]
    mov ax, [esp+4]
    EQ word [esp+8], ax
    lea eax, [edi+100]
    EQ dword [gs:ABI_SIZEOF_CIUKI_TCB], eax
    fist dword [esp]
    lea eax, [edi+1]
    EQ dword [esp], eax
    cmp dword [DATA+36], 0
    je .spin
    fstp st0
    mov eax, edi
    mov esp, ebp
    pop ebp
    ret
wait_token:
    push esi
    mov ebx, DATA+128
    xor edx, edx
    mov esi, CLOCK_MONOTONIC
    CALL CIUKI_SYS_WAIT_WORD
    pop esi
    test eax, eax
    jz .ok
    cmp eax, -EAGAIN
    je .ok
    inc dword [ERRORS]
.ok:
    ret
wake_token:
    mov ebx, DATA+128
    mov ecx, 1
    CALL CIUKI_SYS_WAKE_WORD
    cmp eax, -CIUKI_SYSCALL_ERROR_MAX
    jb .ok
    inc dword [ERRORS]
.ok:
    ret
thread_return:
    mov ebx, eax
    CALL CIUKI_SYS_THREAD_EXIT
bad_exit:
    mov ebx, 99
    CALL CIUKI_SYS_EXIT
    ud2
done:
    mov dword [DONE], 1
.wait:
    cmp dword [RELEASE], 0
    jne .exit
    mov ebx, RELEASE
    xor ecx, ecx
    xor edx, edx
    mov esi, CLOCK_MONOTONIC
    CALL CIUKI_SYS_WAIT_WORD
    jmp .wait
.exit:
    mov ebx, 37
    cmp dword [ERRORS], 0
    je .good
    mov ebx, 99
.good:
    CALL CIUKI_SYS_EXIT
    ud2
align 4
protections: dd PROT_NONE, PROT_READ, PROT_READ|PROT_EXEC, PROT_READ|PROT_WRITE
mmap_args: dd ABI_SIZEOF_CIUKI_MMAP_ARGS, 0, CIUKI_PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1
    dq 0
thread_args: dd ABI_SIZEOF_CIUKI_THREAD_ARGS, worker, 0, thread_return, CIUKI_THREAD_STACK_MIN, 0, 0, 0
zero_deadline: times ABI_SIZEOF_CIUKI_TIMESPEC db 0
spawn_args: dd ABI_SIZEOF_CIUKI_SPAWN_ARGS, image_path, child_argv, 0, 0, 0, 0, 0
image_path: db '/f2-fixture', 0
name: db 'fixture', 0
exit_mode: db 'e', 0
align 4
child_argv: dd name, exit_mode, 0
code_end:
%if code_end-entry > CIUKI_PAGE_SIZE
%error F2 code exceeds one page
%endif
    times 2*CIUKI_PAGE_SIZE-($-$$) db 0
    dd 0x71717171
