; The full DOS dispatcher owns its stack instead of borrowing a program's
; small MZ/COM stack. A slot remains live while EXEC runs a child; nested
; INT21 therefore uses a different slot. See dos-system-call-stack.md.
%define DOS_INTERRUPT_STACK_BYTES 512
%define DOS_INTERRUPT_STACK_COUNT (DOS_EXEC_STATE_FRAME_MAX + 1)

kernel_int21_enter:
    cli
    pushad
    push ds
    push es
    cld
    mov ax, cs
    mov ds, ax
    mov bx, [kernel_int21_stack_depth]
    cmp bx, DOS_INTERRUPT_STACK_COUNT
    jae .overflow
    inc word [kernel_int21_stack_depth]
    mov di, bx
    shl di, 2
    mov ax, ss
    mov [kernel_int21_stack_context + di], ax
    mov si, sp
    lea dx, [si + 36]              ; original caller interrupt frame
    mov [kernel_int21_stack_context + di + 2], dx

    ; DS:SI is the caller snapshot; ES:DI is the private complete frame.
    shl bx, 9
    mov di, kernel_int21_stacks_end - 42
    sub di, bx
    mov dx, di
    push cs
    pop es
    mov ds, ax
    mov cx, 21
    rep movsw
    mov ax, cs
    mov ss, ax
    mov sp, dx
    pop es
    pop ds
    popad
    jmp int21_dispatch

.overflow:
    mov bp, sp
    mov word [ss:bp + 32], 8       ; DOS insufficient memory/stack capacity
    or word [ss:bp + 40], 1
    pop es
    pop ds
    popad
    iret

kernel_int21_leave:
    ; The dispatcher already restored returned registers and patched its
    ; private IRET image (including any termination trampoline). Transfer
    ; both back atomically, then use the caller's actual stack for IRET.
    cli
    pushad
    push ds
    push es
    cld
    push cs
    pop ds
    dec word [kernel_int21_stack_depth]
    mov bx, [kernel_int21_stack_depth]
    shl bx, 2
    mov ax, [kernel_int21_stack_context + bx]
    mov dx, [kernel_int21_stack_context + bx + 2]
    mov es, ax
    mov di, dx
    sub di, 36
    mov si, sp
    mov cx, 21
    rep movsw
    mov ss, ax
    mov sp, dx
    sub sp, 36
    pop es
    pop ds
    popad
    iret

align 2
kernel_int21_stack_depth dw 0
kernel_int21_stack_context times DOS_INTERRUPT_STACK_COUNT * 2 dw 0
kernel_int21_stacks times DOS_INTERRUPT_STACK_COUNT * DOS_INTERRUPT_STACK_BYTES db 0
kernel_int21_stacks_end:
