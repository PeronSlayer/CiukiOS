bits 16
org 0x100
    mov dx, message
    mov ah, 9
    int 0x21
%ifdef FAR_RETURN
    retf
message db '[COMRET] FAR',13,10,'$'
%else
    ret
message db '[COMRET] NEAR',13,10,'$'
%endif
