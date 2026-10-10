; Ring-3 stand-in and disposable clients. Only abi.h-generated public values.
; SPDX-License-Identifier: GPL-2.0-only
bits 32
%include "proc_abi.inc"
org CIUKI_IMAGE_BASE - CIUKI_PAGE_SIZE
%define DATA (CIUKI_IMAGE_BASE + CIUKI_PAGE_SIZE)
%define MODE (DATA+0)
%define STAGE (DATA+4)
%define ERRORS (DATA+8)
%define TURNS (DATA+12)
%define TICKS (DATA+16)
%define ENDPOINT (DATA+20)
%define VICTIM_ENDPOINT (DATA+24)
%define RELEASE (DATA+28)
%define SURFACE (DATA+32)
%define MAPPING (DATA+36)
%define DISPLAY (DATA+40)
%define INPUT (DATA+44)
%define UNAUTHORIZED (DATA+48)
%define ACK (DATA+52)
%define MESSAGE (DATA+128)
%define QUERY (DATA+512)
%define ACTION (DATA+640)
%macro CALL 1
    mov eax, %1
    int 0x80
%endmacro
%macro EQ 2
    cmp %1, %2
    je %%ok
    inc dword [ERRORS]
%%ok:
%endmacro
    db 0x7f, 'ELF', 1, 1, 1, 0, 0
    times 7 db 0
    dw 2, 3
    dd 1, entry, phdr-$$, 0, 0
    dw 52, 32, 2, 0, 0, 0
phdr:
    dd 1, CIUKI_PAGE_SIZE, CIUKI_IMAGE_BASE, 0, code_end-entry, CIUKI_PAGE_SIZE, 5, CIUKI_PAGE_SIZE
    dd 1, 2*CIUKI_PAGE_SIZE, DATA, 0, 4, CIUKI_PAGE_SIZE, 6, CIUKI_PAGE_SIZE
    times CIUKI_PAGE_SIZE-($-$$) db 0
entry:
    cmp dword [MODE], 0
    je server
    ; Every client checks that forged grants cannot expose device geometry/input.
    mov ebx, 127
    mov ecx, QUERY
    CALL CIUKI_SYS_DISPLAY_INFO
    EQ eax, -EBADF
    test eax, eax
    js .no_display
    inc dword [UNAUTHORIZED]
.no_display:
    mov ebx, 127
    mov ecx, QUERY
    mov edx, 1
    CALL CIUKI_SYS_INPUT_READ
    EQ eax, -EBADF
    test eax, eax
    js .no_input
    inc dword [UNAUTHORIZED]
.no_input:
    mov ebx, 8
    mov ecx, 8
    mov edx, CIUKI_SURFACE_XRGB8888
    CALL CIUKI_SYS_SURFACE_CREATE
    test eax, eax
    js failure
    mov [SURFACE], eax
    mov ebx, eax
    mov ecx, PROT_READ|PROT_WRITE
    CALL CIUKI_SYS_SURFACE_MAP
    cmp eax, -CIUKI_SYSCALL_ERROR_MAX
    jae failure
    mov [MAPPING], eax
    mov dword [eax], 0x123456
    mov dword [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FD_COUNT], 1
    mov eax, [SURFACE]
    mov [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FDS], eax
    mov ebx, [ENDPOINT]
    mov ecx, MESSAGE
    xor edx, edx
    CALL CIUKI_SYS_CHANNEL_SEND
    EQ eax, 0
    mov dword [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FD_COUNT], 0
    mov dword [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FDS], 0
    mov dword [STAGE], 1
    cmp dword [MODE], 1
    je client
.wait_ack:
    cmp dword [ACK], 1
    je .pending
    call tick
    jmp .wait_ack
.pending:
    mov dword [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FD_COUNT], 1
    mov eax, [SURFACE]
    mov [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FDS], eax
    ; Leave a second message pending when the fault occurs.
    mov ebx, [ENDPOINT]
    mov ecx, MESSAGE
    xor edx, edx
    CALL CIUKI_SYS_CHANNEL_SEND
    EQ eax, 0
    mov dword [STAGE], 2
    cmp dword [MODE], 3
    je closed_peer
    cmp dword [MODE], 4
    je forged
    cmp dword [MODE], 5
    je handler_fault
    call fault_gate
    mov dword [0], 1
    jmp failure
closed_peer:
    ; A separate pair proves last-peer-close => SIGPIPE while the server
    ; transaction and shared mapping above are still live.
    mov ebx, QUERY
    CALL CIUKI_SYS_CHANNEL_PAIR
    EQ eax, 0
    mov ebx, [QUERY+4]
    CALL CIUKI_SYS_CLOSE
    EQ eax, 0
    mov ebp, [QUERY]
    call fault_gate
    mov ebx, ebp
    mov ecx, MESSAGE
    xor edx, edx
    CALL CIUKI_SYS_CHANNEL_SEND
    jmp failure
forged:
    mov ebx, [ENDPOINT]
    mov ecx, QUERY
    CALL CIUKI_SYS_DISPLAY_INFO
    EQ eax, -EBADF
    mov dword [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FD_COUNT], 1
    mov eax, [ENDPOINT]
    mov [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FDS], eax
    mov ebx, [ENDPOINT]
    mov ecx, MESSAGE
    xor edx, edx
    CALL CIUKI_SYS_CHANNEL_SEND
    EQ eax, -EBADF
    call fault_gate
    mov dword [0], 1
handler_fault:
    mov dword [ACTION+ABI_OFFSETOF_CIUKI_SIGACTION_HANDLER], handler
    mov dword [ACTION+ABI_OFFSETOF_CIUKI_SIGACTION_RESTORER], restorer
    mov ebx, SIGSEGV
    mov ecx, ACTION
    xor edx, edx
    CALL CIUKI_SYS_SIGACTION
    EQ eax, 0
    call fault_gate
    mov dword [0], 1
    jmp failure
handler:
    mov dword [0], 2
    jmp failure
restorer:
    ud2
fault_gate:
    mov dword [STAGE], 3
.wait:
    cmp dword [ACK], 2
    je .go
    call tick
    jmp .wait
.go:
    ret
client:
    ; Consume the initial surface reply, then perform sequential turns.
    call receive
    inc dword [TURNS]
.loop:
    call clear_message
    mov ebx, [ENDPOINT]
    mov ecx, MESSAGE
    xor edx, edx
    CALL CIUKI_SYS_CHANNEL_SEND
    EQ eax, 0
    call receive
    inc dword [TURNS]
    call tick
.pause:
    cmp dword [ACK], 1
    jne .resume
    mov dword [STAGE], 2
    call tick
    jmp .pause
.resume:
    mov dword [STAGE], 1
    jmp .loop
receive:
    mov ebx, [ENDPOINT]
    mov ecx, MESSAGE
    xor edx, edx
    CALL CIUKI_SYS_CHANNEL_RECV
    EQ eax, 1
    ret
server:
    mov dword [STAGE], 1
.loop:
    cmp dword [ENDPOINT], -1
    je .victim
    mov ebx, [ENDPOINT]
    mov ecx, MESSAGE
    mov edx, DONTWAIT
    CALL CIUKI_SYS_CHANNEL_RECV
    cmp eax, -EAGAIN
    je .victim
    cmp eax, 1
    jne failure
    call consume_surface
    call clear_message
    mov ebx, [ENDPOINT]
    mov ecx, MESSAGE
    xor edx, edx
    CALL CIUKI_SYS_CHANNEL_SEND
    EQ eax, 0
    inc dword [TURNS]
.victim:
    ; ACK=1 prevents draining the pending transaction before the victim dies.
    cmp dword [VICTIM_ENDPOINT], -1
    je .next
    cmp dword [ACK], 1
    je .next
    mov ebx, [VICTIM_ENDPOINT]
    mov ecx, MESSAGE
    mov edx, DONTWAIT
    CALL CIUKI_SYS_CHANNEL_RECV
    cmp eax, -EAGAIN
    je .next
    EQ eax, 1
    cmp eax, 1
    jne .next
    call consume_surface
    mov dword [ACK], 1
.next:
    call tick
    jmp .loop
consume_surface:
    cmp dword [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FD_COUNT], 0
    je .done
    mov ebx, [MESSAGE+ABI_OFFSETOF_CIUKI_MESSAGE_FDS]
    mov [SURFACE], ebx
    mov ecx, PROT_READ|PROT_WRITE
    CALL CIUKI_SYS_SURFACE_MAP
    EQ eax, -EACCES
    mov ebx, [SURFACE]
    mov ecx, PROT_READ
    CALL CIUKI_SYS_SURFACE_MAP
    cmp eax, -CIUKI_SYSCALL_ERROR_MAX
    jae failure
    mov [MAPPING], eax
    EQ dword [eax], 0x123456
    mov ebx, [SURFACE]
    CALL CIUKI_SYS_CLOSE
    EQ eax, 0
    mov eax, [MAPPING]
    EQ dword [eax], 0x123456
    mov ebx, eax
    mov ecx, CIUKI_PAGE_SIZE
    CALL CIUKI_SYS_MUNMAP
    EQ eax, 0
.done:
    ret
clear_message:
    push edi
    mov edi, MESSAGE
    xor eax, eax
    mov ecx, ABI_SIZEOF_CIUKI_MESSAGE/4
    rep stosd
    pop edi
    ret
tick:
    cmp dword [RELEASE], 0
    jne done
    mov ebx, QUERY
    mov ecx, 80
    CALL 5
    mov eax, [QUERY+4]
    mov [TICKS], eax
    mov ebx, 1
    CALL 4
    ret
failure:
    inc dword [ERRORS]
done:
    mov ebx, [ERRORS]
    CALL 0
    ud2
code_end:
    times 2*CIUKI_PAGE_SIZE-($-$$) db 0
    dd 0
