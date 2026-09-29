; VMMIO.COM - VM manager gate: file I/O from two DOS VMs at once.
;
;   VMMIO        system VM: runs "VMMIO B" in a second VM through
;                \VM\VMFORK.COM, then "VMMIO A" itself, waits for the other
;                VM to end and prints "[VMMIO] PASS" when both wrote and read
;                back their files intact.
;   VMMIO x      writes \IOTEST.x: 40 records of 1000 bytes, one INT 21h
;                write per record with a timer tick between them (the other VM
;                runs meanwhile), then reads the file back and compares it.
;                Exit code 0 when it matches.
;
; Record i, byte j = (i*7 + j + x) & 0FFh. The QEMU harness checks the files
; and the FAT from the host as well. Output goes to COM1.
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"

RECORDS equ 40
RECORD_BYTES equ 1000

start:
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    mov si,81h
.blank:
    lodsb
    cmp al,' '
    je .blank
    cmp al,13
    je controller
    and al,0DFh
    mov [letter],al
    call worker
    mov ah,4Ch
    int 21h

controller:
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov [entry],di
    mov [entry+2],es
    push cs
    pop es
    mov ax,[entry]
    or ax,[entry+2]
    mov si,msg_nosession
    jz fail
    mov [params+4],cs
    mov [params+8],cs
    mov [params+12],cs
    mov dx,fork_path
    mov bx,params
    mov ax,4B00h
    int 21h
    push cs
    pop ds
    push cs
    pop es
    mov si,msg_fork
    jc fail
    mov ah,4Dh
    int 21h
    test al,al
    jnz fail
    mov byte [letter],'A'
    call worker
    mov [own_exit],al
    ; The other VM ends by itself (bounded wait, ~50 s).
    mov cx,900
.wait:
    push cx
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    mov [other_exit],di
    pop cx
    cmp bx,1
    je .ended
    call wait_tick
    loop .wait
    mov si,msg_timeout
    jmp fail
.ended:
    cmp byte [own_exit],0
    mov si,msg_own
    jne fail
    cmp word [other_exit],0
    mov si,msg_other
    jne fail
    mov si,msg_pass
    call serial_text
    mov ax,4C00h
    int 21h
fail:
    call serial_text
    mov si,msg_fail
    call serial_text
    mov ax,4C01h
    int 21h

; Worker for [letter]; AL = 0 when the file was written and read back intact.
worker:
    mov al,[letter]
    mov [file_name+8],al
    mov [tag+4],al
    mov ah,3Ch
    xor cx,cx
    mov dx,file_name
    int 21h
    jc .error
    mov [handle],ax
    xor bp,bp
.write:
    call fill
    mov ah,40h
    mov bx,[handle]
    mov cx,RECORD_BYTES
    mov dx,record
    int 21h
    jc .error_close
    cmp ax,RECORD_BYTES
    jne .error_close
    mov si,tag
    call serial_text
    mov ax,bp
    call serial_hex
    mov si,msg_crlf
    call serial_text
    call wait_tick
    inc bp
    cmp bp,RECORDS
    jb .write
    mov ah,3Eh
    mov bx,[handle]
    int 21h
    jc .error
    ; Read it back.
    mov ax,3D00h
    mov dx,file_name
    int 21h
    jc .error
    mov [handle],ax
    xor bp,bp
.read:
    mov ah,3Fh
    mov bx,[handle]
    mov cx,RECORD_BYTES
    mov dx,readback
    int 21h
    jc .error_close
    cmp ax,RECORD_BYTES
    jne .error_close
    call fill
    mov si,record
    mov di,readback
    mov cx,RECORD_BYTES
    repe cmpsb
    jne .error_close
    call wait_tick
    inc bp
    cmp bp,RECORDS
    jb .read
    mov ah,3Fh                          ; and nothing after the last record
    mov bx,[handle]
    mov cx,16
    mov dx,readback
    int 21h
    jc .error_close
    test ax,ax
    jnz .error_close
    mov ah,3Eh
    mov bx,[handle]
    int 21h
    mov si,tag
    call serial_text
    mov si,msg_ok
    call serial_text
    xor al,al
    ret
.error_close:
    mov ah,3Eh
    mov bx,[handle]
    int 21h
.error:
    mov si,tag
    call serial_text
    mov si,msg_bad
    call serial_text
    mov ax,bp
    call serial_hex
    mov si,msg_crlf
    call serial_text
    mov al,1
    ret

; record = pattern of record BP.
fill:
    push es
    push cs
    pop es
    mov di,record
    mov ax,bp
    imul ax,ax,7
    add al,[letter]
    mov cx,RECORD_BYTES
.b:
    stosb
    inc al
    loop .b
    pop es
    ret

; Busy wait (no HLT, no DOS) until the BIOS tick count moves on.
wait_tick:
    push es
    push ecx
    push eax
    push 40h
    pop es
    mov eax,[es:6Ch]
    mov ecx,400000000
.w:
    cmp eax,[es:6Ch]
    jne .d
    dec ecx
    jnz .w
.d:
    pop eax
    pop ecx
    pop es
    ret

serial_text:
    lodsb
    test al,al
    jz .done
    call serial_char
    jmp serial_text
.done:
    ret
serial_char:
    push dx
    push ax
    mov dx,3FDh
.w:
    in al,dx
    test al,20h
    jz .w
    pop ax
    mov dx,3F8h
    out dx,al
    pop dx
    ret
serial_hex:
    push cx
    mov cx,4
.digit:
    rol ax,4
    push ax
    and al,0Fh
    add al,'0'
    cmp al,'9'
    jbe .out
    add al,7
.out:
    call serial_char
    pop ax
    loop .digit
    pop cx
    ret

msg_nosession db '[VMMIO] no CVSESSION',13,10,0
msg_fork db '[VMMIO] VMFORK failed',13,10,0
msg_timeout db '[VMMIO] the other VM did not end',13,10,0
msg_own db '[VMMIO] VM 0 file check failed',13,10,0
msg_other db '[VMMIO] VM 1 file check failed',13,10,0
msg_pass db '[VMMIO] PASS',13,10,0
msg_fail db '[VMMIO] FAIL',13,10,0
msg_ok db 'OK',13,10,0
msg_bad db 'ERROR at record ',0
msg_crlf db 13,10,0
tag db '[IO x] ',0
file_name db '\IOTEST.x',0
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMMIO.COM B'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
letter db 0
own_exit db 0
other_exit dw 0
handle dw 0
record times RECORD_BYTES db 0
readback times RECORD_BYTES db 0
    align 2
    times 512 db 0
stack_top:
image_end:
