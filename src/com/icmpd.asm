; icmpd.asm - resident ARP/ICMP service and Packet Driver bridge for CiukiOS
;
; INT 60h remains the physical Crynwr NE2000 Packet Driver.  This TSR owns its
; wildcard receive handle and exposes a compatible Packet Driver at INT 61h.
; mTCP applications use INT 61h, while this resident layer answers ARP and
; ICMP Echo Requests even when no mTCP application (including FTPSRV) is open.

bits 16
org 0x0100

PHYSICAL_INT equ 0x60
VIRTUAL_INT  equ 0x61
RX_BUFFER_SIZE equ 1600
RX_SLOT_BYTES equ RX_BUFFER_SIZE + 2
NATIVE_SLOTS equ 4
NATIVE_TOKEN equ 0xC161

start:
    jmp installer

; ---------------------------------------------------------------------------
; Resident Packet Driver entry point.  The specification requires the
; "PKT DRVR" eye-catcher immediately after three executable bytes.
; ---------------------------------------------------------------------------
virtual_entry:
    jmp near virtual_dispatch
    db 'PKT DRVR'
    db 'CIUKICMP'

virtual_dispatch:
    push bp
    mov bp, sp
    cmp ah, 0x04                 ; mTCP transmit through the bridge
    je .count_send
    cmp ah, 0x02
    je .access_type
    cmp ah, 0x03
    je .release_type
    cmp ah, 0x05
    je .cannot_terminate
    cmp ah, 0xFE
    je .private_api

    ; Driver information, send, address, reset and receive-mode operations
    ; are valid on the physical handle as well, so preserve the caller's
    ; interrupt frame and let the Crynwr driver complete the request.
    pop bp
    jmp far [cs:physical_vector]
.count_send:
    inc word [cs:tx_packets]
    pop bp
    jmp far [cs:physical_vector]

.access_type:
    cmp al, 0x01                 ; Ethernet class
    jne .no_class
    cmp byte [cs:client_active], 0
    jne .type_in_use
    cmp byte [cs:native_claimed], 0
    jne .type_in_use
    mov [cs:client_receiver_off], di
    mov ax, es
    mov [cs:client_receiver_seg], ax
    mov byte [cs:client_active], 1
    mov ax, [cs:physical_handle]
    jmp .success

.release_type:
    cmp bx, [cs:physical_handle]
    jne .bad_handle
    mov byte [cs:client_active], 0
    mov word [cs:client_receiver_off], 0
    mov word [cs:client_receiver_seg], 0
    jmp .success

.cannot_terminate:
    mov dh, 7                    ; CANT_TERMINATE
    jmp .failure

.private_api:
    cmp al, 0x00                 ; installed/status query
    je .private_status
    cmp al, 0x01                 ; update IPv4 from DS:SI (four bytes)
    je .private_set_ip
    cmp al, 0x02                 ; live RX/TX packet counters
    je .private_counters
    cmp al, 0x03                 ; claim exclusive native client RX/TX
    je .native_claim
    cmp al, 0x04                 ; release native client claim
    je .native_release
    cmp al, 0x05                 ; native queue status + address snapshot
    je .native_status
    cmp al, 0x06                 ; transmit bounded Ethernet frame DS:SI/CX
    je .native_send
    cmp al, 0x07                 ; receive next frame ES:DI/CX capacity
    je .native_poll
    mov dh, 11                   ; BAD_COMMAND
    jmp .failure
.private_status:
    mov bx, 0x4943               ; "CI"
    mov cx, 0x0100               ; API v1.0
    push cs
    pop es
    mov di, configured_ip
    jmp .success
.private_set_ip:
    push ax
    push cx
    push di
    push es
    push cs
    pop es
    mov di, configured_ip
    mov cx, 4
    cld
    rep movsb
    pop es
    pop di
    pop cx
    pop ax
    jmp .success
.private_counters:
    mov bx, [cs:rx_packets]
    mov cx, [cs:tx_packets]
    jmp .success
.native_claim:
    cmp byte [cs:client_active], 0
    jne .type_in_use
    cmp byte [cs:native_claimed], 0
    jne .type_in_use
    mov byte [cs:native_claimed], 1
    mov byte [cs:native_head], 0
    mov byte [cs:native_tail], 0
    mov byte [cs:native_count], 0
    mov word [cs:native_dropped], 0
    mov bx, NATIVE_TOKEN
    jmp .success
