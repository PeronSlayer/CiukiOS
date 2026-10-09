; Keep the exercised T23 UMB inclusion off other physical firmware maps.
; Read-only PCI mechanism 1 probes, preserving its address latch and caller.
; AL=1 only for the exact T23 pair behind the observed AGP bridge.
vm_platform_is_t23:
    push ebx
    push dx
    mov dx,0CF8h
    in eax,dx
    mov ebx,eax
    mov eax,80010000h           ; observed AGP bus1/device0/function0
    out dx,eax
    mov dx,0CFCh
    in eax,dx
    cmp eax,8C2E5333h
    jne .other
    mov dx,0CF8h
    mov eax,8000FD00h           ; ICH3 bus0/device31/function5
    out dx,eax
    mov dx,0CFCh
    in eax,dx
    cmp eax,24858086h
    jne .other
    mov eax,1
    jmp .restore
.other:
    xor eax,eax
.restore:
    push eax
    mov eax,ebx
    mov dx,0CF8h
    out dx,eax
    pop eax
    pop dx
    pop ebx
    ret
