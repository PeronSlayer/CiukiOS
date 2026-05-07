; format.asm - CiukiOS FORMAT.COM (native HDD partition initializer, slice 1)
;
; Slice 1 scope:
;   - Probe target HDD geometry via INT 13h AH=0x08
;   - Confirm destructive operation with the user (type DESTROY)
;   - Write a CiukiOS-compatible MBR (chain-loader + FAT16 partition entry)
;     to LBA 0 of the target HDD using INT 13h AH=0x43 EDD with a retry path
;     and a CHS AH=0x03 fallback for real BIOSes that ship without EDD on
;     internal HDDs.
;   - Report success / failure on screen and on COM1 serial.
;
; Slice 1 explicitly does NOT initialize the FAT16 BPB/FATs/root directory.
; A subsequent slice will add the FAT16 metadata and file-by-file copy path.

bits 16
org 0x0100

%define MBR_PARTITION_LBA 63
%define MBR_PARTITION_SECTORS 0x00040000   ; 128 MB FAT16 (matches full-CD layout)
%define TARGET_DRIVE 0x81                  ; live-CD install target (BIOS HDD #2)
%define SOURCE_DRIVE 0x80                  ; live-CD source media (BIOS HDD #1)

start:
    cld
    push cs
    pop ds
    push cs
    pop es

    call serial_init_com1
    mov dx, msg_serial_start
    call serial_write_z
    call serial_write_crlf

    mov dx, msg_banner
    call print_line
    mov dx, msg_banner_v
    call print_line

    call parse_command_tail
    cmp al, 0xFF
    jne .have_drive
    mov al, 2                              ; default to C: when no arg given
.have_drive:
    cmp al, 2                              ; only C: supported in slice 1
    jne .bad_drive

    call probe_target
    jc .probe_fail

    call print_geometry

    cmp byte [force_flag], 1
    je .skip_confirm
    call confirm_destructive
    jc .user_abort
.skip_confirm:

    call write_mbr_sector
    jc .write_fail

    mov dx, msg_serial_done
    call serial_write_z
    call serial_write_crlf

    mov dx, msg_screen_done
    call print_line

    mov ax, 0x4C00
    int 0x21

.bad_drive:
    mov dx, msg_bad_drive
    call print_line
    mov ax, 0x4C02
    int 0x21

.probe_fail:
    mov dx, msg_serial_probe_fail
    call serial_write_z
    mov al, [last_int13_status]
    call serial_write_hex_byte
    call serial_write_crlf
    mov dx, msg_screen_probe_fail
    call print_z
    mov al, [last_int13_status]
    call print_u8_dec
    call print_crlf
    mov ax, 0x4C03
    int 0x21

.user_abort:
    mov dx, msg_serial_abort
    call serial_write_z
    call serial_write_crlf
    mov dx, msg_screen_abort
    call print_line
    mov ax, 0x4C01
    int 0x21

.write_fail:
    mov dx, msg_serial_write_fail
    call serial_write_z
    mov al, [last_int13_status]
    call serial_write_hex_byte
    call serial_write_crlf
    mov dx, msg_screen_write_fail
    call print_z
    mov al, [last_int13_status]
    call print_u8_dec
    mov dx, msg_screen_write_path
    call print_z
    mov al, [last_int13_path]
    call print_char_dl_al
    call print_crlf
    mov ax, 0x4C04
    int 0x21

; -----------------------------------------------------------------------------
; Command tail parser: reads PSP DTA at 0x80 (length byte + tail bytes).
; Looks for the first letter A..Z or a..z and converts it to a drive index.
; Returns AL = drive index (0..25) or 0xFF if no letter found.
; -----------------------------------------------------------------------------
parse_command_tail:
    push si
    push cx
    push dx
    mov byte [force_flag], 0
    mov dl, 0xFF                          ; DL = pending drive index
    mov si, 0x0081
    mov cl, [0x0080]
    xor ch, ch
    or cx, cx
    jz .done

.scan:
    lodsb
    cmp al, '/'
    je .switch
    cmp al, '-'
    je .switch
    cmp al, 'a'
    jb .check_upper
    cmp al, 'z'
    ja .next
    sub al, ('a' - 'A')

.check_upper:
    cmp al, 'A'
    jb .next
    cmp al, 'Z'
    ja .next
    cmp dl, 0xFF
    jne .next
    sub al, 'A'
    mov dl, al
    jmp .next

.switch:
    or cx, cx
    jz .done
    lodsb
    dec cx
    cmp al, 'F'
    je .force
    cmp al, 'f'
    je .force
    jmp .next

.force:
    mov byte [force_flag], 1
    jmp .next

.next:
    loop .scan

.done:
    mov al, dl
    pop dx
    pop cx
    pop si
    ret

; -----------------------------------------------------------------------------
; INT 13h AH=0x08 geometry probe of TARGET_DRIVE.
; On success stores SPT/heads in target_spt/target_heads and returns CF=0.
; On failure sets last_int13_status and returns CF=1.
; -----------------------------------------------------------------------------
probe_target:
    push ax
    push bx
    push cx
    push dx
    push di
    push es

    xor ax, ax
    mov es, ax
    xor di, di
    mov dl, TARGET_DRIVE
    mov ah, 0x08
    int 0x13
    jc .fail

    push cs
    pop ds
    and cl, 0x3F
    jz .fail_zero
    xor ax, ax
    mov al, cl
    mov [target_spt], ax
    xor ax, ax
    mov al, dh
    inc ax
    mov [target_heads], ax

    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

.fail:
    mov [last_int13_status], ah
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

.fail_zero:
    mov byte [last_int13_status], 0xFE
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

; -----------------------------------------------------------------------------
; Print probed target drive geometry (cyl/head/sector approximation).
; -----------------------------------------------------------------------------
print_geometry:
    mov dx, msg_geom_prefix
    call print_z
    mov ax, [target_heads]
    call print_u16_dec
    mov dx, msg_geom_heads
    call print_z
    mov ax, [target_spt]
    call print_u16_dec
    mov dx, msg_geom_spt
    call print_line
    ret

; -----------------------------------------------------------------------------
; Read up to 8 chars and check for "DESTROY" before Enter.
; CF=0 if DESTROY confirmed; CF=1 otherwise (Esc, mismatch, empty).
; -----------------------------------------------------------------------------
confirm_destructive:
    mov dx, msg_destroy_prompt
    call print_line

    mov di, input_buf
    mov cx, 8

.loop:
    mov ah, 0x01
    int 0x21                      ; read with echo
    cmp al, 13
    je .check
    cmp al, 27
    je .abort
    cmp al, 8
    je .backspace
    cmp cx, 0
    je .loop
    stosb
    dec cx
    jmp .loop

.backspace:
    cmp di, input_buf
    je .loop
    dec di
    inc cx
    jmp .loop

.check:
    call print_crlf
    mov si, input_buf
    mov bx, di
    sub bx, si
    cmp bx, 7
    jne .mismatch
    push cs
    pop es
    mov si, input_buf
    mov di, str_destroy
    mov cx, 7
    repe cmpsb
    jne .mismatch
    clc
    ret

.mismatch:
    mov dx, msg_mismatch
    call print_line
    stc
    ret

.abort:
    call print_crlf
    stc
    ret

; -----------------------------------------------------------------------------
; Build the MBR sector image (chain-loader + active FAT16 partition entry)
; in io_buffer and write it to LBA 0 of TARGET_DRIVE.
; -----------------------------------------------------------------------------
write_mbr_sector:
    push ax
    push cx
    push dx
    push si
    push di
    push es

    push cs
    pop es
    mov di, io_buffer
    xor ax, ax
    mov cx, 256
    rep stosw

    mov si, mbr_template
    mov di, io_buffer
    mov cx, mbr_template_end - mbr_template
    rep movsb

    mov word [io_buffer + 510], 0xAA55

    mov byte [io_buffer + 446 + 0], 0x80                       ; active flag
    mov byte [io_buffer + 446 + 1], 0x01                       ; head
    mov byte [io_buffer + 446 + 2], 0x01                       ; sector/cylinder hi
    mov byte [io_buffer + 446 + 3], 0x00                       ; cylinder lo
    mov byte [io_buffer + 446 + 4], 0x06                       ; type FAT16
    mov byte [io_buffer + 446 + 5], 0xFE                       ; end head
    mov byte [io_buffer + 446 + 6], 0xFF                       ; end sect/cyl hi
    mov byte [io_buffer + 446 + 7], 0xFF                       ; end cyl lo
    mov dword [io_buffer + 446 + 8], MBR_PARTITION_LBA
    mov dword [io_buffer + 446 + 12], MBR_PARTITION_SECTORS

    mov word [target_lba_lo], 0
    mov word [target_lba_hi], 0
    mov bx, io_buffer
    mov dl, TARGET_DRIVE
    mov ah, 0x43                                               ; EDD write
    call int13_transfer_with_retry
    jnc .ok

    mov bx, io_buffer
    mov dl, TARGET_DRIVE
    mov ah, 0x03                                               ; CHS write fallback
    call int13_chs_transfer
    jnc .ok

    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop ax
    stc
    ret

.ok:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop ax
    clc
    ret

; -----------------------------------------------------------------------------
; INT 13h AH=0x42/0x43 transfer with reset+retry on first failure.
; Inputs: AH=op, DL=drive, BX=buffer offset (CS:BX), [target_lba_lo/hi]=LBA
; Output: CF on failure, last_int13_status/path updated.
; -----------------------------------------------------------------------------
int13_transfer_with_retry:
    push ax
    push bx
    push cx
    push dx
    push si
    push cs
    pop ds

    mov byte [last_int13_path], 'E'
    mov [retry_op], ah
    mov [retry_drive], dl

    mov word [dap + 0], 0x0010
    mov word [dap + 2], 0x0001
    mov word [dap + 4], bx
    mov bx, cs
    mov word [dap + 6], bx
    mov bx, [target_lba_lo]
    mov word [dap + 8], bx
    mov bx, [target_lba_hi]
    mov word [dap + 10], bx
    mov word [dap + 12], 0
    mov word [dap + 14], 0
    mov si, dap
    xor al, al
    int 0x13
    jnc .ok

    mov dl, [retry_drive]
    xor ax, ax
    int 0x13                                                   ; reset

    push cs
    pop ds
    mov word [dap + 0], 0x0010
    mov word [dap + 2], 0x0001
    mov si, dap
    mov dl, [retry_drive]
    mov ah, [retry_op]
    xor al, al
    int 0x13
    jc .fail

.ok:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

.fail:
    mov [last_int13_status], ah
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

; -----------------------------------------------------------------------------
; CHS fallback: INT 13h AH=0x02/0x03 using probed target geometry.
; Inputs identical to int13_transfer_with_retry. LBA -> CHS via target_spt and
; target_heads probed earlier.
; -----------------------------------------------------------------------------
int13_chs_transfer:
    push ax
    push bx
    push cx
    push dx
    push si
    push es
    push cs
    pop ds

    mov [retry_op], ah
    mov [retry_drive], dl
    mov si, bx
    mov byte [last_int13_path], 'C'

    mov ax, [target_spt]
    or ax, ax
    jz .fail_geom
    mul word [target_heads]
    or dx, dx
    jnz .fail_geom
    or ax, ax
    jz .fail_geom
    mov [chs_spc], ax

    mov ax, [target_lba_lo]
    mov dx, [target_lba_hi]
    div word [chs_spc]
    mov [chs_cylinder], ax
    mov ax, dx
    xor dx, dx
    div word [target_spt]

    mov dh, al
    mov cl, dl
    inc cl
    mov ax, [chs_cylinder]
    mov ch, al
    mov al, ah
    and al, 0x03
    shl al, 6
    or cl, al

    push cs
    pop es
    mov bx, si
    mov dl, [retry_drive]
    mov ah, [retry_op]
    mov al, 0x01
    int 0x13
    jc .fail

    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

.fail:
    mov [last_int13_status], ah
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

.fail_geom:
    mov byte [last_int13_status], 0xFD
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

; -----------------------------------------------------------------------------
; Tiny helpers
; -----------------------------------------------------------------------------
print_line:
    call print_z
    call print_crlf
    ret

print_z:
    push ax
    push dx
    push si
    mov si, dx
.loop:
    lodsb
    or al, al
    jz .done
    mov dl, al
    mov ah, 0x02
    int 0x21
    jmp .loop
.done:
    pop si
    pop dx
    pop ax
    ret

print_crlf:
    push ax
    push dx
    mov dl, 13
    mov ah, 0x02
    int 0x21
    mov dl, 10
    mov ah, 0x02
    int 0x21
    pop dx
    pop ax
    ret

print_char_dl_al:
    push ax
    push dx
    mov dl, al
    mov ah, 0x02
    int 0x21
    pop dx
    pop ax
    ret

print_u8_dec:
    push ax
    push bx
    push cx
    push dx
    xor ah, ah
    mov bl, 100
    div bl
    mov ch, al
    mov al, ah
    xor ah, ah
    mov bl, 10
    div bl
    mov cl, al
    mov bl, ah
    cmp ch, 0
    je .skip_h
    mov dl, ch
    add dl, '0'
    mov ah, 0x02
    int 0x21
.skip_h:
    cmp ch, 0
    jne .print_t
    cmp cl, 0
    je .print_o
.print_t:
    mov dl, cl
    add dl, '0'
    mov ah, 0x02
    int 0x21
.print_o:
    mov dl, bl
    add dl, '0'
    mov ah, 0x02
    int 0x21
    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_u16_dec:
    push ax
    push bx
    push cx
    push dx
    push si
    mov si, dec_u16_buf + 5
    mov byte [si], 0
    mov bx, 10
.div_loop:
    xor dx, dx
    div bx
    dec si
    add dl, '0'
    mov [si], dl
    or ax, ax
    jnz .div_loop
    mov dx, si
    call print_z
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

serial_init_com1:
    push ax
    push dx
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 0x01
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al
    pop dx
    pop ax
    ret

serial_write_char:
    push ax
    push dx
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait
    pop dx
    pop ax
    push dx
    mov dx, 0x3F8
    out dx, al
    pop dx
    ret

serial_write_z:
    push ax
    push dx
    push si
    mov si, dx
.loop:
    lodsb
    or al, al
    jz .done
    call serial_write_char
    jmp .loop
.done:
    pop si
    pop dx
    pop ax
    ret

serial_write_crlf:
    push ax
    mov al, 13
    call serial_write_char
    mov al, 10
    call serial_write_char
    pop ax
    ret

serial_write_hex_nibble:
    and al, 0x0F
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    call serial_write_char
    ret

serial_write_hex_byte:
    push ax
    mov ah, al
    shr al, 4
    call serial_write_hex_nibble
    mov al, ah
    and al, 0x0F
    call serial_write_hex_nibble
    pop ax
    ret

; -----------------------------------------------------------------------------
; Data
; -----------------------------------------------------------------------------
msg_banner          db 'CiukiOS FORMAT', 0
msg_banner_v        db 'Native MBR initializer (slice 1)', 0
msg_bad_drive       db 'Only C: is supported in slice 1.', 0
msg_geom_prefix     db 'Target HDD: ', 0
msg_geom_heads      db ' heads, ', 0
msg_geom_spt        db ' sectors/track', 0
msg_destroy_prompt  db 'WARNING: writing MBR to BIOS HDD #2 will destroy data.', 13, 10, 'Type DESTROY then Enter to continue, or Esc to abort.', 0
msg_mismatch        db 'Confirmation mismatch. Aborted.', 0
msg_screen_done     db 'MBR written. Target now has a FAT16 partition table.', 0
msg_screen_probe_fail db 'Target probe failed. AH=', 0
msg_screen_write_fail db 'MBR write failed. AH=', 0
msg_screen_write_path db ' P=', 0
msg_screen_abort    db 'Aborted by user.', 0

msg_serial_start    db '[FORMAT-NATIVE] START', 0
msg_serial_done     db '[FORMAT-NATIVE] DONE', 0
msg_serial_abort    db '[FORMAT-NATIVE] ABORT', 0
msg_serial_probe_fail db '[FORMAT-NATIVE] PROBE-FAIL AH=', 0
msg_serial_write_fail db '[FORMAT-NATIVE] WRITE-FAIL AH=', 0

str_destroy         db 'DESTROY', 0

mbr_template:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [mbrt_drive_slot], dl
    cld
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    rep movsw
    jmp 0x0000:0x0600 + (mbrt_relocated - mbr_template)
mbrt_relocated:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov si, 0x0600 + (mbrt_dap - mbr_template)
    mov dl, [0x0600 + (mbrt_drive_slot - mbr_template)]
    mov ah, 0x42
    int 0x13
    jnc mbrt_boot
    mov dl, [0x0600 + (mbrt_drive_slot - mbr_template)]
    xor ax, ax
    mov es, ax
    mov bx, 0x7C00
    mov ax, 0x0201
    xor ch, ch
    mov cl, 0x01
    mov dh, 0x01
    int 0x13
    jc mbrt_err
mbrt_boot:
    mov dl, [0x0600 + (mbrt_drive_slot - mbr_template)]
    jmp 0x0000:0x7C00
mbrt_err:
    cli
    hlt
    jmp mbrt_err
mbrt_drive_slot db 0
mbrt_dap:
    db 0x10, 0x00
    dw 0x0001
    dw 0x7C00
    dw 0x0000
    dd MBR_PARTITION_LBA
    dd 0
mbr_template_end:

; -----------------------------------------------------------------------------
; State / scratch (BSS-like, follows code in COM image but keeps it small)
; -----------------------------------------------------------------------------
target_spt          dw 63
target_heads        dw 16
target_lba_lo       dw 0
target_lba_hi       dw 0
chs_cylinder        dw 0
chs_spc             dw 0
retry_op            db 0
retry_drive         db 0
last_int13_status   db 0
last_int13_path     db 0
force_flag          db 0
input_buf           times 16 db 0
dec_u16_buf         times 6 db 0

align 16
dap                 db 0x10, 0x00
                    dw 0x0001
                    dw 0x0000
                    dw 0x0000
                    dq 0x0000000000000000

align 16
io_buffer           times 512 db 0
