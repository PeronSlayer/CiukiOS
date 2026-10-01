; netcfg.asm - persistent IPv4 configuration for CiukiOS/mTCP
;
;   NETCFG STATIC <ip> <mask> <gateway> <dns>
;   NETCFG DHCP
;   NETCFG APPLY

bits 16
org 0x0100

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, launcher_stack_top
    sti
    mov es, ax
    mov bx, ((launcher_image_end - $$ + 0x0100) + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc memory_error

    cld
    push cs
    pop ds
    push cs
    pop es
    call terminate_command_tail
    call next_token
    jc usage_error
    mov di, token_static
    call token_equal_ci
    jz static_command
    mov di, token_dhcp
    call token_equal_ci
    jz dhcp_command
    mov di, token_apply
    call token_equal_ci
    jz apply_command
    jmp usage_error

static_command:
    mov di, new_ip
    call parse_next_ipv4
    jc invalid_address
    mov di, new_mask
    call parse_next_ipv4
    jc invalid_address
    mov di, new_gateway
    call parse_next_ipv4
    jc invalid_address
    mov di, new_dns
    call parse_next_ipv4
    jc invalid_address
    call next_token
    jnc usage_error
    call validate_netmask
    jc invalid_mask
    call build_config
    call save_config_atomic
    jc save_error
    call update_resident_ip
    mov dx, msg_saved
    call print_dollar
    cmp byte [resident_updated], 0
    jne .active
    mov dx, msg_start_hint
    call print_dollar
    jmp success_exit
.active:
    mov dx, msg_resident_updated
    call print_dollar
    jmp success_exit

dhcp_command:
    call next_token
    jnc usage_error
    call require_resident
    jnc .service_ready
    ; The graphical Network panel invokes DHCP directly. Start the packet
    ; service on demand so a fitted adapter needs no prior DOS command.
    mov dx, netstart_path
    mov bx, empty_param_block
    call exec_child
    jc network_not_started
    call require_resident
    jc network_not_started
.service_ready:
    mov dx, dhcp_path
    mov bx, empty_param_block
    call exec_child
    jc dhcp_error
    mov dx, icmpd_path
    mov bx, empty_param_block
    call exec_child
    jc apply_error
    mov dx, msg_dhcp_done
    call print_dollar
    jmp success_exit

apply_command:
    call next_token
    jnc usage_error
    mov dx, icmpd_path
    mov bx, empty_param_block
    call exec_child
    jc apply_error
    mov dx, msg_apply_done
    call print_dollar
    jmp success_exit

memory_error:
    push cs
    pop ds
    mov dx, msg_memory_error
    call print_dollar
    mov ax, 0x4C08
    int 0x21

usage_error:
    mov dx, msg_usage
    jmp error_exit
invalid_address:
    mov dx, msg_invalid_address
    jmp error_exit
invalid_mask:
    mov dx, msg_invalid_mask
    jmp error_exit
save_error:
    mov dx, msg_save_error
    jmp error_exit
network_not_started:
    mov dx, msg_network_not_started
    jmp error_exit
dhcp_error:
    mov dx, msg_dhcp_error
    jmp error_exit
apply_error:
    mov dx, msg_apply_error
error_exit:
    call print_dollar
    mov ax, 0x4C01
    int 0x21
success_exit:
    mov ax, 0x4C00
    int 0x21

terminate_command_tail:
    xor ax, ax
    mov al, [0x0080]
    mov si, 0x0081
    add ax, si
    mov bx, ax
    mov byte [bx], 0
    mov word [tail_cursor], 0x0081
    ret

; Return DS:SI at the next ASCIIZ token, CF=1 if no token remains.  Spaces are
; replaced with NUL in the writable PSP command tail.
next_token:
    mov si, [tail_cursor]
.skip:
    mov al, [si]
    cmp al, ' '
    je .advance
    cmp al, 9
    jne .start
.advance:
    inc si
    jmp .skip
.start:
    test al, al
    jz .none
    mov bx, si
.scan:
    mov al, [bx]
    test al, al
    jz .at_end
    cmp al, ' '
    je .terminate
    cmp al, 9
    je .terminate
    inc bx
    jmp .scan
.terminate:
    mov byte [bx], 0
    inc bx
.at_end:
    mov [tail_cursor], bx
    clc
    ret
.none:
    stc
    ret

token_equal_ci:
    push si
.loop:
    mov al, [si]
    cmp al, 'a'
    jb .upper
    cmp al, 'z'
    ja .upper
    sub al, 'a' - 'A'
.upper:
    cmp al, [di]
    jne .different
    test al, al
    jz .same
    inc si
    inc di
    jmp .loop
.different:
    pop si
    mov al, 1
    or al, al
    ret
.same:
    pop si
    xor al, al
    or al, al
    ret

parse_next_ipv4:
    push di
    call next_token
    pop di
    jc .bad
    call parse_ipv4
    ret
.bad:
    stc
    ret

; DS:SI ASCIIZ dotted decimal -> ES:DI four bytes.
parse_ipv4:
    push ax
    push bx
    push cx
    push dx
    push bp
    mov cx, 4
.octet:
    xor ax, ax
    xor bp, bp
.digit:
    mov bl, [si]
    cmp bl, '0'
    jb .end_octet
    cmp bl, '9'
    ja .end_octet
    sub bl, '0'
    xor bh, bh
    push bx
    mov bx, 10
    xor dx, dx
    mul bx
    pop bx
    or dx, dx
    jnz .bad
    add ax, bx
    cmp ax, 255
    ja .bad
    inc si
    inc bp
    jmp .digit
.end_octet:
    or bp, bp
    jz .bad
    stosb
    dec cx
    jz .last
    cmp byte [si], '.'
    jne .bad
    inc si
    jmp .octet
.last:
    cmp byte [si], 0
    jne .bad
    pop bp
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret
.bad:
    pop bp
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

; Accept only contiguous IPv4 masks (including /0 and /32).
validate_netmask:
    push ax
    push cx
    push si
    mov si, new_mask
    mov cx, 4
    xor ah, ah                         ; AH=1 once the zero suffix begins
.byte:
    lodsb
    test ah, ah
    jnz .must_be_zero
    cmp al, 255
    je .next
    cmp al, 254
    je .suffix
    cmp al, 252
    je .suffix
    cmp al, 248
    je .suffix
    cmp al, 240
    je .suffix
    cmp al, 224
    je .suffix
    cmp al, 192
    je .suffix
    cmp al, 128
    je .suffix
    cmp al, 0
    jne .bad
.suffix:
    mov ah, 1
    jmp .next
.must_be_zero:
    test al, al
    jnz .bad
.next:
    loop .byte
    pop si
    pop cx
    pop ax
    clc
    ret
.bad:
    pop si
    pop cx
    pop ax
    stc
    ret

build_config:
    mov di, config_output
    mov si, config_prefix
    call append_z
    mov si, new_ip
    call append_ipv4
    mov si, config_mask
    call append_z
    mov si, new_mask
    call append_ipv4
    mov si, config_gateway
    call append_z
    mov si, new_gateway
    call append_ipv4
    mov si, config_dns
    call append_z
    mov si, new_dns
    call append_ipv4
    mov si, config_suffix
    call append_z
    mov ax, di
    sub ax, config_output
    mov [config_output_size], ax
    ret

append_z:
    lodsb
    test al, al
    jz .done
    stosb
    jmp append_z
.done:
    ret

append_ipv4:
    mov cx, 4
.octet:
    lodsb
    call append_decimal_al
    dec cx
    jz .done
    mov al, '.'
    stosb
    jmp .octet
.done:
    ret

append_decimal_al:
    push ax
    push bx
    push dx
    xor ah, ah
    mov bl, 100
    div bl                              ; AL hundreds, AH remainder
    mov dl, al
    mov al, ah
    xor ah, ah
    test dl, dl
    jz .tens
    add dl, '0'
    mov [es:di], dl
    inc di
.tens:
    mov bl, 10
    div bl                              ; AL tens, AH units
    mov bh, al
    test dl, dl                         ; hundreds already emitted?
    jnz .emit_tens
    test bh, bh
    jz .units
.emit_tens:
    add bh, '0'
    mov [es:di], bh
    inc di
.units:
    mov al, ah
    add al, '0'
    stosb
    pop dx
    pop bx
    pop ax
    ret

; Create NEW completely, rotate CFG to BAK, then atomically rename NEW to CFG.
save_config_atomic:
    mov dx, new_path
    xor cx, cx
    mov ax, 0x3C00
    int 0x21
    jc .fail
    mov bx, ax
    mov dx, config_output
    mov cx, [config_output_size]
    mov ah, 0x40
    int 0x21
    jc .close_fail
    cmp ax, [config_output_size]
    jne .close_fail
    mov ah, 0x3E
    int 0x21
    jc .delete_new_fail

    mov dx, backup_path
    mov ah, 0x41
    int 0x21                            ; absence is harmless
    push cs
    pop es
    mov dx, config_path
    mov di, backup_path
    mov ah, 0x56
    int 0x21
    jc .delete_new_fail
    push cs
    pop es
    mov dx, new_path
    mov di, config_path
    mov ah, 0x56
    int 0x21
    jc .restore_backup
    clc
    ret
.restore_backup:
    push cs
    pop es
    mov dx, backup_path
    mov di, config_path
    mov ah, 0x56
    int 0x21
    jmp .delete_new_fail
.close_fail:
    pushf
    mov ah, 0x3E
    int 0x21
    popf
.delete_new_fail:
    mov dx, new_path
    mov ah, 0x41
    int 0x21
.fail:
    stc
    ret

update_resident_ip:
    mov byte [resident_updated], 0
    call require_resident
    jc .done
    mov ax, 0xFE01
    mov si, new_ip
    int 0x61
    jc .done
    mov byte [resident_updated], 1
.done:
    ret

require_resident:
    mov ax, 0x3561
    int 0x21
    cmp word [es:bx + 3], 0x4B50
    jne .missing
    cmp word [es:bx + 11], 0x4943
    jne .missing
    clc
    ret
.missing:
    stc
    ret

exec_child:
    ; IN DS:DX path, DS:BX parameter block
    push ax
    push bx
    push dx
    push es
    mov si, bx
    mov ah, 0x62
    int 0x21
    mov word [si], 0
    mov word [si + 2], empty_tail
    mov [si + 4], cs
    mov word [si + 6], 0x005C
    mov [si + 8], bx
    mov word [si + 10], 0x006C
    mov [si + 12], bx
    mov bx, si
    push cs
    pop es
    mov ax, 0x4B00
    int 0x21
    jc .done
    mov ah, 0x4D
    int 0x21
    or al, al
    jnz .failed
    clc
    jmp .done
.failed:
    stc
.done:
    pop es
    pop dx
    pop bx
    pop ax
    ret

print_dollar:
    mov ah, 0x09
    int 0x21
    ret

token_static db 'STATIC', 0
token_dhcp db 'DHCP', 0
token_apply db 'APPLY', 0

msg_usage db 'Usage:', 13, 10
          db '  NETCFG STATIC <ip> <mask> <gateway> <dns>', 13, 10
          db '  NETCFG DHCP', 13, 10
          db '  NETCFG APPLY', 13, 10, '$'
msg_invalid_address db 'NETCFG: invalid IPv4 address (each octet must be 0..255)', 13, 10, '$'
msg_invalid_mask db 'NETCFG: invalid subnet mask (bits must be contiguous)', 13, 10, '$'
msg_save_error db 'NETCFG: cannot safely replace C:\NET\MTCP.CFG', 13, 10, '$'
msg_saved db 'NETCFG: static IPv4 configuration saved (backup: MTCP.BAK)', 13, 10, '$'
msg_start_hint db 'NETCFG: run NETSTART to activate networking and permanent ICMP', 13, 10, '$'
msg_resident_updated db 'NETCFG: resident ICMP address updated immediately', 13, 10, '$'
msg_network_not_started db 'NETCFG: network service could not start; check adapter and driver', 13, 10, '$'
msg_dhcp_error db 'NETCFG: DHCP failed', 13, 10, '$'
msg_dhcp_done db 'NETCFG: DHCP lease saved and resident ICMP address reloaded', 13, 10, '$'
msg_apply_error db 'NETCFG: cannot start/reload the resident network service', 13, 10, '$'
msg_apply_done db 'NETCFG: configuration applied to the resident network service', 13, 10, '$'
msg_memory_error db 'NETCFG: cannot release memory for child process', 13, 10, '$'

config_prefix db '# CiukiOS mTCP profile - managed by NETCFG', 13, 10
              db 'packetint 0x61', 13, 10
              db 'mtu 1500', 13, 10
              db 'hostname ciukios', 13, 10, 13, 10
              db 'ftpsrv_password_file c:\net\ftppass.txt', 13, 10
              db 'ftpsrv_log_file c:\net\ftpsrv.log', 13, 10
              db 'ftpsrv_session_timeout 300', 13, 10
              db 'ftpsrv_control_port 21', 13, 10
              db 'ftpsrv_pasv_base 2048', 13, 10
              db 'ftpsrv_pasv_ports 256', 13, 10
              db 'ftpsrv_clients 1', 13, 10
              db 'ftpsrv_filebuffer_size 16', 13, 10
              db 'ftpsrv_tcpbuffer_size 16', 13, 10
              db 'ftpsrv_packets_per_poll 2', 13, 10
              db 'ftpsrv_exclude_drives ABDEFGHIJKLMNOPQRSTUVWXYZ', 13, 10, 13, 10
              db 'HOSTNAME_ASSIGNED ciukios', 13, 10
              db 'IPADDR ', 0
config_mask db 13, 10, 'NETMASK ', 0
config_gateway db 13, 10, 'GATEWAY ', 0
config_dns db 13, 10, 'NAMESERVER ', 0
config_suffix db 13, 10, 'LEASE_TIME 86400', 13, 10, 0

config_path db 'C:\NET\MTCP.CFG', 0
new_path db 'C:\NET\MTCP.NEW', 0
backup_path db 'C:\NET\MTCP.BAK', 0
dhcp_path db 'C:\NET\DHCP.EXE', 0
netstart_path db 'C:\NET\NETSTART.COM', 0
icmpd_path db 'C:\NET\ICMPD.COM', 0
empty_tail db 0, 0x0D
empty_param_block times 14 db 0

tail_cursor dw 0
config_output_size dw 0
resident_updated db 0
new_ip times 4 db 0
new_mask times 4 db 0
new_gateway times 4 db 0
new_dns times 4 db 0
config_output times 1536 db 0

align 16
launcher_stack times 1024 db 0
launcher_stack_top:
launcher_image_end:
