; A plain absolute COM/EXE can use DOS EXEC directly. Keep the command
; interpreter for batch files, search, quoting, expansion and operators.
; Called only after command_program accepted the absolute pathname.
vm_direct_command:
    mov byte [vm_direct],0
    mov si,program
.end:
    cmp byte [si],0
    je .extension
    inc si
    jmp .end
.extension:
    cmp si,program+4
    jb .done
    cmp byte [si-4],'.'
    jne .done
    mov ax,[si-3]
    or ax,2020h
    mov dl,[si-1]
    or dl,20h
    cmp ax,'co'
    jne .exe
    cmp dl,'m'
    jne .done
    jmp .plain
.exe:
    cmp ax,'ex'
    jne .done
    cmp dl,'e'
    jne .done
.plain:
    mov si,child_tail+1
    movzx cx,byte [child_tail]
    jcxz .done
.syntax:
    lodsb
    cmp al,'<'
    je .done
    cmp al,'>'
    je .done
    cmp al,'|'
    je .done
    cmp al,'&'
    je .done
    cmp al,'"'
    je .done
    cmp al,'%'
    je .done
    cmp al,'^'
    je .done
    loop .syntax
    mov si,[vm_command_args]
    mov di,vm_direct_tail+1
    xor cx,cx
.arguments:
    lodsb
    cmp al,13
    je .ready
    cmp cx,125
    jae .done
    stosb
    inc cx
    jmp .arguments
.ready:
    mov byte [di],13
    mov [vm_direct_tail],cl
    mov byte [vm_direct],1
.done:
    ret

vm_direct db 0
vm_command_args dw 0
vm_direct_tail times 128 db 0