.native_release:
    cmp bx, NATIVE_TOKEN
    jne .bad_handle
    mov byte [cs:native_claimed], 0
    mov byte [cs:native_count], 0
    mov byte [cs:native_head], 0
    mov byte [cs:native_tail], 0
    jmp .success
.native_status:
    cld
    cmp byte [cs:native_claimed], 1
    jne .bad_handle
    mov ax, di
    add ax, 18
    jc .bad_buffer
    mov bx, NATIVE_TOKEN
    mov cl, [cs:native_count]
    xor ch, ch
    mov dx, [cs:native_dropped]
    push ds
    push es
    push si
    push di
    push cx
    push cs
    pop ds
    push cs
    pop es
    mov si, configured_ip
    mov di, native_info + 6
    mov cx, 4
    rep movsb
    mov si, local_mac
    mov di, native_info + 10
    mov cx, 6
    rep movsb
    mov ax, [native_dropped]
    mov [native_info + 16], ax
    pop cx
    pop di
    pop si
    pop es
    pop ds
    push ds
    push si
    push cx
    push cs
    pop ds
    mov si, native_info
    mov cx, 18
    rep movsb
    pop cx
    pop si
    pop ds
    jmp .success
.native_send:
    cmp bx, NATIVE_TOKEN
    jne .bad_handle
    cmp byte [cs:native_claimed], 1
    jne .bad_handle
    cmp cx, 14
    jb .bad_buffer
    cmp cx, RX_BUFFER_SIZE
    ja .bad_buffer
    mov ax, si
    add ax, cx
    jc .bad_buffer
    ; Snapshot caller data before entering the physical driver. The private
    ; TX buffer also prevents a caller from mutating a frame during send.
    push ds
    push si
    push cx
    push es
    push di
    push cs
    pop es
    mov di, native_tx_buffer
    cld
    rep movsb
    pop di
    pop es
    pop cx
    pop si
    pop ds
    push ds
    push es
    push si
    push di
    push cs
    pop ds
    mov si, native_tx_buffer
    mov ah, 0x04
    inc word [cs:tx_packets]
    push bp
    pushf
    cli
    call far [cs:physical_vector]
    pop bp
    pop di
    pop si
    pop es
    pop ds
    jc .failure
    jmp .success
.native_poll:
    cld
    cmp bx, NATIVE_TOKEN
    jne .bad_handle
    cmp byte [cs:native_claimed], 1
    jne .bad_handle
    cmp cx, RX_BUFFER_SIZE
    ja .bad_buffer
    mov ax, di
    add ax, cx
    jc .bad_buffer
    mov [cs:native_capacity], cx
    push ds
    push es
    push si
    push di
    push cx
    push bp
    push cs
    pop ds
    cli
    cmp byte [native_count], 0
    je .poll_empty
    xor bx, bx
    mov bl, [native_head]
    mov ax, bx
    mov dx, RX_SLOT_BYTES
    mul dx
    mov si, native_queue
    add si, ax
    mov ax, [si]                   ; length stored before slot payload
    cmp ax, [cs:native_capacity]
    ja .poll_too_small
    mov [native_last_length], ax
    mov cx, ax
    add si, 2
    rep movsb
    inc byte [cs:native_head]
    cmp byte [cs:native_head], NATIVE_SLOTS
    jb .poll_decrement
    mov byte [cs:native_head], 0
.poll_decrement:
    dec byte [cs:native_count]
    jmp .poll_restore
.poll_too_small:
    ; Leave the packet queued so the caller can retry with a larger buffer.
    pop bp
    pop cx
    pop di
    pop si
    pop es
    pop ds
    jmp .bad_buffer
.poll_restore:
    pop bp
    pop cx
    pop di
    pop si
    pop es
    pop ds
    mov cx, [cs:native_last_length]
    jmp .success
.poll_empty:
    pop bp
    pop cx
    pop di
    pop si
    pop es
    pop ds
    xor cx, cx
    jmp .success
.bad_buffer:
    mov dh, 9                    ; NO_SPACE / invalid bounded buffer
    jmp .failure

.no_class:
    mov dh, 2                    ; NO_CLASS
    jmp .failure
.type_in_use:
    mov dh, 10                   ; TYPE_INUSE
    jmp .failure
.bad_handle:
    mov dh, 1                    ; BAD_HANDLE
.failure:
    or word [ss:bp + 6], 1       ; CF in caller's saved FLAGS
    pop bp
    iret
.success:
    and word [ss:bp + 6], 0xFFFE
    pop bp
    iret

; ---------------------------------------------------------------------------
; Receive callback registered with the physical driver.  The physical driver
; calls twice: AX=0 asks for a buffer; AX!=0 reports that copying completed.
; ---------------------------------------------------------------------------
physical_receiver:
    or ax, ax
    jnz .complete
    cmp cx, RX_BUFFER_SIZE
    ja .drop
    push cs
    pop es
    mov di, rx_buffer
    retf
.drop:
    xor di, di
    mov es, di
    retf

.complete:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    cld
    push cs
    pop ds
    push cs
    pop es
    cmp cx, RX_BUFFER_SIZE
    ja .done
    inc word [rx_packets]
    mov [rx_length], cx
    call classify_service_packet
    or al, al
    jz .forward
    cmp byte [service_pending], 0
    je .queue_service
    ; Prefer Echo over a not-yet-sent ARP reply.  This also handles peers that
    ; transmit ARP and ICMP back-to-back inside one 18.2 Hz timer interval.
    cmp al, 2
    jne .done
    cmp byte [service_pending], 1
    jne .done
.queue_service:
    mov [service_pending], al
    mov si, rx_buffer
    mov di, service_buffer
    mov cx, [rx_length]
    rep movsb
    jmp .done                       ; suppress duplicate mTCP ARP/ICMP reply

.forward:
    cmp byte [client_active], 0
    jne .forward_mtcp
    cmp byte [native_claimed], 1
    jne .done
    cmp byte [native_count], NATIVE_SLOTS
    jae .native_drop
    xor bx, bx
    mov bl, [native_tail]
    mov ax, bx
    mov dx, RX_SLOT_BYTES
    mul dx
    mov di, native_queue
    add di, ax
    mov ax, [rx_length]
    mov [di], ax
    add di, 2
    mov si, rx_buffer
    mov cx, ax
    rep movsb
    inc byte [native_tail]
    cmp byte [native_tail], NATIVE_SLOTS
    jb .native_count_up
    mov byte [native_tail], 0
.native_count_up:
    inc byte [native_count]
    jmp .done
.native_drop:
    inc word [native_dropped]
    jmp .done
.forward_mtcp:

    ; Ask the mTCP receiver for one of its buffers.
    xor ax, ax
    mov bx, [physical_handle]
    mov cx, [rx_length]
    call far [client_receiver_off]
    mov [cs:client_dest_off], di
    mov ax, es
    mov [cs:client_dest_seg], ax
    or ax, di
    jz .done

    ; Copy the frame into that buffer and deliver the completion callback.
    push cs
    pop ds
    mov si, rx_buffer
    mov ax, [client_dest_seg]
    mov es, ax
    mov di, [client_dest_off]
    mov cx, [rx_length]
    rep movsb
    mov ax, [client_dest_seg]
    mov ds, ax
    mov si, [cs:client_dest_off]
    mov es, ax
    mov di, si
    mov ax, 1
    mov bx, [cs:physical_handle]
    mov cx, [cs:rx_length]
    call far [cs:client_receiver_off]

.done:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    retf

