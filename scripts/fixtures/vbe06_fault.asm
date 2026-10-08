; Test-only VBE 4F06 BIOS fault injector for qemu_test_boot_hardware.py.
; With an empty command tail, install this INT 10h hook as a DOS TSR. For a
; successful Get Scan Line Length (AX=4F06h, BL=1), divide returned BX by 8
; and leave AX/CX/DX and every other output untouched. Run the same COM later
; with argument Q to query the resident hit count and last before/after BX.
bits 16
cpu 386
org 0x100

start:
    cmp byte [0x80],0
    jne parse_query

    mov ax,0x3510
    int 0x21
    mov [cs:old_int10],bx
    mov [cs:old_int10+2],es
    push cs
    pop ds
    mov dx,int10_hook
    mov ax,0x2510
    int 0x21
    mov dx,resident_end
    add dx,15                  ; DOS counts paragraphs from the PSP
    shr dx,4
    mov ax,0x3100
    int 0x21

parse_query:
    ; The DOS tail length is at PSP:80h and normally starts with a space at
    ; 81h. Accept leading whitespace and either case for the query switch.
    mov si,0x81
    mov cl,[0x80]
    xor ch,ch
.skip_space:
    jcxz exit_error
    mov al,[si]
    inc si
    dec cx
    cmp al,' '
    jbe .skip_space
    and al,0xdf
    cmp al,'Q'
    jne exit_error

query_resident:
    mov ax,0x5f06             ; private query service implemented by our hook
    int 0x10
    push cs
    pop ds
    mov [query_record+4],bx
    mov [query_record+6],cx
    mov [query_record+8],dx
    mov dx,query_path
    xor cx,cx
    mov ah,0x3c
    int 0x21
    jc exit_error
    mov bx,ax
    mov dx,query_record
    mov cx,query_record_end-query_record
    mov ah,0x40
    int 0x21
    pushf
    push ax
    mov ah,0x3e
    int 0x21
    pop ax
    popf
    jc exit_error
    cmp ax,query_record_end-query_record
    jne exit_error
    mov ax,0x4c00
    int 0x21
exit_error:
    mov ax,0x4cff
    int 0x21

int10_hook:
    cmp ax,0x5f06
    je .query
    cmp ax,0x4f06
    jne .chain
    cmp bl,1
    jne .chain
    pushf
    call far [cs:old_int10]
    cmp ax,0x004f
    jne .iret
    mov [cs:last_before],bx
    shr bx,3
    mov [cs:last_after],bx
    inc word [cs:hit_count]
.iret:
    iret
.query:
    mov bx,[cs:hit_count]
    mov cx,[cs:last_before]
    mov dx,[cs:last_after]
    iret
.chain:
    jmp far [cs:old_int10]

old_int10     dd 0
hit_count     dw 0
last_before   dw 0
last_after    dw 0
query_path    db '\SYSTEM\VIDEO\VBE06.LOG',0
query_record  db 'V06Q'
              dw 0,0,0
query_record_end:
resident_end:
