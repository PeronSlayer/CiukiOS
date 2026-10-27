; G14 Startup menu, Step-by-step confirmation, the ScanDisk message and the
; boot-status sector (docs/design/startup-recovery.md sections 2.1, 2.2 and
; 2.4; Windows 98 Resource Kit Table 5.15, "Step-by-Step Confirmation" and
; the AutoScan setting). SPDX-License-Identifier: GPL-2.0-only
;
; Keys are BIOS INT 16h AH=00h scan/ASCII pairs (IBM PC/AT Technical
; Reference: F5 3F00h, F8 4200h, Shift+F5 5800h, Shift+F8 5B00h, Up 4800h,
; Down 5000h); AH=02h returns the shift flags, bit 2 Ctrl.

; CF: Ctrl is held. Preserves everything but AX.
startup_hotkey:
    mov ah, 2
    int 0x16
    test al, 4 ; clears CF
    jz .up
    stc
.up:
    ret

; AH=scan code of a key with AL=0. CF: no Startup menu key. Otherwise AL is
; the choice to start at once ('3', '6', '4'), or 0 for the menu (F8).
startup_scan:
    mov al, '3'
    cmp ah, 0x3f
    je .key
    mov al, '6'
    cmp ah, 0x58
    je .key
    mov al, '4'
    cmp ah, 0x5b
    je .key
    xor al, al
    cmp ah, 0x42
    je .key
    stc
    ret
.key:
    clc
    ret

; The menu, its 30-second countdown (restarted by every key) and the choice.
startup_menu:
    or byte [B(flags)+2], CBI_F_MENU_SHOWN >> 16
    mov byte [screen_quiet], 0
    mov ax, 3
    int 0x10
    mov si, startup_title
    call print
    mov al, '1'
    test byte [B(flags)+1], CBI_F_BOOT_FAILED >> 8
    jz .items
    mov si, startup_failed
    call print
    mov al, '3'
.items:
    mov [startup_choice], al
    mov si, startup_items
    call print
    mov ah, 2
    mov bh, 0
    mov dx, 0x1600
    int 0x10
    mov si, startup_keys
    call print
.again:
    mov word [startup_secs], '30'
.second:
    call startup_prompt
    mov bp, 18 ; one second at the BIOS 18.2 Hz rate
    call deadline_start
.wait:
    call progress_check
    jc .chosen
    call menu_getc
    jnc .key
    call deadline_expired
    jnc .wait
    dec byte [startup_secs+1]
    cmp byte [startup_secs+1], '0'
    jae .counted
    mov byte [startup_secs+1], '9'
    dec byte [startup_secs]
.counted:
    cmp word [startup_secs], '00'
    jne .second
    jmp .chosen
.key:
    cmp al, 13
    je .chosen
    test al, al
    jz .scan
    sub al, '1'
    cmp al, 8
    ja .again
    add al, '1'
    mov [startup_choice], al
    jmp .again
.scan:
    call startup_scan
    jnc .now
    mov al, [startup_choice]
    cmp ah, 0x48
    jne .down
    dec al
.down:
    cmp ah, 0x50
    jne .clamp
    inc al
.clamp:
    cmp al, '1'
    jb .again
    cmp al, '9'
    ja .again
    mov [startup_choice], al
    jmp .again
.now:
    test al, al ; F8 inside the menu is Step-by-step (Table 5.15)
    jnz .set
    mov al, '4'
.set:
    mov [startup_choice], al
.chosen:
    call startup_prompt
    mov si, newline
    call print
    mov al, [startup_choice]
    ; fall through
; AL=choice '1'..'9': its boot flags, a serial record, then Step-by-step.
startup_apply:
    mov [startup_choice], al
    mov byte [screen_quiet], 0
    movzx bx, al
    sub bx, '1'
    shl bx, 2
    mov eax, [startup_flags+bx]
    or [B(flags)], eax
    mov si, startup_selected
    call print
    mov al, [startup_choice]
    call menu_echo
    mov si, newline
    call print
    cmp byte [startup_choice], '4'
    jne .done
    mov si, step_table
.step:
    lodsb ; action: class bit, 0FEh log, 0FFh desktop, 0 end
    test al, al
    jz .done
    mov [step_action], al
    call print ; the question; SI then follows its NUL
    push si
    mov si, step_prompt
    call print
.answer:
    call progress_check
    jc .yes ; the menu deadline answers Yes, the normal start
    call menu_getc
    jc .answer
    cmp al, 13
    je .yes
    cmp al, 27
    jne .answer
    mov al, 'N'
    call menu_echo
    mov al, [step_action]
    cmp al, 0xfe
    je .line
    cmp al, 0xff
    jne .class
    or byte [B(flags)+2], CBI_F_CMD_ONLY >> 16
    jmp .line
.class:
    or [B(flags)+3], al
    jmp .line
.yes:
    mov al, 'Y'
    call menu_echo
    cmp byte [step_action], 0xfe
    jne .line
    or byte [B(flags)+1], (CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE) >> 8
.line:
    mov si, newline
    call print
    pop si
    jmp .step
.done:
    ret

; Row 20: "Enter a choice: N   Time remaining: SS", screen only.
startup_prompt:
    mov ah, 2
    mov bh, 0
    mov dx, 0x1400
    int 0x10
    mov si, startup_enter
    call screen_text
    mov al, [startup_choice]
    call screen_char
    mov si, startup_time
    call screen_text
    mov al, [startup_secs]
    call screen_char
    mov al, [startup_secs+1]
    ; fall through
screen_char:
    push bx
    mov ah, 0x0e
    mov bx, 7
    int 0x10
    pop bx
    ret
screen_text:
    lodsb
    test al, al
    jz .end
    call screen_char
    jmp screen_text
.end:
    ret

; The boot volume's FAT[1]: clean-shutdown bit 27 and hard-error bit 26
; (fatgen103; Resource Kit "FAT32" reserved FAT entry). Unclean asks for
; ScanDisk (thorough after a hard error); the message lets Esc skip it.
autoscan_check:
    cmp byte [fat_type], 32
    jne .done
    test dword [B(flags)], CBI_F_TEST_REQUEST
    jnz .done
    cmp byte [autoscan_mode], 0
    je .done
    mov eax, [B(flags)] ; safe mode without disks mounts nothing
    and eax, CBI_F_SAFE_MODE | CBI_F_SAFE_STORAGE
    cmp eax, CBI_F_SAFE_MODE
    je .done
    mov eax, [fat_lba]
    mov bx, sector
    call disk_read
    mov eax, [sector+4]
    xor eax, 0x0c000000
    test eax, 0x0c000000
    jz .done
    test eax, 0x04000000
    jz .unclean
    or byte [B(flags)+2], CBI_F_SCANDISK_THOROUGH >> 16
.unclean:
    or byte [B(flags)+2], CBI_F_SCANDISK >> 16
    cmp byte [autoscan_mode], 2
    je .done
    mov byte [screen_quiet], 0
    mov si, autoscan_text
    call print
    mov bp, 182 ; ten seconds
    call deadline_start
.wait:
    call progress_check
    jc .done
    call menu_getc
    jc .tick
    cmp al, 27
    jne .done ; any other key: check now
    and byte [B(flags)+2], ~((CBI_F_SCANDISK | CBI_F_SCANDISK_THOROUGH) >> 16)
    or byte [B(flags)+2], CBI_F_SCANDISK_SKIP >> 16
    mov si, autoscan_skipped
    jmp print
.tick:
    call deadline_expired
    jnc .wait
.done:
    ret

; Partition sector 9 (bootstat.h): magic and version, then the previous
; start's state, the persistent disabled/skipped classes and the requests.
bootstat_read:
    cmp byte [fat_type], 32
    jne .done
    cmp byte [cv_active], 0
    jne .done
    test dword [B(flags)], CBI_F_TEST_REQUEST
    jnz .done
    mov eax, [B(partition_lba)]
    add eax, 9
    mov bx, sector
    call disk_read
    cmp dword [sector], 0x31534243 ; 'CBS1'
    jne .done
    cmp byte [sector+5], 1
    jne .done
    mov byte [bootstat_valid], 1
    cmp byte [sector+4], 0
    je .classes
    or byte [B(flags)+1], CBI_F_BOOT_FAILED >> 8
    mov byte [startup_auto], 1
.classes:
    mov al, [sector+6]
    or al, [sector+7]
    and al, 0x7f
    or [B(flags)+3], al
    mov al, [sector+9]
    test al, 1
    jz .thorough
    or byte [B(flags)+2], CBI_F_SCANDISK >> 16
.thorough:
    test al, 2
    jz .menu
    or byte [B(flags)+2], CBI_F_SCANDISK_THOROUGH >> 16
.menu:
    test al, 4
    jz .done
    mov byte [startup_auto], 1
.done:
    ret

; After the menu: this start is "in progress" until the kernel completes it.
; Re-read the sector (the buffer was reused) and change byte 4 only. Safe
; mode without disks never mounts C:, so nothing could complete it: unmarked.
bootstat_write:
    cmp byte [bootstat_valid], 0
    je .done
    test dword [B(flags)], CBI_F_TEST_REQUEST
    jnz .done
    mov eax, [B(flags)]
    and eax, CBI_F_SAFE_MODE | CBI_F_SAFE_STORAGE
    cmp eax, CBI_F_SAFE_MODE
    je .done
    mov eax, [B(partition_lba)]
    add eax, 9
    mov bx, sector
    call disk_read
    cmp dword [sector], 0x31534243
    jne .done
    mov byte [sector+4], 1
    mov byte [disk_op], 1
    call disk_read
    mov byte [disk_op], 0
.done:
    ret

startup_auto db 0
autoscan_mode db 1
bootstat_valid db 0
startup_choice db '1'
startup_secs db '30'
step_action db 0
align 4, db 0
; Boot flags of choices 1-9 (design 2.2 table); 9 is the fx-selftest boot.
startup_flags dd 0, CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE
    dd CBI_F_SAFE_MODE | CBI_F_SAFE_STORAGE, CBI_F_STEP, CBI_F_CMD_ONLY
    dd CBI_F_SAFE_MODE | CBI_F_SAFE_STORAGE | CBI_F_CMD_ONLY, CBI_F_SAFE_MODE
    dd CBI_F_HW_CHECK | CBI_F_VERBOSE, CBI_F_SELF_TEST
startup_title db 'CiukiOS ', CIUKI_VERSION, ' Startup Menu',13,10
    db '================================',13,10,0
startup_failed db 'CiukiOS did not start successfully. Safe mode is selected.',13,10,0
startup_items db 13,10
    db '  1. Normal',13,10
    db '  2. Logged (boot log on screen)',13,10
    db '  3. Safe mode',13,10
    db '  4. Step-by-step confirmation',13,10
    db '  5. Command prompt only',13,10
    db '  6. Safe mode command prompt only',13,10
    db '  7. Safe mode without disks',13,10
    db '  8. Hardware check',13,10
    db '  9. Self-test (unattended, about 5 minutes)',13,10,0
startup_keys db 'F5=Safe mode  Shift+F5=Command prompt  Shift+F8=Step-by-step confirmation [N]',0
startup_enter db 'Enter a choice: ',0
startup_time db '   Time remaining: ',0
startup_selected db 'L:STARTUP=',0
step_prompt db ' [Enter=Y,Esc=N] ',0
; Action byte, then the question (design 2.2). Class bits are bootstat.h's.
step_table db 0xfe, 'Create a startup log on screen?',0
    db 0x01, 'Load the accelerated display driver?',0
    db 0x02, 'Load the sound drivers?',0
    db 0x04, 'Load the network drivers?',0
    db 0x08, 'Load the USB drivers?',0
    db 0x10, 'Load the serial, parallel, floppy and game ports?',0
    db 0x20, 'Load the power management drivers?',0
    db 0x40, 'Run the startup programs?',0
    db 0xff, 'Start the desktop?',0
    db 0
autoscan_text db 13,10,'CiukiOS was not shut down properly. ScanDisk will check your disks.',13,10
    db 'Press Esc within 10 seconds to skip ScanDisk.',13,10,0
autoscan_skipped db 'ScanDisk skipped.',13,10,0