; Return AL=1 for an ARP request for us, AL=2 for an IPv4 ICMP Echo Request
; for us, or AL=0 when the packet must be passed to the active mTCP client.
classify_service_packet:
    xor ax, ax
    cmp word [rx_length], 42
    jb .done
    cmp word [rx_buffer + 12], 0x0608       ; EtherType ARP (08 06)
    je .arp
    cmp word [rx_buffer + 12], 0x0008       ; EtherType IPv4 (08 00)
    jne .done
    cmp byte [rx_buffer + 23], 1            ; IP protocol ICMP
    jne .done
    mov ax, [rx_buffer + 30]
    cmp ax, [configured_ip]
    jne .not_for_us
    mov ax, [rx_buffer + 32]
    cmp ax, [configured_ip + 2]
    jne .not_for_us
    mov al, [rx_buffer + 14]
    and al, 0x0F
    cmp al, 5
    jb .not_for_us
    xor ah, ah
    shl ax, 1
    shl ax, 1
    mov si, rx_buffer + 14
    add si, ax
    cmp si, rx_buffer + RX_BUFFER_SIZE - 2
    ja .not_for_us
    cmp byte [si], 8                       ; Echo Request
    jne .not_for_us
    cmp byte [si + 1], 0
    jne .not_for_us
    mov al, 2
    ret
.arp:
    cmp word [rx_buffer + 14], 0x0100       ; hardware Ethernet
    jne .not_for_us
    cmp word [rx_buffer + 16], 0x0008       ; protocol IPv4
    jne .not_for_us
    cmp word [rx_buffer + 20], 0x0100       ; opcode request
    jne .not_for_us
    mov ax, [rx_buffer + 38]
    cmp ax, [configured_ip]
    jne .not_for_us
    mov ax, [rx_buffer + 40]
    cmp ax, [configured_ip + 2]
    jne .not_for_us
    mov al, 1
    ret
.not_for_us:
    xor ax, ax
.done:
    ret

; BIOS invokes INT 1Ch after each timer tick.  Processing queued frames here
; keeps DOS and BIOS calls out of the NIC receive callback.
timer_handler:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    cld
    push cs
    pop ds
    push cs
    pop es
    mov al, [service_pending]
    test al, al
    jz .chain
    mov byte [service_pending], 0
    cmp al, 1
    je .arp
    call make_icmp_reply
    jmp .send
.arp:
    call make_arp_reply
.send:
    mov si, service_buffer
    mov cx, [service_send_length]
    mov ah, 0x04
    inc word [tx_packets]
    pushf
    cli
    call far [physical_vector]
.chain:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    cmp word [cs:old_timer_vector + 2], 0
    je .return
    pushf
    call far [cs:old_timer_vector]
.return:
    iret

make_arp_reply:
    ; Ethernet destination is the request sender; source is our NIC.
    mov si, service_buffer + 6
    mov di, service_buffer
    mov cx, 6
    rep movsb
    mov si, local_mac
    mov di, service_buffer + 6
    mov cx, 6
    rep movsb

    mov word [service_buffer + 20], 0x0200  ; ARP reply
    mov si, service_buffer + 22             ; original sender MAC
    mov di, service_buffer + 32             ; reply target MAC
    mov cx, 6
    rep movsb
    mov si, service_buffer + 28             ; original sender IP
    mov di, service_buffer + 38             ; reply target IP
    mov cx, 4
    rep movsb
    mov si, local_mac
    mov di, service_buffer + 22
    mov cx, 6
    rep movsb
    mov ax, [configured_ip]
    mov [service_buffer + 28], ax
    mov ax, [configured_ip + 2]
    mov [service_buffer + 30], ax
    mov ax, [rx_length]
    cmp ax, 60
    jae .length_ready
    mov ax, 60
.length_ready:
    mov [service_send_length], ax
    ret

make_icmp_reply:
    ; Ethernet header.
    mov si, service_buffer + 6
    mov di, service_buffer
    mov cx, 6
    rep movsb
    mov si, local_mac
    mov di, service_buffer + 6
    mov cx, 6
    rep movsb

    ; Preserve the request source IP as the reply destination.
    mov ax, [service_buffer + 26]
    mov [reply_peer_ip], ax
    mov ax, [service_buffer + 28]
    mov [reply_peer_ip + 2], ax
    mov ax, [configured_ip]
    mov [service_buffer + 26], ax
    mov ax, [configured_ip + 2]
    mov [service_buffer + 28], ax
    mov ax, [reply_peer_ip]
    mov [service_buffer + 30], ax
    mov ax, [reply_peer_ip + 2]
    mov [service_buffer + 32], ax
    mov byte [service_buffer + 22], 64      ; TTL

    mov al, [service_buffer + 14]
    and al, 0x0F
    xor ah, ah
    shl ax, 1
    shl ax, 1
    mov bp, ax                              ; IP header length
    mov si, service_buffer + 14
    add si, bp
    mov di, si                              ; checksum routine advances SI
    mov byte [si], 0                        ; Echo Reply
    mov word [si + 2], 0

    mov ax, [service_buffer + 16]           ; network total length
    xchg al, ah
    sub ax, bp
    mov cx, ax
    call internet_checksum
    mov [di + 2], ax

    mov word [service_buffer + 24], 0
    mov si, service_buffer + 14
    mov cx, bp
    call internet_checksum
    mov [service_buffer + 24], ax

    mov ax, [rx_length]
    cmp ax, 60
    jae .length_ready
    mov ax, 60
.length_ready:
    mov [service_send_length], ax
    ret

; DS:SI, CX -> RFC 1071 checksum.  AX is byte-swapped for direct storage in a
; network-order packet held in little-endian memory.
internet_checksum:
    push bx
    push dx
    xor dx, dx
.words:
    cmp cx, 2
    jb .odd
    lodsw
    xchg al, ah
    add dx, ax
    adc dx, 0
    sub cx, 2
    jmp .words
.odd:
    jcxz .finish
    lodsb
    mov ah, al
    xor al, al
    add dx, ax
    adc dx, 0
.finish:
    mov ax, dx
    not ax
    xchg al, ah
    pop dx
    pop bx
    ret

physical_vector       dw 0, 0
old_timer_vector      dw 0, 0
old_virtual_vector    dw 0, 0
physical_handle       dw 0
client_receiver_off   dw 0
client_receiver_seg   dw 0
client_dest_off       dw 0
client_dest_seg       dw 0
client_active         db 0
native_claimed        db 0
native_head           db 0
native_tail           db 0
native_count          db 0
native_dropped        dw 0
native_last_length    dw 0
native_capacity       dw 0
native_info           db 'C','V','N','T',1,4
                      times 12 db 0
native_tx_buffer      times RX_BUFFER_SIZE db 0
native_queue          times NATIVE_SLOTS * RX_SLOT_BYTES db 0
service_pending       db 0
rx_packets            dw 0
tx_packets            dw 0
configured_ip         db 10, 0, 2, 15
local_mac             times 6 db 0
rx_length             dw 0
service_send_length   dw 0
reply_peer_ip         times 4 db 0
rx_buffer             times RX_BUFFER_SIZE db 0
service_buffer        times RX_BUFFER_SIZE db 0

resident_end:

; ---------------------------------------------------------------------------
; Transient installer / reload command.
; ---------------------------------------------------------------------------
installer:
    cld
    ; DOS gives a COM process the complete free arena.  Move the transient
    ; installer stack inside our image and release the tail before EXECing the
    ; physical Packet Driver; otherwise a conforming nested EXEC has no free
    ; interval to allocate from.
    cli
    mov ax, cs
    mov ss, ax
    mov sp, installer_stack_top
    sti
    push cs
    pop ds
    push cs
    pop es
    mov bx, program_end
    add bx, 15
    mov cl, 4
    shr bx, cl
    mov ah, 0x4A
    int 0x21
    jc driver_load_error

    call find_installed_service
    jc .not_installed
    call load_config_ip
    jc config_error
    mov ax, 0xFE01
    mov si, loaded_ip
    int VIRTUAL_INT
    mov dx, msg_reloaded
    call print_dollar
    mov ax, 0x4C00
    int 0x21

.not_installed:
    call load_config_ip
    jc config_error
    mov si, loaded_ip
    mov di, configured_ip
    mov cx, 4
    rep movsb

    call find_physical_driver
    jnc .physical_ready
    call exec_ne2000
    jc driver_load_error
    call find_physical_driver
    jc driver_missing_error
.physical_ready:
    mov [physical_vector], bx
    mov ax, es
    mov [physical_vector + 2], ax

    ; Register for every Ethernet frame on the physical Packet Driver.
    push ds
    xor ax, ax
    mov ds, ax
    xor si, si
    mov ax, 0x0201
    mov bx, 0xFFFF
    xor dx, dx
    xor cx, cx
    push cs
    pop es
    mov di, physical_receiver
    pushf
    cli
    call far [cs:physical_vector]
    pop ds
    jc access_error
    mov [physical_handle], ax

    ; Cache the NIC address used in autonomous replies.
    mov bx, ax
    mov ah, 0x06
    mov cx, 6
    push cs
    pop es
    mov di, local_mac
    pushf
    cli
    call far [physical_vector]
    jc address_error
    cmp cx, 6
    jne address_error

    mov ax, 0x3561
    int 0x21
    mov [old_virtual_vector], bx
    mov ax, es
    mov [old_virtual_vector + 2], ax
    push cs
    pop ds
    mov dx, virtual_entry
    mov ax, 0x2561
    int 0x21

    mov ax, 0x351C
    int 0x21
    mov [old_timer_vector], bx
    mov ax, es
    mov [old_timer_vector + 2], ax
    push cs
    pop ds
    mov dx, timer_handler
    mov ax, 0x251C
    int 0x21

    mov dx, msg_installed
    call print_dollar
    mov ax, 0x3100
    mov dx, resident_end
    add dx, 15
    mov cl, 4
    shr dx, cl
    int 0x21

config_error:
    mov dx, msg_config_error
    jmp fatal_message
driver_load_error:
    push ax
    mov dx, msg_driver_load_error
    call print_dollar
    mov dx, msg_exec_ax
    call print_dollar
    pop ax
    call print_hex16
    mov dx, msg_crlf
    call print_dollar
    mov ax, 0x4C01
    int 0x21
driver_missing_error:
    mov dx, msg_driver_missing
    jmp fatal_message
access_error:
    mov dx, msg_access_error
    jmp fatal_message
address_error:
    mov dx, msg_address_error
fatal_message:
    call print_dollar
    mov ax, 0x4C01
    int 0x21

find_installed_service:
    mov ax, 0x3561
    int 0x21
    cmp word [es:bx + 3], 0x4B50          ; PK
    jne .missing
    cmp word [es:bx + 5], 0x2054          ; T<space>
    jne .missing
    cmp word [es:bx + 7], 0x5244          ; DR
    jne .missing
    cmp word [es:bx + 9], 0x5256          ; VR
    jne .missing
    cmp word [es:bx + 11], 0x4943         ; CI
    jne .missing
    cmp word [es:bx + 13], 0x4B55         ; UK
    jne .missing
    clc
    ret
.missing:
    stc
    ret

find_physical_driver:
    mov ax, 0x3560
    int 0x21
    cmp word [es:bx + 3], 0x4B50
    jne .missing
    cmp word [es:bx + 5], 0x2054
    jne .missing
    cmp word [es:bx + 7], 0x5244
    jne .missing
    cmp word [es:bx + 9], 0x5256
    jne .missing
    clc
    ret
.missing:
    stc
    ret

exec_ne2000:
    mov ah, 0x62
    int 0x21
    mov word [exec_block], 0
    mov word [exec_block + 2], ne2000_tail
    mov [exec_block + 4], cs
    mov word [exec_block + 6], 0x005C
    mov [exec_block + 8], bx
    mov word [exec_block + 10], 0x006C
    mov [exec_block + 12], bx
    push cs
    pop es
    mov bx, exec_block
    mov dx, ne2000_path
    mov ax, 0x4B00
    int 0x21
    jc .done
    mov ah, 0x4D
    int 0x21
    or al, al
    jnz .failed
    clc
    ret
.failed:
    stc
.done:
    ret

; Load IPADDR from the canonical mTCP profile into loaded_ip.
load_config_ip:
    push cs
    pop es
    mov dx, config_path
    mov ax, 0x3D00
    int 0x21
    jc .fail
    mov bx, ax
    mov dx, config_buffer
    mov cx, CONFIG_BUFFER_SIZE - 1
    mov ah, 0x3F
    int 0x21
    pushf
    push ax
    mov ah, 0x3E
    int 0x21
    pop ax
    popf
    jc .fail
    mov si, config_buffer
    add si, ax
    mov byte [si], 0
    mov si, config_buffer
.next_line:
    cmp byte [si], 0
    je .fail
    cmp byte [si], 0x0D
    je .advance
    cmp byte [si], 0x0A
    je .advance
    mov di, key_ipaddr
    mov bx, si
.compare:
    mov al, [di]
    test al, al
    jz .key_found
    cmp al, [bx]
    jne .skip_line
    inc di
    inc bx
    jmp .compare
.key_found:
    cmp byte [bx], ' '
    je .skip_spaces
    cmp byte [bx], 9
    jne .skip_line
.skip_spaces:
    inc bx
    cmp byte [bx], ' '
    je .skip_spaces
    cmp byte [bx], 9
    je .skip_spaces
    mov si, bx
    mov di, loaded_ip
    call parse_ipv4
    ret
.skip_line:
    cmp byte [si], 0
    je .fail
    lodsb
    cmp al, 0x0A
    jne .skip_line
    jmp .next_line
.advance:
    inc si
    jmp .next_line
.fail:
    stc
    ret

; DS:SI dotted decimal -> DS:DI four bytes.  Stops only at CR/LF/NUL/space.
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
    mov al, [si]
    cmp al, 0
    je .good
    cmp al, 0x0D
    je .good
    cmp al, 0x0A
    je .good
    cmp al, ' '
    je .good
    cmp al, 9
    jne .bad
.good:
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

print_dollar:
    ; NETSTART can run during the graphical splash. Keep DOS text out of the
    ; framebuffer in VBE modes; mirror those diagnostics to BIOS COM1 instead.
    push bp
    mov bp, sp
    push ax
    push bx
    push dx
    push si
    mov [cs:print_ptr], dx
    mov ah, 0x0F
    int 0x10
    cmp al, 3
    jbe .console
    mov si, [cs:print_ptr]
.serial:
    lodsb
    cmp al, '$'
    je .serial_done
    mov ah, 1
    xor dx, dx
    int 0x14
    jmp .serial
.serial_done:
    pop si
    pop dx
    pop bx
    pop ax
    pop bp
    ret
.console:
    pop si
    pop dx
    pop bx
    pop ax
    pop bp
    mov ah, 0x09
    int 0x21
    ret

print_hex16:
    push ax
    push bx
    push cx
    push dx
    mov bx, ax
    mov cx, 4
.digit:
    rol bx, 4
    mov dl, bl
    and dl, 0x0F
    add dl, '0'
    cmp dl, '9'
    jbe .emit
    add dl, 7
.emit:
    call print_char
    loop .digit
    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_char:
    mov [cs:print_char_byte], dl
    push ax
    push bx
    push dx
    mov ah, 0x0F
    int 0x10
    cmp al, 3
    jbe .dos_char
    mov al, [cs:print_char_byte]
    mov ah, 1
    xor dx, dx
    int 0x14
    jmp .char_done
.dos_char:
    mov dl, [cs:print_char_byte]
    mov ah, 2
    int 0x21
.char_done:
    pop dx
    pop bx
    pop ax
    ret

msg_installed db 'NETSTART: ICMP resident active; mTCP Packet Driver is INT 61h', 13, 10, '$'
print_ptr dw 0
print_char_byte db 0
msg_reloaded db 'ICMPD: resident IPv4 address reloaded from MTCP.CFG', 13, 10, '$'
msg_config_error db 'NETSTART: invalid or unreadable C:\NET\MTCP.CFG IPADDR', 13, 10, '$'
msg_driver_load_error db 'NETSTART: cannot execute C:\NET\NE2000.COM', '$'
msg_exec_ax db ' (AX=', '$'
msg_crlf db ')', 13, 10, '$'
msg_driver_missing db 'NETSTART: NE2000 Packet Driver did not install at INT 60h', 13, 10, '$'
msg_access_error db 'NETSTART: cannot acquire the physical Packet Driver', 13, 10, '$'
msg_address_error db 'NETSTART: cannot read the NE2000 MAC address', 13, 10, '$'

config_path db 'C:\NET\MTCP.CFG', 0
key_ipaddr db 'IPADDR', 0
ne2000_path db 'C:\NET\NE2000.COM', 0
ne2000_tail db 13, ' 0X60 3 0X300', 0x0D
exec_block times 14 db 0
loaded_ip times 4 db 0
CONFIG_BUFFER_SIZE equ 4096
config_buffer times CONFIG_BUFFER_SIZE db 0
installer_stack times 512 db 0
installer_stack_top:
program_end:
