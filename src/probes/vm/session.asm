; Execute the actual DOS and BIOS interfaces while a V86 monitor is installed.
; This is an application probe, not a monitor or a source-port graphics bridge.
bits 16
org 100h

start:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    mov bx,(program_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    jc failure
    smsw ax
    test al,1
    jz not_monitored
    mov ax,0DE00h
    int 67h
    test ah,ah
    jnz failure
    mov dx,monitor_message
    call print

    ; Ordinary direct-video writes must execute, without a cooperative API.
    mov ax,0B800h
    mov es,ax
    mov word [es:160*23],1F56h
    cmp word [es:160*23],1F56h
    jne failure
    push ds
    pop es

    mov dx,filename
    xor cx,cx
    mov ah,3Ch
    int 21h
    jc failure
    mov bx,ax
    mov dx,payload
    mov cx,payload_end-payload
    mov ah,40h
    int 21h
    jc close_failure
    cmp ax,payload_end-payload
    jne close_failure
    mov ah,3Eh
    int 21h
    jc failure
    mov dx,filename
    mov ax,3D00h
    int 21h
    jc failure
    mov bx,ax
    mov dx,buffer
    mov cx,payload_end-payload
    mov ah,3Fh
    int 21h
    jc close_failure
    cmp ax,payload_end-payload
    jne close_failure
    mov ah,3Eh
    int 21h
    jc failure
    push ds
    pop es
    mov si,payload
    mov di,buffer
    mov cx,payload_end-payload
    cld
    repe cmpsb
    jne failure
    mov dx,filename
    mov ah,41h
    int 21h
    jc failure
    mov dx,pass_message
    call print
    mov ax,4C00h
    int 21h

close_failure:
    mov ah,3Eh
    int 21h
failure:
    push cs
    pop ds
    mov dx,fail_message
    call print
    mov ax,4C01h
    int 21h
not_monitored:
    mov dx,absent_message
    call print
    mov ax,4C02h
    int 21h
print:
    mov ah,9
    int 21h
    ret

monitor_message db '[VMSESSION] PE=1 VCPI=present',13,10,'$'
pass_message db '[VMSESSION] DOS create/write/read/close/delete and B800 PASS',13,10,'$'
fail_message db '[VMSESSION] FAIL',13,10,'$'
absent_message db '[VMSESSION] No V86 monitor',13,10,'$'
filename db '\VMTEST.DAT',0
payload db 'CiukiOS monitored DOS file round-trip',13,10
payload_end:
buffer times payload_end-payload db 0
align 16
stack_space times 2048 db 0
stack_top:
program_end:
