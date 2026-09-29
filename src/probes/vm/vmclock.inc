; Shared by VMCTEST/VMCCHILD: BIOS timer ticks over 10 s of real time.
; The CMOS clock is physical (no VM owns it). Busy loop: never yields.
; Returns AX = BIOS tick count delta.
measure_ticks:
    call rtc_seconds
    mov bl,al
.edge:
    call rtc_seconds                    ; start on a second boundary
    cmp al,bl
    je .edge
    mov bl,al
    push es
    push 40h
    pop es
    mov eax,[es:6Ch]
    pop es
    mov [ticks0],eax
    mov cx,10
.second:
    call rtc_seconds
    cmp al,bl
    je .second
    mov bl,al
    loop .second
    push es
    push 40h
    pop es
    mov eax,[es:6Ch]
    pop es
    sub eax,[ticks0]
    ret

; AL = CMOS seconds (BCD), read outside the update cycle.
rtc_seconds:
.uip:
    mov al,0Ah
    out 70h,al
    in al,71h
    test al,80h
    jnz .uip
    xor al,al
    out 70h,al
    in al,71h
    ret

ticks0 dd 0
