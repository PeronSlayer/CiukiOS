bits 16
org 0x1000

%define STAGE2_ENTRY 0x1000
%define STAGE2_SEG   0x1000
%define STAGE2_STACK_SIZE 2048
%define STAGE2_STACK_BASE (STAGE2_ENTRY + 0xFFFE - STAGE2_STACK_SIZE)

%define FAT_SPT 18
%define FAT_HEADS 2
%define FAT_RESERVED_SECTORS 21
%define FAT_SECTORS_PER_FAT 9
%define FAT_COUNT 2
%define FAT_ROOT_DIR_SECTORS 14
%define FAT_TYPE 12
%define FAT_EOF 0xFF8
%define FAT1_LBA FAT_RESERVED_SECTORS
%define FAT2_LBA (FAT1_LBA + FAT_SECTORS_PER_FAT)
%define FAT_ROOT_START_LBA (FAT_RESERVED_SECTORS + (FAT_COUNT * FAT_SECTORS_PER_FAT))
%define FAT_DATA_START_LBA (FAT_ROOT_START_LBA + FAT_ROOT_DIR_SECTORS)

%define DOS_META_BUF_SEG 0x7000
%define DOS_FAT_BUF_SEG  0x7200
%define DOS_IO_BUF_SEG   0x7400

%define RUNTIME_LOAD_SEG 0xA000

section .text
stage2_start:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STAGE2_STACK_BASE
    sti

    mov si, msg_stage2_entry
    call print_string_serial

    call init_mouse
    call init_vbe_query
    call install_int33_vector

    mov si, msg_stage2_ready
    call print_string_serial

    call load_and_boot_runtime

    mov si, msg_stage2_done
    call print_string_serial

    jmp .halt
.halt:
    cli
    hlt
    jmp .halt

init_mouse:
    push ax
    push bx
    mov ax, 0x0000
    int 0x33
    cmp ax, 0xFFFF
    jne .no_mouse
    mov ax, 0x0001
    int 0x33
    mov si, msg_mouse_enabled
    call print_string_serial
    pop bx
    pop ax
    ret
.no_mouse:
    mov si, msg_mouse_not_found
    call print_string_serial
    pop bx
    pop ax
    ret

init_vbe_query:
    mov si, msg_vbe_init
    call print_string_serial
    ret

install_int33_vector:
    push ax
    push bx
    push es
    xor ax, ax
    mov es, ax
    mov bx, 0x33 * 4
    mov word [es:bx], int33_handler
    mov ax, cs
    mov [es:bx + 2], ax
    pop es
    pop bx
    pop ax
    ret

int33_handler:
    push ax
    mov al, 0
    jmp .done
.done:
    pop ax
    iret

load_and_boot_runtime:
    mov si, msg_loading_runtime
    call print_string_serial

    mov si, path_runtime
    call find_file_in_root
    jc .load_fail

    mov ax, RUNTIME_LOAD_SEG
    mov es, ax
    xor di, di
    call load_file_to_es
    jc .load_fail

    mov si, msg_runtime_loaded
    call print_string_serial

    call boot_runtime
    ret

.load_fail:
    mov si, msg_runtime_load_fail
    call print_string_serial
    ret

boot_runtime:
    mov si, msg_booting_runtime
    call print_string_serial

    mov ax, 0x0013
    int 0x10

    mov si, msg_runtime_started
    call print_string_serial

    ret

find_file_in_root:
    push ax
    push bx
    push cx
    push dx
    push di
    push es

    mov ax, DOS_META_BUF_SEG
    mov es, ax

    mov ax, FAT_ROOT_START_LBA
    xor bx, bx
    call read_sector_lba
    jc .fail

    xor bx, bx
.scan_loop:
    cmp bx, 224
    jae .not_found
    mov di, bx
    shl di, 5

    cmp byte [es:di], 0x00
    je .not_found
    cmp byte [es:di], 0xE5
    je .next_entry

    mov cx, 8
    mov si, path_name
    xor dx, dx
.cmp_name:
    mov al, [si]
    cmp al, 0
    je .check_ext
    mov bx, di
    add bx, dx
    cmp al, [es:bx]
    jne .next_entry
    inc si
    inc dx
    loop .cmp_name

.check_ext:
    mov cx, 3
    mov si, path_ext
    xor ax, ax
.cmp_ext:
    mov al, [si]
    cmp al, 0
    je .found
    mov bx, di
    add bx, 8
    add bx, ax
    cmp al, [es:bx]
    jne .next_entry
    inc si
    inc ax
    loop .cmp_ext

.found:
    clc
    jmp .done

.next_entry:
    inc bx
    jmp .scan_loop

.not_found:
    mov ax, 0x0002
    stc
    jmp .done

.fail:
    mov ax, 0x0005
    stc

.done:
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

read_sector_lba:
    push ax
    push bx
    push cx
    push dx
    push di

    mov [tmp_lba], ax

    mov ax, [tmp_lba]
    xor dx, dx
    mov cx, FAT_SPT
    div cx
    mov cl, al
    mov al, ah
    inc al
    mov ch, 0
    mov dl, 0

    mov ax, [tmp_lba]
    xor dx, dx
    mov cx, (FAT_SPT * FAT_HEADS)
    div cx
    mov dh, al
    mov ch, 0

    mov ax, 0x0201
    int 0x13
    jc .fail

    clc
    jmp .done

.fail:
    stc

.done:
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

load_file_to_es:
    push ax
    push bx
    push cx
    push dx
    push di
    push si

    mov ax, 0x8000
    mov [tmp_cluster], ax
    mov word [tmp_done], 0

.load_loop:
    cmp word [tmp_done], 65536
    jae .done

    mov ax, [tmp_cluster]
    cmp ax, FAT_EOF
    jae .done

    mov bx, ax
    sub bx, 2
    mov cx, 1
    add bx, FAT_DATA_START_LBA

    call read_sector_lba
    jc .fail

    add di, 512
    cmp di, 0
    jne .load_loop
    add word [tmp_done], 512

    jmp .load_loop

.done:
    clc
    jmp .done2

.fail:
    stc

.done2:
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_string_serial:
    lodsb
    test al, al
    jz .done
    call serial_putc
    jmp print_string_serial
.done:
    ret

serial_putc:
    push ax
    push dx
    mov ah, al          ; Salva il byte da spedire in ah
.wait:
    mov dx, 0x03FD     ; Porta status
    in al, dx
    test al, 0x20      ; Bit 5 = TX ready?
    jz .wait            ; Se no, aspetta
    mov dx, 0x03F8     ; Porta TX
    mov al, ah          ; Ripristina il byte
    out dx, al          ; Invia
    pop dx
    pop ax
    ret

msg_stage2_entry db "[STAGE2] Entry", 13, 10, 0
msg_stage2_ready db "[STAGE2] Ready", 13, 10, 0
msg_stage2_done db "[STAGE2] Complete", 13, 10, 0
msg_mouse_enabled db "[STAGE2] Mouse INT33h installed", 13, 10, 0
msg_mouse_not_found db "[STAGE2] Mouse not detected", 13, 10, 0
msg_vbe_init db "[STAGE2] VBE query initialized", 13, 10, 0
msg_loading_runtime db "[STAGE2] Loading runtime...", 13, 10, 0
msg_runtime_loaded db "[STAGE2] Runtime loaded", 13, 10, 0
msg_booting_runtime db "[STAGE2] Booting runtime...", 13, 10, 0
msg_runtime_started db "[STAGE2] Runtime started (mode 13h)", 13, 10, 0
msg_runtime_load_fail db "[STAGE2] Runtime load failed", 13, 10, 0

path_runtime db "RUNTIME SYS", 0
path_name db "RUNTIME ", 0
path_ext db "SYS", 0

tmp_lba dw 0
tmp_cluster dw 0
tmp_done dw 0
