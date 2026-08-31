; ipconfig.asm - display the active mTCP IPv4 configuration
;
; Reads the file named by MTCPCFG instead of embedding the QEMU defaults, so
; values written by DHCP are shown immediately and accurately.

bits 16
org 0x0100

start:
    cld
    push cs
    pop ds

    call load_config_path

    mov dx, config_path
    mov ax, 0x3D00
    int 0x21
    jc open_failed
    mov [file_handle], ax

    mov bx, ax
    mov dx, file_buffer
    mov cx, FILE_BUFFER_SIZE - 1
    mov ah, 0x3F
    int 0x21
    jc read_failed
    mov [file_size], ax
    mov si, file_buffer
    add si, ax
    mov byte [si], 0

    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21

    mov dx, msg_header
    call print_dollar
    mov dx, msg_config
    call print_dollar
    mov si, config_path
    call print_z
    call print_crlf

    mov dx, label_hostname
    mov di, key_hostname_assigned
    call show_field
    jnc .hostname_done
    mov dx, label_hostname
    mov di, key_hostname
    call show_field
.hostname_done:
    mov dx, label_ipaddr
    mov di, key_ipaddr
    call show_field_or_missing
    mov dx, label_netmask
    mov di, key_netmask
    call show_field_or_missing
    mov dx, label_gateway
    mov di, key_gateway
    call show_field_or_missing
    mov dx, label_nameserver
    mov di, key_nameserver
    call show_field_or_missing
    mov dx, label_packetint
    mov di, key_packetint
    call show_field_or_missing
    mov dx, label_lease
    mov di, key_lease
    call show_field_or_missing

    call show_icmp_status
    mov ax, 0x4C00
    int 0x21

open_failed:
    mov dx, msg_open_error
    call print_dollar
    mov si, config_path
    call print_z
    call print_crlf
    mov ax, 0x4C01
    int 0x21

read_failed:
    push ax
    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21
    pop ax
    mov dx, msg_read_error
    call print_dollar
    mov ax, 0x4C01
    int 0x21

; Start with the canonical path, then replace it with MTCPCFG from the inherited
; DOS environment when present. ES is allowed to remain on the environment.
load_config_path:
    mov si, default_config_path
    mov di, config_path
    call copy_z

    mov ax, [0x002C]
    test ax, ax
    jz .done
    mov es, ax
    xor di, di
.next_entry:
    cmp byte [es:di], 0
    je .done
.compare:
    mov bx, di
    mov si, env_name
.compare_char:
    lodsb
    test al, al
    jz .found
    cmp al, [es:bx]
    jne .skip_entry
    inc bx
    jmp .compare_char
.skip_entry:
    cmp byte [es:di], 0
    je .advance
    inc di
    jmp .skip_entry
.advance:
    inc di
    jmp .next_entry
.found:
    mov di, config_path
    mov cx, CONFIG_PATH_SIZE - 1
.copy_value:
    mov al, [es:bx]
    test al, al
    jz .terminate
    mov [di], al
    inc bx
    inc di
    loop .copy_value
.terminate:
    mov byte [di], 0
.done:
    ret

; DX = label, DI = key. Returns CF set if the key was not found.
show_field:
    push dx
    call find_key
    jc .missing
    pop dx
    push si
    push cx
    call print_dollar
    pop cx
    pop si
    call print_span
    call print_crlf
    clc
    ret
.missing:
    pop dx
    stc
    ret

show_field_or_missing:
    push dx
    call find_key
    jc .missing
    pop dx
    push si
    push cx
    call print_dollar
    pop cx
    pop si
    call print_span
    call print_crlf
    ret
.missing:
    pop dx
    call print_dollar
    mov dx, msg_not_set
    call print_dollar
    ret

; DI = uppercase key. Returns DS:SI and CX for the trimmed value.
find_key:
    push ax
    push bx
    push dx
    push bp
    mov bp, di
    mov si, file_buffer
.next_line:
    mov al, [si]
    test al, al
    jz .not_found
    cmp al, 0x0D
    je .advance_line_start
    cmp al, 0x0A
    je .advance_line_start
.skip_leading:
    cmp byte [si], ' '
    je .advance_leading
    cmp byte [si], 0x09
    jne .line_ready
.advance_leading:
    inc si
    jmp .skip_leading
.line_ready:
    cmp byte [si], '#'
    je .skip_line
    cmp byte [si], 0
    je .not_found
    mov di, bp
.compare_key:
    mov bl, [di]
    test bl, bl
    jz .key_complete
    mov al, [si]
    cmp al, 'a'
    jb .upper_ready
    cmp al, 'z'
    ja .upper_ready
    sub al, 'a' - 'A'
.upper_ready:
    cmp al, bl
    jne .skip_line
    inc si
    inc di
    jmp .compare_key
.key_complete:
    cmp byte [si], ' '
    je .skip_value_space
    cmp byte [si], 0x09
    jne .skip_line
.skip_value_space:
    inc si
    cmp byte [si], ' '
    je .skip_value_space
    cmp byte [si], 0x09
    je .skip_value_space
    mov dx, si
.find_value_end:
    mov al, [si]
    test al, al
    jz .value_end
    cmp al, 0x0D
    je .value_end
    cmp al, 0x0A
    je .value_end
    inc si
    jmp .find_value_end
.value_end:
    mov bx, si
.trim_value:
    cmp bx, dx
    je .not_found
    cmp byte [bx - 1], ' '
    je .trim_one
    cmp byte [bx - 1], 0x09
    jne .value_ready
.trim_one:
    dec bx
    jmp .trim_value
.value_ready:
    mov si, dx
    mov cx, bx
    sub cx, dx
    pop bp
    pop dx
    pop bx
    pop ax
    clc
    ret
.advance_line_start:
    inc si
    jmp .next_line
.skip_line:
    mov al, [si]
    test al, al
    jz .not_found
    inc si
    cmp al, 0x0A
    jne .skip_line
    jmp .next_line
.not_found:
    xor cx, cx
    pop bp
    pop dx
    pop bx
    pop ax
    stc
    ret

copy_z:
    lodsb
    stosb
    test al, al
    jnz copy_z
    ret

print_dollar:
    mov ah, 0x09
    int 0x21
    ret

print_z:
    lodsb
    test al, al
    jz .done
    mov dl, al
    mov ah, 0x02
    int 0x21
    jmp print_z
.done:
    ret

print_span:
    jcxz .done
.next:
    lodsb
    mov dl, al
    mov ah, 0x02
    int 0x21
    loop .next
.done:
    ret

print_crlf:
    mov dx, msg_crlf
    jmp print_dollar

show_icmp_status:
    mov ax, 0x3561
    int 0x21
    cmp word [es:bx + 3], 0x4B50       ; "PK"
    jne .inactive
    cmp word [es:bx + 11], 0x4943      ; "CI" (CIUKICMP signature)
    jne .inactive
    mov dx, msg_icmp_active
    jmp print_dollar
.inactive:
    mov dx, msg_icmp_inactive
    jmp print_dollar

msg_header db 'CiukiOS IPv4 configuration', 0x0D, 0x0A
           db '---------------------------', 0x0D, 0x0A, '$'
msg_config db '  Config file  : ', '$'
label_hostname db '  Host name    : ', '$'
label_ipaddr db '  IPv4 address : ', '$'
label_netmask db '  Subnet mask  : ', '$'
label_gateway db '  Gateway      : ', '$'
label_nameserver db '  DNS server   : ', '$'
label_packetint db '  Packet API   : INT ', '$'
label_lease db '  Lease seconds: ', '$'
msg_icmp_active db '  ICMP receive : ACTIVE (resident, independent of FTPSRV)', 0x0D, 0x0A, '$'
msg_icmp_inactive db '  ICMP receive : INACTIVE (run NETSTART)', 0x0D, 0x0A, '$'
msg_not_set db '<not set>', 0x0D, 0x0A, '$'
msg_open_error db 'IPCONFIG: cannot open ', '$'
msg_read_error db 'IPCONFIG: cannot read MTCPCFG', 0x0D, 0x0A, '$'
msg_crlf db 0x0D, 0x0A, '$'

env_name db 'MTCPCFG=', 0
default_config_path db 'C:\NET\MTCP.CFG', 0
key_hostname_assigned db 'HOSTNAME_ASSIGNED', 0
key_hostname db 'HOSTNAME', 0
key_ipaddr db 'IPADDR', 0
key_netmask db 'NETMASK', 0
key_gateway db 'GATEWAY', 0
key_nameserver db 'NAMESERVER', 0
key_packetint db 'PACKETINT', 0
key_lease db 'LEASE_TIME', 0

file_handle dw 0
file_size dw 0

CONFIG_PATH_SIZE equ 128
config_path times CONFIG_PATH_SIZE db 0
FILE_BUFFER_SIZE equ 4096
file_buffer times FILE_BUFFER_SIZE db 0
