; NATRESUM.COM: actual DOS transport for two persistent CPL3 processes.
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"

OP_START equ 0Eh
OP_STATE equ 0Fh
OP_STOP equ 10h
COOKIE_A equ 41414943h
COOKIE_B equ 42424943h

start:
 cli
 mov ax,cs
 mov ss,ax
 mov sp,stack_top
 sti
 push cs
 pop ds
 push cs
 pop es
 cld
 mov byte [stage],1
 mov bx,(image_end-$$+100h+15)/16
 mov ah,4Ah
 int 21h
 jc fail
 mov byte [stage],2
 mov dx,path_a
 mov di,buffer_a
 call read_image
 jc fail
 mov [length_a],ax
 mov byte [stage],3
 mov dx,path_b
 mov di,buffer_b
 call read_image
 jc fail
 mov [length_b],ax
 mov byte [stage],4
 mov dx,path_loop
 mov di,buffer_loop
 call read_image
 jc fail
 mov [length_loop],ax
 mov byte [stage],5
 xor di,di
 mov es,di
 mov ax,1684h
 mov bx,VM_DEVICE_ID
 int 2Fh
 mov [vm_entry],di
 mov [vm_entry+2],es
 mov ax,di
 mov dx,es
 or ax,dx
 jz fail
 ; Native quanta use the VM manager's InDOS/FAT gates: give it the kernel
 ; layout exactly as VMFORK does (DOSMGR table segment/InDOS, first MCB).
 mov byte [stage],18
 mov ax,1607h
 mov bx,15h
 xor cx,cx
 int 2Fh
 mov ax,es
 mov si,[es:bx+6]
 push ax
 mov ah,52h
 int 21h
 mov dx,[es:bx-2]
 pop bx
 push cs
 pop es
 movzx ebx,bx
 movzx ecx,si
 movzx edx,dx
 mov ax,VM_OP_VMM_INIT
 call far [vm_entry]
 push cs
 pop ds
 jc fail
 push cs
 pop es

 ; Both DOS buffers are read before either process starts.
 mov byte [stage],6
 mov si,buffer_a
 movzx ecx,word [length_a]
 mov ax,OP_START
 call far [vm_entry]
 push cs
 pop ds
 jc fail
 test eax,eax
 jnz fail
 mov [handle_a],ebx
 mov byte [stage],7
 mov si,buffer_b
 movzx ecx,word [length_b]
 mov ax,OP_START
 call far [vm_entry]
 push cs
 pop ds
 jc fail
 test eax,eax
 jnz fail
 cmp ebx,[handle_a]
 je fail
 mov [handle_b],ebx
 mov byte [stage],8
 mov ebx,[handle_a]
 call state
 jc fail
 cmp dword [q_state],1
 jne fail
 mov byte [stage],9
 mov ebx,[handle_b]
 call state
 jc fail
 cmp dword [q_state],1
 jne fail
 mov si,msg_live
 call print
 mov word [ticks_left],180
.wait:
 mov byte [stage],10
 call wait_tick
 mov byte [stage],11
 mov ebx,[handle_a]
 call state
 jc fail
 cmp dword [q_state],3
 je .a_ended
 cmp dword [q_state],1
 jne fail
 jmp .b_state
.a_ended:
 mov eax,COOKIE_A
 call check_end
 jc fail
 mov byte [done_a],1
.b_state:
 mov byte [stage],12
 mov ebx,[handle_b]
 call state
 jc fail
 cmp dword [q_state],3
 je .b_ended
 cmp dword [q_state],1
 jne fail
 jmp .check_both
.b_ended:
 mov eax,COOKIE_B
 call check_end
 jc fail
 mov byte [done_b],1
.check_both:
 cmp byte [done_a],1
 jne .continue
 cmp byte [done_b],1
 je .complete
.continue:
 dec word [ticks_left]
 jnz .wait
 jmp fail
.complete:
 mov si,msg_completed
 call print

 ; A live infinite process must yield and remain independently stoppable.
 mov byte [stage],13
 mov si,buffer_loop
 movzx ecx,word [length_loop]
 mov ax,OP_START
 call far [vm_entry]
 push cs
 pop ds
 jc fail
 test eax,eax
 jnz fail
 mov [handle_loop],ebx
 mov word [ticks_left],120
.wait_loop:
 mov byte [stage],14
 call wait_tick
 mov ebx,[handle_loop]
 call state
 jc fail
 cmp dword [q_state],1
 jne fail
 cmp dword [q_steps],256
 jae .stop_loop
 dec word [ticks_left]
 jnz .wait_loop
 jmp fail
.stop_loop:
 mov byte [stage],15
 mov ebx,[handle_loop]
 mov ax,OP_STOP
 call far [vm_entry]
 push cs
 pop ds
 jc fail
 test eax,eax
 jnz fail
 mov byte [stage],16
 mov ebx,[handle_loop]
 call state
 jc fail
 cmp dword [q_state],3
 jne fail
 cmp dword [q_result],5
 jne fail
 mov byte [stage],17
 ; A changed generation must never resolve to this slot's process.
 mov ebx,[handle_loop]
 xor ebx,100h
 call state
 jnc fail
 mov si,msg_stopped
 call print
 mov si,msg_pass
 call print
 mov ax,4C00h
 int 21h

check_end:
 cmp dword [q_result],0
 jne .bad
 cmp dword [q_status],0
 jne .bad
 cmp dword [q_reports],2
 jne .bad
 cmp dword [q_tag],2
 jne .bad
 cmp [q_value],eax
 jne .bad
 cmp dword [q_steps],1200
 jbe .bad
 clc
 ret
.bad:
 stc
 ret

state:
 mov ax,OP_STATE
 call far [cs:vm_entry]
 pushf
 mov [cs:q_state],eax
 mov [cs:q_tag],ebx
 mov [cs:q_steps],ecx
 mov [cs:q_result],edx
 mov [cs:q_reports],esi
 mov [cs:q_status],edi
 mov [cs:q_value],ebp
 push cs
 pop ds
 popf
 ret

wait_tick:
 push es
 xor ax,ax
 mov es,ax
 mov eax,[es:46Ch]
.wait:
 sti
 hlt
 cmp eax,[es:46Ch]
 je .wait
 pop es
 ret

; A small, complete fixture fits the fixed buffer; host verifies its CRC.
read_image:
 push di
 mov ax,3D00h
 int 21h
 jc .open_failed
 mov bx,ax
 pop dx
 mov cx,2048
 mov ah,3Fh
 int 21h
 pushf
 push ax
 mov ah,3Eh
 int 21h
 pop ax
 popf
 jc .bad
 cmp ax,36
 jbe .bad
 cmp ax,2048
 jae .bad
 clc
 ret
.open_failed:
 pop di
.bad:
 stc
 ret

print:
 lodsb
 test al,al
 jz .done
 push si
 mov dl,al
 mov ah,02h
 int 21h
 pop si
 jmp print
.done:
 ret
fail:
 mov [cs:failure_eax],eax
 push cs
 pop ds
 mov si,msg_fail
 call print
 mov si,stage
 call hex32
 mov si,msg_ax
 call print
 mov si,failure_eax
 call hex32
 mov si,msg_state
 call print
 mov si,q_state
 call hex32
 mov si,msg_result
 call print
 mov si,q_result
 call hex32
 mov si,msg_reports
 call print
 mov si,q_reports
 call hex32
 mov si,msg_steps
 call print
 mov si,q_steps
 call hex32
 mov si,msg_newline
 call print
 mov ax,4C01h
 int 21h

hex32:
 push ax
 push bx
 push cx
 push si
 add si,3
 mov cx,4
.byte:
 mov bl,[si]
 mov al,bl
 shr al,4
 call .nibble
 mov al,bl
 and al,0Fh
 call .nibble
 dec si
 loop .byte
 pop si
 pop cx
 pop bx
 pop ax
 ret
.nibble:
 cmp al,10
 jb .digit
 add al,'A'-10
 jmp .emit
.digit:
 add al,'0'
.emit:
 push bx
 push cx
 push si
 mov dl,al
 mov ah,02h
 int 21h
 pop si
 pop cx
 pop bx
 ret

path_a db '\VM\NRESUMA.N32',0
path_b db '\VM\NRESUMB.N32',0
path_loop db '\VM\NLOOP.N32',0
msg_live db '[NATIVE-RESUME] TWO LIVE HANDLES',13,10,0
msg_completed db '[NATIVE-RESUME] TWO COMPLETE reports=2 private cookies steps>1200',13,10,0
msg_stopped db '[NATIVE-RESUME] STOP AND GENERATION GUARD PASS',13,10,0
msg_pass db '[NATIVE-RESUME] PASS',13,10,0
msg_fail db '[NATIVE-RESUME] FAIL stage=',0
msg_ax db ' ax=',0
msg_state db ' state=',0
msg_result db ' result=',0
msg_reports db ' reports=',0
msg_steps db ' steps=',0
msg_newline db 13,10,0
stage dd 0
failure_eax dd 0
vm_entry dd 0
handle_a dd 0
handle_b dd 0
handle_loop dd 0
length_a dw 0
length_b dw 0
length_loop dw 0
ticks_left dw 0
done_a db 0
done_b db 0
q_state dd 0
q_tag dd 0
q_steps dd 0
q_result dd 0
q_reports dd 0
q_status dd 0
q_value dd 0
buffer_a times 2048 db 0
buffer_b times 2048 db 0
buffer_loop times 2048 db 0
stack_space times 1024 db 0
stack_top:
image_end:
