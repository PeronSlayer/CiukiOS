; Install-only binding of public RTSV 0Dh and the typed CLFN v1 table.
; Input ES = candidate kernel segment returned by INT21/AH34. The DOS
; pointer is only a discovery candidate: all identity and extents are checked.
; Output AX=0/CF=0 bound, AX=1/CF=1 rejected; all other registers preserved.
; Required far pointers are published only after validating the whole table.
%include "src/runtime/ciukidos_abi.inc"
%include "src/runtime/ciukidos_lfn_abi.inc"

lfn_bind_runtime:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    mov ax, es
    mov [cs:lfn_candidate_segment], ax
    mov ds, ax
    cmp dword [2], 0x4B554943       ; CIUK
    jne .bad
    cmp dword [6], 0x534F4449       ; IDOS
    jne .bad
    cmp word [12], CIUKIDOS_ABI_VERSION
    jne .bad
    mov bp, [20]                   ; actual image extent
    cmp bp, CIUKIDOS_KERNEL_MAX_BYTES
    ja .bad
    cmp bp, CIUKIDOS_HEADER_SIZE
    jb .bad
    mov [cs:lfn_candidate_bytes], bp
    mov ax, [10]
    cmp ax, CIUKIDOS_HEADER_SIZE
    jb .bad
    cmp ax, bp
    ja .bad
    mov si, [22]                   ; declared RTSV position (not fixed)
    cmp si, ax
    jb .bad
    mov di, si
    add di, 10
    jc .bad
    cmp di, bp
    ja .bad
    cmp dword [si], 0x56535452      ; RTSV
    jne .bad
    cmp word [si+4], CIUKIDOS_ABI_VERSION
    jne .bad
    cmp word [16], CIUKIDOS_DESCRIPTOR_SIZE
    jne .bad
    cmp word [si+8], CIUKIDOS_DESCRIPTOR_SIZE
    jne .bad
    mov cx, [si+6]
    cmp cx, [14]
    jne .bad
    cmp cx, CIUKIDOS_SERVICE_LFN_API
    jb .bad
    mov ax, cx
    cmp ax, 0x1FFF
    ja .bad
    shl ax, 3
    add ax, di
    jc .bad
    cmp ax, bp
    ja .bad
    mov [cs:lfn_candidate_rtsv_end], ax
    mov si, di
.find_api:
    cmp word [si], CIUKIDOS_SERVICE_LFN_API
    je .api
    add si, CIUKIDOS_DESCRIPTOR_SIZE
    loop .find_api
    jmp .bad
.api:
    cmp word [si+2], 1
    jne .bad
    cmp word [si+6], 0
    jne .bad
    mov ax, [si+4]
    cmp ax, [cs:lfn_candidate_rtsv_end]
    jb .bad
    cmp ax, bp
    jae .bad
    mov [cs:lfn_candidate_getter], ax
    mov ax, ds
    mov [cs:lfn_candidate_getter+2], ax
    call far [cs:lfn_candidate_getter]
    jc .bad
    cmp ax, CLFN_API_VERSION
    jne .bad
    mov ax, ds
    cmp ax, [cs:lfn_candidate_segment]
    jne .bad
    mov bp, [cs:lfn_candidate_bytes]
    cmp cx, CLFN_TABLE_SIZE
    jb .bad
    cmp si, [cs:lfn_candidate_rtsv_end]
    jb .bad
    mov di, si
    add di, cx
    jc .bad
    cmp di, bp
    ja .bad
    mov [cs:lfn_candidate_clfn_start], si
    mov [cs:lfn_candidate_clfn_end], di
    cmp dword [si], 0x4E464C43      ; CLFN
    jne .bad
    cmp word [si+4], CLFN_API_VERSION
    jne .bad
    cmp [si+6], cx
    jne .bad
    cmp word [si+10], CLFN_ENTRY_SIZE
    jne .bad
    cmp word [si+12], 1
    jne .bad
    cmp word [si+14], 0
    jne .bad
    mov ax, [si+8]
    cmp ax, CLFN_ENTRY_COUNT
    jb .bad
    cmp ax, 0x1FFD
    ja .bad
    shl ax, 3
    add ax, CLFN_HEADER_SIZE
    cmp ax, cx
    jne .bad
    mov cx, [si+8]
    mov bx, si
    add bx, CLFN_HEADER_SIZE
    xor dx, dx
.entry:
    cmp word [bx], 0
    je .bad
    cmp word [bx+2], 1
    jne .bad
    mov ax, [bx+6]
    cmp ax, [cs:lfn_candidate_segment]
    jne .bad
    mov ax, [bx+4]
    cmp ax, [cs:lfn_candidate_rtsv_end]
    jb .bad
    cmp ax, bp
    jae .bad
    cmp ax, [cs:lfn_candidate_clfn_start]
    jb .outside_table
    cmp ax, [cs:lfn_candidate_clfn_end]
    jb .bad
.outside_table:
    mov di, [bx]
    dec di
    cmp di, CLFN_ENTRY_COUNT-1
    ja .next
    bts dx, di
    jc .bad
    shl di, 2
    mov [cs:lfn_candidate_entries+di], ax
    mov ax, [bx+6]
    mov [cs:lfn_candidate_entries+di+2], ax
.next:
    add bx, CLFN_ENTRY_SIZE
    loop .entry
    cmp dx, (1 << CLFN_ENTRY_COUNT)-1
    jne .bad
    push cs
    pop ds
    push cs
    pop es
    mov si, lfn_candidate_entries
    mov di, lfn_entries
    mov cx, CLFN_ENTRY_COUNT*2
    cld
    rep movsw
    xor ax, ax
    clc
    jmp .done
.bad:
    mov di, lfn_entries
    mov cx, CLFN_ENTRY_COUNT
.clear:
    mov dword [cs:di], 0
    add di, 4
    loop .clear
    mov ax, 1
    stc
.done:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

align 2
lfn_candidate_segment dw 0
lfn_candidate_bytes dw 0
lfn_candidate_rtsv_end dw 0
lfn_candidate_clfn_start dw 0
lfn_candidate_clfn_end dw 0
lfn_candidate_getter dd 0
lfn_candidate_entries times CLFN_ENTRY_COUNT dd 0
