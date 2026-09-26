; The shipped SB/Windows profiles use A220 I7 D1. Query its DSP before
; loading VSBHDA: the emulator refuses an already present Sound Blaster and
; never reaches /RUN. Reading the DSP version does not reset a resident card,
; change its IRQ/DMA routing or start a DMA transfer. Every poll is bounded.
audio_native_probe:
    push ax
    push cx
    push dx
    mov dx, 0x22C
    mov cx, 0xFFFF
.write_ready:
    in al, dx
    cmp al, 0xFF
    je .fail
    test al, 0x80
    jz .request
    loop .write_ready
    jmp .fail
.request:
    mov al, 0xE1
    out dx, al
    call .read
    jc .fail
    cmp al, 1
    jb .fail
    cmp al, 4
    ja .fail
    call .read
    jc .fail
    clc
    jmp .done
.fail:
    stc
.done:
    pop dx
    pop cx
    pop ax
    ret
.read:
    mov dx, 0x22E
    mov cx, 0xFFFF
.wait:
    in al, dx
    test al, 0x80
    jnz .ready
    loop .wait
    stc
    ret
.ready:
    mov dx, 0x22A
    in al, dx
    clc
    ret

%ifdef NATIVE_PROGRAM
; DOS4GW needs the engine name before the original command line. Keep the
; user's arguments for direct COM and MZ launches, as on the emulated path.
build_native_tail:
    pushad
    push es
    push ds
    pop es
    cld
    mov si, native_prefix
    mov di, native_tail+1
    mov cx, native_prefix_end-native_prefix
    rep movsb
    mov bx, native_prefix_end-native_prefix
    mov si, NATIVE_USER_TAIL
    movzx cx, byte [si]
    inc si
.copy:
    jcxz .done
    cmp bx, 126
    jae .done
    lodsb
    cmp al, 13
    je .done
    stosb
    inc bx
    loop .copy
.done:
    mov [native_tail], bl
    mov al, 13
    stosb
    pop es
    popad
    ret
native_path db NATIVE_PROGRAM,0
native_prefix db NATIVE_PREFIX
native_prefix_end:
native_tail times 128 db 0
native_param_block dw 0,native_tail,0,native_fcb1,0,native_fcb2,0
; EXEC copies 16 bytes per default FCB: drive 0, blank 8.3 name, zero state.
native_fcb1 db 0, '           ', 0, 0, 0, 0
native_fcb2 db 0, '           ', 0, 0, 0, 0
msg_native db '[AUDIO] Native Sound Blaster - starting application',13,10,'$'
%endif
