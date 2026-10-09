bits 16
org 0x100
; Separate foreground diagnostic module. The desktop passes stage0/1/2.
start:
    mov ax,cs
    mov ds,ax
    mov es,ax
    cli
    mov ss,ax
    mov sp,stack_top
    sti
    cld
    mov al,[0x82]
    sub al,'0'
    cmp al,2
    ja .exit
    call input_snapshot_save
.exit:
    mov ax,0x4C00
    int 0x21
input_snapshot_save:
    pushf
    pushad
    push ds
    push es
    push cs
    pop ds
    mov [input_snapshot_record+8],al
    add al,'0'
    mov [input_snapshot_path+13],al
    pushf
    cli
    in al,0x21
    mov [input_snapshot_record+10],al
    in al,0xA1
    mov [input_snapshot_record+11],al
    in al,0x64
    mov [input_snapshot_record+12],al
    mov ax,0x40
    mov es,ax
    mov al,[es:0x17]
    mov [input_snapshot_record+13],al
    mov eax,[es:0x1A]
    mov [input_snapshot_record+14],eax
    mov eax,[es:0x80]
    mov [input_snapshot_record+18],eax
    mov al,[es:0x18]
    mov [input_snapshot_record+22],al
    mov al,[es:0x96]
    mov [input_snapshot_record+23],al
    mov eax,[es:0x6C]
    mov [input_snapshot_record+44],eax
    xor ax,ax
    mov es,ax
    mov eax,[es:0x09*4]
    mov [input_snapshot_record+24],eax
    mov eax,[es:0x74*4]
    mov [input_snapshot_record+28],eax
    mov eax,[es:0x16*4]
    mov [input_snapshot_record+32],eax
    mov eax,[es:0x33*4]
    mov [input_snapshot_record+36],eax
    mov eax,[es:0x15*4]
    mov [input_snapshot_record+40],eax
    popf
    push cs
    pop es
    mov dx,input_snapshot_path
    xor cx,cx
    mov ah,0x3C
    int 0x21
    jc .done
    mov bx,ax
    mov dx,input_snapshot_record
    mov cx,64
    mov ah,0x40
    int 0x21
    mov ah,0x3E
    int 0x21
.done:
    pop es
    pop ds
    popad
    popf
    ret
input_snapshot_path db '\SYSTEM\INPUT0.BIN',0
input_snapshot_record db 'CIPS'
    dw 0x0100,64,0
    times 54 db 0

align 2
    times 256 db 0
stack_top:
