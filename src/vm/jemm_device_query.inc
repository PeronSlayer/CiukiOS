; CiukiOS opt-in Jemm device-query adapter, 2026-09-26.
; Copyright (c) 2026 CiukiOS contributors.
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
;
; Not a DOS character-device implementation. Only failed read-only opens of
; EMMXXXX0/EMMQXXX0 (DOS AX=2) may use a genuine device-chain header. FFFEh is
; private to this executable; it is never passed to DOS for read-IOCTL/close.
; All other services retain the original INT21 behavior. The caller is a tiny
; Jemm/JLOAD process whose IOCTL transfer buffer uses DS=SS.

ciuki_device_int21 proc near
    pushf
    cmp ax,3D00h
    je cdq_open
    cmp bx,0FFFEh
    jne cdq_dos
    cmp ax,4402h
    je cdq_private
    cmp ah,3Eh
    je cdq_private
    cmp ax,4403h
    jne cdq_dos
    ; Write-IOCTL is deliberately unsupported, never submitted as a DOS handle.
    popf
    mov ax,1
    stc
    ret
cdq_dos:
    popf
    int 21h
    ret
cdq_open:
    popf
    call cdq_open_call
    ret
cdq_private:
    popf
    call cdq_private_call
    ret
ciuki_device_int21 endp

; Saved frame after PUSHAD, DS, ES, FS, GS, BP=SP:
; +0 GS, +2 FS, +4 ES, +6 DS, +8 EDI, +12 ESI, +16 EBP,
; +20 ESP, +24 EBX, +28 EDX, +32 ECX, +36 EAX.
cdq_open_call proc near
    pushad
    push ds
    push es
    push fs
    push gs
    mov bp,sp
    int 21h
    jnc cdq_open_ok
    cmp ax,2
    jne cdq_open_bad
    mov fs,word ptr ss:[bp+6]
    mov si,word ptr ss:[bp+28]
    cmp si,0FFF7h
    ja cdq_not_found
    movzx eax,si
    mov dx,fs
    movzx edx,dx
    shl edx,4
    add eax,edx
    cmp eax,500h
    jb cdq_not_found
    cmp eax,0A0000h-9
    ja cdq_not_found
    cmp byte ptr fs:[si+8],0
    jne cdq_not_found
    cmp dword ptr fs:[si+4],'0XXX'
    jne cdq_not_found
    mov eax,dword ptr fs:[si]
    cmp eax,'XMME'
    je cdq_name_ok
    cmp eax,'QMME'
    jne cdq_not_found
cdq_name_ok:
    cmp byte ptr cs:[cdq_active],0
    jne cdq_already_open
    mov cs:[cdq_name],eax
    mov ah,52h
    int 21h
    mov ax,es
    mov fs,ax
    mov si,bx
    add si,22h
    jc cdq_not_found
    mov cx,64
cdq_walk:
    cmp si,0FFFFh
    je cdq_not_found
    cmp si,0FFEEh
    ja cdq_not_found
    movzx eax,si
    mov dx,fs
    movzx edx,dx
    shl edx,4
    add eax,edx
    cmp eax,500h
    jb cdq_not_found
    cmp eax,0A0000h-18
    jbe cdq_header_ok
    ; Jemm's NOHI-free resident header may live in the configured UMB.
    ; Do not follow arbitrary ROM, VGA or other upper-memory pointers.
    cmp eax,0CD000h
    jb cdq_not_found
    cmp eax,0EC000h-18
    ja cdq_not_found
cdq_header_ok:
    mov ax,word ptr fs:[si+4]
    and ax,0C000h
    cmp ax,0C000h
    jne cdq_next
    mov eax,dword ptr fs:[si+10]
    cmp eax,cs:[cdq_name]
    jne cdq_next
    cmp dword ptr fs:[si+14],'0XXX'
    jne cdq_next
    mov ax,word ptr fs:[si+6]
    call cdq_check_entry
    jc cdq_not_found
    mov word ptr cs:[cdq_strategy],ax
    mov ax,fs
    mov word ptr cs:[cdq_strategy+2],ax
    mov ax,word ptr fs:[si+8]
    call cdq_check_entry
    jc cdq_not_found
    mov word ptr cs:[cdq_interrupt],ax
    mov ax,fs
    mov word ptr cs:[cdq_interrupt+2],ax
    mov byte ptr cs:[cdq_active],1
    mov ax,0FFFEh
cdq_open_ok:
    clc
    jmp cdq_open_done
cdq_next:
    lfs si,dword ptr fs:[si]
    dec cx
    jnz cdq_walk
cdq_not_found:
    mov ax,2
cdq_open_bad:
    stc
    jmp cdq_open_done
cdq_already_open:
    mov ax,4
    stc
cdq_open_done:
    mov word ptr ss:[bp+36],ax
    pop gs
    pop fs
    pop es
    pop ds
    popad
    ret
cdq_open_call endp

; Check a strategy/interrupt offset in FS without changing AX.
cdq_check_entry proc near
    push eax
    push edx
    movzx eax,ax
    mov dx,fs
    movzx edx,dx
    shl edx,4
    add eax,edx
    cmp eax,500h
    jb cdq_entry_bad
    cmp eax,0A0000h
    jb cdq_entry_ok
    cmp eax,0CD000h
    jb cdq_entry_bad
    cmp eax,0EC000h
    jae cdq_entry_bad
cdq_entry_ok:
    clc
    jmp cdq_entry_done
cdq_entry_bad:
    stc
cdq_entry_done:
    pop edx
    pop eax
    ret
cdq_check_entry endp

cdq_private_call proc near
    pushad
    push ds
    push es
    push fs
    push gs
    mov bp,sp
    cmp byte ptr cs:[cdq_active],1
    je cdq_private_active
    mov ax,6
    stc
    jmp cdq_private_done
cdq_private_active:
    cmp ah,3Eh
    je cdq_close
    cmp byte ptr cs:[cdq_busy],0
    jne cdq_io_error
    mov ax,ds
    mov bx,ss
    cmp ax,bx
    jne cdq_io_error
    or cx,cx
    jz cdq_io_error
    movzx eax,dx
    movzx ebx,cx
    add eax,ebx
    cmp eax,10000h
    ja cdq_io_error
    mov bx,ds
    movzx ebx,bx
    shl ebx,4
    add eax,ebx
    cmp eax,0A0000h
    ja cdq_io_error
    movzx eax,dx
    add eax,ebx
    cmp eax,500h
    jb cdq_io_error
    mov dword ptr cs:[cdq_request+0],00030014h
    mov dword ptr cs:[cdq_request+4],0
    mov dword ptr cs:[cdq_request+8],0
    mov word ptr cs:[cdq_request+12],0
    mov word ptr cs:[cdq_request+14],dx
    mov ax,ds
    mov word ptr cs:[cdq_request+16],ax
    mov word ptr cs:[cdq_request+18],cx
    mov byte ptr cs:[cdq_busy],1
    push cs
    pop es
    mov bx,offset cdq_request
    push ebp
    call dword ptr cs:[cdq_strategy]
    pop ebp
    push cs
    pop es
    mov bx,offset cdq_request
    push ebp
    call dword ptr cs:[cdq_interrupt]
    pop ebp
    mov byte ptr cs:[cdq_busy],0
    mov ax,word ptr cs:[cdq_request+3]
    and ax,8100h
    cmp ax,0100h
    jne cdq_io_error
    mov ax,word ptr cs:[cdq_request+18]
    cmp ax,word ptr ss:[bp+32]
    ja cdq_io_error
    clc
    jmp cdq_private_done
cdq_close:
    mov byte ptr cs:[cdq_active],0
    xor ax,ax
    jmp cdq_private_done
cdq_io_error:
    mov ax,13
    stc
cdq_private_done:
    mov word ptr ss:[bp+36],ax
    pop gs
    pop fs
    pop es
    pop ds
    popad
    ret
cdq_private_call endp

cdq_active db 0
cdq_busy db 0
cdq_name dd 0
cdq_strategy dd 0
cdq_interrupt dd 0
cdq_request db 20 dup (0)
