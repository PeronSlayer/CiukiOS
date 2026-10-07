; LOADDRV.COM - loads the drivers installed through the Control Panel.
;
; \DRIVERS\DRIVERS.CFG has one driver per line (';' starts a comment):
;
;     E CLASS NAME [@CONDITION...] COMMAND...
;
; E is 1 (enabled) or 0 (disabled). CLASS and NAME are words of up to eight
; characters; the driver's files and DRIVER.INF live in \DRIVERS\CLASS\NAME.
; Lines without conditions run as before. Conditions (all must hold):
;
;   @PCI           a PCI device from the Hardware= list of the driver's own
;                  \DRIVERS\CLASS\NAME\DRIVER.INF is present
;   @PCI=V:D,...   a PCI device from this list (VVVV:DDDD, hex) is present
;   @APM           the BIOS has an APM interface (INT 15h AX=5300h)
;
; A driver whose condition fails is not run ("NAME SKIP"). For @PCI, the
; first listed device found is the driver's device: LOADDRV enables its I/O
; decoding and bus mastering, and the command may use its resources:
;
;   %IO%  %IOH%    first I/O BAR (0xC000 / C000)
;   %IRQ% %IRQH%   interrupt line (11 / B); a device without one is an error
;   %PKT% %PKTH%   a free packet-driver software interrupt (0x60 / 60),
;                  from 60h-66h, 68h-6Fh, 78h-7Eh. When a command uses it,
;                  the driver must answer there ("PKT DRVR" signature and
;                  driver_info); its name and MAC address are reported.
;
; Guards: a driver still running after 20 seconds (with interrupts enabled)
; is terminated. A driver that fails (EXEC error, nonzero exit code, time
; out) has the interrupt vector table and the PIC masks restored to their
; state before it ran, so a half-installed driver cannot take the system
; down. A driver that hangs with interrupts disabled cannot be recovered.
;
; The result of each is written to \DRIVERS\LOADDRV.LOG ("NAME OK ...",
; "NAME SKIP ...", "NAME ERROR ...") and to COM1 ("[LOADDRV] NAME OK ...")
; for the test gates. On a graphics screen (the boot splash) the drivers'
; console output goes to COM1 instead of the screen.
bits 16
cpu 386
org 0x100

CFG_MAX equ 4096
INF_MAX equ 2048
DETAIL_MAX equ 120
WATCHDOG_TICKS equ 364                ; 20 s at 18.2 Hz
RETRY_TICKS equ 36                    ; then every 2 s for nested programs

start:
    jmp init

; ---------------------------------------------------------------------------
; Resident part. If a driver hooked INT 08h or INT 21h after LOADDRV, these
; handlers stay chained under it: LOADDRV then stays resident with only this
; part, both handlers passing everything through.
; ---------------------------------------------------------------------------
wd_int8:
    cmp byte [cs:wd_armed],0
    je .chain
    dec word [cs:wd_ticks]
    jz .fire
.chain:
    jmp far [cs:old_int8]
.fire:
    ; The driver has run too long: terminate the current program (the
    ; driver, or a program it started) from a stack of our own. CiukiDOS
    ; picks its COM or EXE unwind by the caller's CS, so the AH=4Ch call
    ; enters INT 21h with a frame whose CS is the program's PSP (the load
    ; segment of a COM program).
    mov word [cs:wd_ticks],RETRY_TICKS
    mov byte [cs:wd_fired],1
    mov al,0x20
    out 0x20,al
    mov ax,cs
    mov ss,ax
    mov sp,kill_stack_top
    sti
    mov ah,0x62
    int 0x21                          ; BX = the current PSP
    xor ax,ax
    mov ds,ax
    mov eax,[0x21*4]
    mov [cs:kill_vector],eax
    pushf
    push bx
    push word 0x100
    mov ax,0x4CFF
    jmp far [cs:kill_vector]

q_int21:
    cmp byte [cs:q_active],0
    je .chain
    cmp ah,2
    je .char
    cmp ah,9
    je .string
    cmp ah,6
    jne .not_direct
    cmp dl,0xFF
    jne .char
.not_direct:
    cmp ah,0x40
    jne .chain
    cmp bx,1
    je .write
    cmp bx,2
    je .write
.chain:
    jmp far [cs:old_int21]
.char:
    push ax
    mov al,dl
    call com1
    pop ax
    mov al,dl
    iret
.string:
    push si
    mov si,dx
.next:
    lodsb
    cmp al,'$'
    je .string_done
    call com1
    jmp .next
.string_done:
    pop si
    iret
.write:
    push si
    push cx
    mov si,dx
    jcxz .written
.byte:
    lodsb
    call com1
    loop .byte
.written:
    pop cx
    pop si
    mov ax,cx
    push bp
    mov bp,sp
    and byte [bp+6],0xFE                ; CF clear in the caller's flags
    pop bp
    iret

; INT 27h (terminate and stay resident, DX = bytes from the PSP), which
; CiukiDOS does not provide, as INT 21h AH=31h. Old drivers use it. The
; caller's own interrupt frame goes on to INT 21h: CiukiDOS picks its unwind
; by the caller's CS.
q_int27:
    mov ax,dx
    shr ax,4
    test dl,0x0F
    jz .whole
    inc ax
.whole:
    mov dx,ax
    push ds
    xor ax,ax
    mov ds,ax
    mov eax,[0x21*4]
    mov [cs:int27_vector],eax
    pop ds
    mov ax,0x3100
    jmp far [cs:int27_vector]

; AL -> COM1 (bounded wait). Preserves every register.
com1:
    push ax
    push cx
    push dx
    mov ah,al
    mov dx,0x3FD
    mov cx,0x8000
.wait:
    in al,dx
    test al,0x20
    jnz .send
    loop .wait
.send:
    mov dx,0x3F8
    mov al,ah
    out dx,al
    pop dx
    pop cx
    pop ax
    ret

old_int8 dd 0
old_int21 dd 0
old_int27 dd 0
kill_vector dd 0
int27_vector dd 0
wd_ticks dw 0
wd_armed db 0
wd_fired db 0
q_active db 0
resident_end:

; ---------------------------------------------------------------------------
init:
    cld
    ; Keep only this program's own memory: the drivers need the rest.
    mov sp,stack_top
    mov [saved_ss],ss
    mov bx,(image_end-start+0x100+15)/16
    mov ah,0x4A
    int 0x21
    mov ax,cs
    mov [params+4],ax                 ; command tail segment
    mov [params+8],ax                 ; FCB 1 segment
    mov [params+12],ax                ; FCB 2 segment
    ; Read the list.
    mov dx,cfg_path
    mov ax,0x3D00
    int 0x21
    jc .none
    mov bx,ax
    mov dx,cfg
    mov cx,CFG_MAX-1
    mov ah,0x3F
    int 0x21
    pushf
    push ax
    mov ah,0x3E
    int 0x21
    pop cx
    popf
    jc .none
    mov bx,cx
    mov byte [cfg+bx],0
    ; The log starts again at every startup.
    mov dx,log_path
    xor cx,cx
    mov ah,0x3C
    int 0x21
    jc .no_log
    mov [log_handle],ax
.no_log:
    call probe_firmware
    call install_hooks
    mov si,cfg
.line:
    cmp byte [si],0
    je .done
    cmp byte [si],'1'
    jne .skip
    inc si
    call blanks
    mov [class_start],si
    call word_end                     ; the class
    mov [class_end],si
    call blanks
    mov [name_start],si
    call word_end                     ; the name
    mov [name_end],si
    call blanks
    call do_driver
.skip:
    lodsb
    test al,al
    jz .done
    cmp al,10
    jne .skip
    jmp .line
.done:
    mov bx,[log_handle]
    cmp bx,0xFFFF
    je .unhook
    mov ah,0x3E
    int 0x21
.unhook:
    call remove_hooks
    jc .resident
.exit:
    mov ax,0x4C00
    int 0x21
.resident:
    ; Something chained over a hook: keep the pass-through handlers.
    mov si,resident_text
    call serial
    mov dx,(resident_end-start+0x100+15)/16
    mov ax,0x3100
    int 0x21
.none:
    mov si,no_drivers
    call serial
    jmp .exit

; ---------------------------------------------------------------------------
; Firmware probes, once per startup.
probe_firmware:
    pushad
    push es
    mov ax,0xB101                     ; PCI BIOS installation check
    xor edi,edi
    int 0x1A
    jc .no_pci
    cmp edx,0x20494350                ; 'PCI '
    jne .no_pci
    test ah,ah
    jnz .no_pci
    mov byte [have_pci],1
.no_pci:
    mov ax,0x5300                     ; APM installation check
    xor bx,bx
    int 0x15
    jc .no_apm
    cmp bx,0x504D                     ; 'PM'
    jne .no_apm
    mov byte [have_apm],1
.no_apm:
    pop es
    popad
    push cs
    pop ds
    push cs
    pop es
    mov si,start_text
    call serial
    mov al,'0'
    add al,[have_pci]
    call serial_char
    mov si,apm_text
    call serial
    mov al,'0'
    add al,[have_apm]
    call serial_char
    mov si,crlf
    jmp serial

; The watchdog on INT 08h, INT 27h for old TSRs; on a graphics screen, the
; quiet console on INT 21h.
install_hooks:
    push es
    xor ax,ax
    mov es,ax
    cli
    mov eax,[es:8*4]
    mov [old_int8],eax
    mov word [es:8*4],wd_int8
    mov [es:8*4+2],cs
    mov eax,[es:0x27*4]
    mov [old_int27],eax
    mov word [es:0x27*4],q_int27
    mov [es:0x27*4+2],cs
    sti
    call graphics_screen
    jnc .done
    cli
    mov eax,[es:0x21*4]
    mov [old_int21],eax
    mov word [es:0x21*4],q_int21
    mov [es:0x21*4+2],cs
    mov byte [q_active],1
    mov byte [q_hooked],1
    sti
.done:
    pop es
    ret

; CF set on a VESA mode or a graphics BIOS mode.
graphics_screen:
    push es
    mov ax,0x4F03
    int 0x10
    cmp ax,0x004F
    jne .bda
    and bx,0x3FFF
    cmp bx,0x100
    jae .graphics
.bda:
    push 0x40
    pop es
    mov al,[es:0x49]
    cmp al,3
    jbe .text
    cmp al,7
    je .text
.graphics:
    pop es
    stc
    ret
.text:
    pop es
    clc
    ret

; Removes the hooks; CF set when one of them is chained under another program.
remove_hooks:
    push es
    xor ax,ax
    mov es,ax
    mov byte [wd_armed],0
    mov byte [q_active],0
    xor dx,dx
    cli
    cmp word [es:8*4],wd_int8
    jne .int8_kept
    mov ax,cs
    cmp [es:8*4+2],ax
    jne .int8_kept
    mov eax,[old_int8]
    mov [es:8*4],eax
    jmp .int27
.int8_kept:
    inc dx
.int27:
    cmp word [es:0x27*4],q_int27
    jne .int27_kept
    mov ax,cs
    cmp [es:0x27*4+2],ax
    jne .int27_kept
    mov eax,[old_int27]
    mov [es:0x27*4],eax
    jmp .int21
.int27_kept:
    inc dx
.int21:
    cmp byte [q_hooked],0
    je .done
    cmp word [es:0x21*4],q_int21
    jne .int21_kept
    mov ax,cs
    cmp [es:0x21*4+2],ax
    jne .int21_kept
    mov eax,[old_int21]
    mov [es:0x21*4],eax
    jmp .done
.int21_kept:
    inc dx
.done:
    sti
    pop es
    cmp dx,1                          ; CF = (dx == 0) inverted below
    cmc
    ret

; ---------------------------------------------------------------------------
; SI = after the name. Evaluates the conditions, runs the command and
; reports. SI stays on the line.
do_driver:
    push si
    mov byte [detail],0
    mov word [detail_end],detail
    mov byte [want_pci],0
    mov byte [dev_found],0
    mov byte [pkt_int],0
    mov word [pci_list],0
.cond:
    call blanks
    cmp byte [si],'@'
    jne .conditions_done
    mov di,cond_pci
    call match_keyword
    jc .not_pci
    mov byte [want_pci],1
    cmp byte [si],'='
    jne .cond
    inc si
    mov [pci_list],si
    call word_end
    jmp .cond
.not_pci:
    mov di,cond_apm
    call match_keyword
    jc .bad_condition
    cmp byte [have_apm],1
    je .cond
    mov si,skip_text
    mov bx,no_apm_detail
    jmp .report
.bad_condition:
    mov si,error_text
    mov bx,bad_condition_detail
    jmp .report
.conditions_done:
    mov [command_start],si
    cmp byte [want_pci],0
    je .run
    cmp word [pci_list],0
    jne .have_list
    call inf_hardware
    jnc .have_list
    mov si,error_text
    mov bx,no_inf_detail
    jmp .report
.have_list:
    call find_pci
    jnc .found
    mov si,skip_text
    mov bx,no_hardware_detail
    jmp .report
.found:
    call device_resources
.run:
    mov si,[command_start]
    call build_command
    jnc .exec
    mov si,error_text                 ; BX = the reason
    jmp .report
.exec:
    call run_command                  ; SI = result text, BX = detail or 0
    pushf
    call video_hook_evidence
    popf
    jc .report
    cmp byte [pkt_int],0
    je .report
    call verify_packet_driver         ; may turn OK into ERROR
.report:
    test bx,bx
    jz .no_detail
    push si
    mov si,bx
    call detail_text
    pop si
.no_detail:
    call report
    pop si
    ret

; SI = "@WORD..." at the line; DI = keyword (zero terminated, upper case,
; without '@'). CF clear and SI after the keyword when it matches (followed
; by a blank, '=' or the end of the line).
match_keyword:
    push si
    inc si
.next:
    mov al,[di]
    test al,al
    jz .end
    mov ah,[si]
    cmp ah,'a'
    jb .cmp
    cmp ah,'z'
    ja .cmp
    sub ah,0x20
.cmp:
    cmp al,ah
    jne .no
    inc si
    inc di
    jmp .next
.end:
    cmp byte [si],'='
    je .yes
    cmp byte [si],' '
    jbe .yes
.no:
    pop si
    stc
    ret
.yes:
    add sp,2
    clc
    ret

; Record actual INT10 installation separately from the child's exit status.
; The pre-EXEC IVT copy already belongs to the watchdog's restore contract.
; An unchanged vector is an observation, not an invented driver-load error.
video_hook_evidence:
    pushad
    push es
    mov si,[class_start]
    mov ax,[class_end]
    sub ax,si
    cmp ax,5
    jne .done
    cmp dword [si],'VIDE'
    jne .done
    cmp byte [si+4],'O'
    jne .done
    xor ax,ax
    mov es,ax
    mov ebx,[es:0x10*4]
    push cs
    pop es
    mov edx,[ivt_copy+0x10*4]
    mov si,video_int_detail
    call detail_text
    mov eax,edx
    call .pointer
    mov si,video_arrow_detail
    call detail_text
    mov eax,ebx
    call .pointer
    mov si,video_unchanged_detail
    cmp ebx,edx
    je .text
    mov si,video_hooked_detail
.text:
    call detail_text
.done:
    pop es
    popad
    ret
.pointer:
    push eax
    shr eax,16
    call detail_hex4
    mov al,':'
    call detail_char
    pop eax
    jmp detail_hex4

; Reads \DRIVERS\CLASS\NAME\DRIVER.INF; [pci_list] = its Hardware= value.
; CF set when the file or the key is missing.
inf_hardware:
    push si
    mov di,path
    mov si,drivers_dir
    call copy_z
    mov si,[class_start]
    mov cx,[class_end]
    call copy_n
    mov al,'\'
    stosb
    mov si,[name_start]
    mov cx,[name_end]
    call copy_n
    mov si,inf_name
    call copy_z
    mov byte [di],0
    mov dx,path
    mov ax,0x3D00
    int 0x21
    jc .fail
    mov bx,ax
    mov dx,inf
    mov cx,INF_MAX-1
    mov ah,0x3F
    int 0x21
    pushf
    push ax
    mov ah,0x3E
    int 0x21
    pop cx
    popf
    jc .fail
    mov bx,cx
    mov byte [inf+bx],0
    mov si,inf
.line:
    call blanks
    mov di,hardware_key
    push si
.key:
    mov al,[di]
    test al,al
    jz .found
    mov ah,[si]
    cmp ah,'a'
    jb .cmp
    cmp ah,'z'
    ja .cmp
    sub ah,0x20
.cmp:
    cmp al,ah
    jne .other
    inc si
    inc di
    jmp .key
.found:
    add sp,2
    mov [pci_list],si
    pop si
    clc
    ret
.other:
    pop si
.eol:
    lodsb
    test al,al
    jz .fail
    cmp al,10
    jne .eol
    jmp .line
.fail:
    pop si
    stc
    ret

; Looks for the devices of [pci_list] (VVVV:DDDD separated by commas or
; blanks, up to the end of the line), in list order. CF clear when found:
; pci_bus, pci_devfn, pci_vendor, pci_device.
find_pci:
    cmp byte [have_pci],1
    jne .none
    mov si,[pci_list]
.next:
    lodsb
    cmp al,','
    je .next
    cmp al,' '
    je .next
    cmp al,9
    je .next
    dec si
    cmp al,' '
    jb .none                          ; end of the line
    call hex_word
    jc .skip
    mov dx,ax
    cmp byte [si],':'
    jne .skip
    inc si
    call hex_word
    jc .skip
    mov cx,ax
    push si
    pushad
    mov ax,0xB102
    xor si,si
    int 0x1A
    mov [cs:pci_bus],bh
    mov [cs:pci_devfn],bl
    mov [cs:pci_status],ah
    popad
    pop si
    jc .next
    cmp byte [pci_status],0
    jne .next
    mov [pci_vendor],dx
    mov [pci_device],cx
    mov byte [dev_found],1
    clc
    ret
.skip:
    ; Not an ID: skip the word.
    lodsb
    cmp al,' '
    jbe .next_word
    cmp al,','
    jne .skip
.next_word:
    dec si
    jmp .next
.none:
    stc
    ret

; SI = four hex digits -> AX (SI after them). CF set if they are not.
hex_word:
    push cx
    push dx
    xor dx,dx
    mov cx,4
.digit:
    mov al,[si]
    call hex_value
    jc .bad
    shl dx,4
    or dl,al
    inc si
    loop .digit
    mov ax,dx
    pop dx
    pop cx
    clc
    ret
.bad:
    pop dx
    pop cx
    stc
    ret

; AL = a hex digit -> its value. CF set if it is not one.
hex_value:
    cmp al,'0'
    jb .bad
    cmp al,'9'
    jbe .digit
    or al,0x20
    cmp al,'a'
    jb .bad
    cmp al,'f'
    ja .bad
    sub al,'a'-10
    clc
    ret
.digit:
    sub al,'0'
    clc
    ret
.bad:
    stc
    ret

; The found device: first I/O BAR, interrupt line; enables I/O decoding and
; bus mastering. Adds "PCI VVVV:DDDD" to the detail.
device_resources:
    pushad
    mov word [dev_io],0
    mov di,0x10
.bar:
    mov bh,[pci_bus]
    mov bl,[pci_devfn]
    mov ax,0xB10A
    push di
    int 0x1A
    pop di
    jc .next_bar
    test cl,1
    jz .next_bar
    and cx,0xFFFC
    jz .next_bar
    mov [dev_io],cx
    jmp .irq
.next_bar:
    add di,4
    cmp di,0x24
    jbe .bar
.irq:
    mov bh,[pci_bus]
    mov bl,[pci_devfn]
    mov ax,0xB108
    mov di,0x3C
    int 0x1A
    jnc .irq_read
    mov cl,0xFF
.irq_read:
    mov [dev_irq],cl
    mov bh,[pci_bus]
    mov bl,[pci_devfn]
    mov ax,0xB109
    mov di,0x04
    int 0x1A
    jc .detail
    or cx,0x0005                      ; I/O space + bus master
    mov bh,[pci_bus]
    mov bl,[pci_devfn]
    mov ax,0xB10C
    mov di,0x04
    int 0x1A
.detail:
    popad
    mov si,pci_detail
    call detail_text
    mov ax,[pci_vendor]
    call detail_hex4
    mov al,':'
    call detail_char
    mov ax,[pci_device]
    call detail_hex4
    mov ax,[dev_io]
    test ax,ax
    jz .irq_detail
    mov si,io_detail
    call detail_text
    mov ax,[dev_io]
    call detail_0x
    call detail_hex4
.irq_detail:
    mov al,[dev_irq]
    test al,al
    jz .done
    cmp al,15
    ja .done
    mov si,irq_detail
    call detail_text
    mov al,[dev_irq]
    call detail_hex2
.done:
    ret

; SI = the command (up to the end of the line). Builds 'program' and 'tail'
; with the placeholders replaced. CF set with BX = the reason on error.
build_command:
    mov di,program
    mov cx,79
.path:
    lodsb
    cmp al,' '
    jbe .path_end
    stosb
    loop .path
.path_end:
    mov byte [di],0
    dec si
    cmp di,program
    jne .tail_start
    mov bx,no_command_detail
    stc
    ret
.tail_start:
    mov di,tail+1
.tail:
    lodsb
    cmp al,13
    je .tail_end
    cmp al,10
    je .tail_end
    test al,al
    je .tail_end
    cmp al,'%'
    je .placeholder
.store:
    cmp di,tail+126
    jae .tail
    stosb
    jmp .tail
.placeholder:
    push si
    mov bx,placeholders
.try:
    mov dx,[bx]                       ; the name (after '%')
    test dx,dx
    jz .literal
    push si
    push di
    mov di,dx
.cmp:
    mov al,[di]
    cmp al,[si]
    jne .differ
    inc di
    inc si
    cmp al,'%'
    jne .cmp
    pop di
    add sp,2                          ; the name matched: keep SI after it
    add sp,2                          ; drop the saved '%' position
    call [bx+2]                       ; appends at DI; CF + BX on error
    jc .fail
    jmp .tail
.differ:
    pop di
    pop si
    add bx,4
    jmp .try
.literal:
    pop si
    mov al,'%'
    jmp .store
.tail_end:
    mov byte [di],13
    mov ax,di
    sub ax,tail+1
    mov [tail],al
    clc
    ret
.fail:
    ret

; Placeholder writers: append at DI (bounded by the tail); CF + BX on error.
put_io:
    mov ax,[dev_io]
    test ax,ax
    jz no_io
    call put_0x
    jmp put_hex4
put_ioh:
    mov ax,[dev_io]
    test ax,ax
    jz no_io
    jmp put_hex4
put_irq:
    call irq_value
    jc .ret
    xor ah,ah
    cmp al,10
    jb .one
    push ax
    mov al,'1'
    call put_char
    pop ax
    sub al,10
.one:
    add al,'0'
    call put_char
    clc
.ret:
    ret
put_irqh:
    call irq_value
    jc .ret
    call hex_digit
    call put_char
    clc
.ret:
    ret
put_pkt:
    call packet_int
    jc .ret
    call put_0x
    call put_hex2
    clc
.ret:
    ret
put_pkth:
    call packet_int
    jc .ret
    call put_hex2
    clc
.ret:
    ret

no_io:
    mov bx,no_io_detail
    stc
    ret

; AL = the device's IRQ (1..15). CF + BX when it has none.
irq_value:
    mov al,[dev_irq]
    cmp byte [dev_found],1
    jne .none
    test al,al
    jz .none
    cmp al,15
    ja .none
    clc
    ret
.none:
    mov bx,no_irq_detail
    stc
    ret

; AL = the line's packet interrupt, allocated on first use: the first
; candidate that is free (null, or pointing at a bare IRET, as CiukiDOS
; initializes 60h-67h). It is set to null for the driver (some refuse a
; non-null vector); a failed driver gets the old vector back.
packet_int:
    mov al,[pkt_int]
    test al,al
    jnz .ok
    push si
    push di
    push es
    mov si,pkt_candidates
.next:
    lodsb
    test al,al
    jz .none
    xor bx,bx
    mov bl,al
    shl bx,2
    xor di,di
    mov es,di
    les di,[es:bx]
    mov cx,es
    or cx,di
    jz .free
    cmp byte [es:di],0xCF             ; IRET
    jne .next
.free:
    mov [pkt_int],al
    xor di,di
    mov es,di
    cli
    mov [es:bx],di
    mov [es:bx+2],di
    sti
    pop es
    pop di
    pop si
.ok:
    clc
    ret
.none:
    pop es
    pop di
    pop si
    mov bx,no_pkt_detail
    stc
    ret

; AL = interrupt. CF clear if a packet driver answers there (its vector
; has "PKT DRVR" after the first three bytes). Preserves AX, SI, DI.
is_packet_driver:
    push ax
    push cx
    push si
    push di
    push ds
    push es
    xor ah,ah
    shl ax,2
    mov si,ax
    xor ax,ax
    mov ds,ax
    lds si,[si]
    add si,3
    push cs
    pop es
    mov di,pkt_signature
    mov cx,8
    repe cmpsb
    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop ax
    je .yes
    stc
    ret
.yes:
    clc
    ret

put_0x:
    push ax
    mov al,'0'
    call put_char
    mov al,'x'
    call put_char
    pop ax
    ret
put_hex4:
    push ax
    mov al,ah
    call put_hex2
    pop ax
put_hex2:
    push ax
    shr al,4
    call hex_digit
    call put_char
    pop ax
    and al,0x0F
    call hex_digit
    jmp put_char
hex_digit:
    and al,0x0F
    add al,'0'
    cmp al,'9'
    jbe .done
    add al,7
.done:
    ret
put_char:
    cmp di,tail+126
    jae .full
    stosb
.full:
    clc
    ret

; ---------------------------------------------------------------------------
; Runs 'program' with 'tail' under the watchdog. SI = result text, BX = the
; detail (0 for none); CF set when the driver failed.
run_command:
    ; What a failed driver must not leave behind.
    push ds
    push es
    xor si,si
    mov ds,si
    push cs
    pop es
    mov di,ivt_copy
    mov cx,512
    cli
    rep movsw
    sti
    pop es
    pop ds
    in al,0x21
    mov [pic_masks],al
    in al,0xA1
    mov [pic_masks+1],al
    mov word [params],0               ; the environment of LOADDRV
    mov word [params+2],tail
    mov word [params+6],fcb
    mov word [params+10],fcb
    mov byte [wd_fired],0
    mov word [wd_ticks],WATCHDOG_TICKS
    mov dx,program
    mov bx,params
    mov [saved_sp],sp
    mov byte [wd_armed],1
    mov ax,0x4B00
    int 0x21
    mov byte [cs:wd_armed],0
    cli                               ; EXEC may not keep SS:SP
    mov ss,[cs:saved_ss]
    mov sp,[cs:saved_sp]
    sti
    push cs
    pop ds
    push cs
    pop es
    cld
    jnc .ran
    mov si,error_text
    mov bx,cannot_run_detail
    jmp .failed
.ran:
    mov ah,0x4D
    int 0x21
    cmp byte [wd_fired],0
    je .no_timeout
    mov si,error_text
    mov bx,timeout_detail
    jmp .failed
.no_timeout:
    cmp ah,3                          ; resident
    je .ok
    test ah,ah
    jnz .abnormal
    test al,al
    jz .ok
    push ax
    mov si,exit_detail
    call detail_text
    pop ax
    call detail_hex2
    mov si,error_text
    xor bx,bx
    jmp .failed
.abnormal:
    mov si,error_text
    mov bx,abnormal_detail
    jmp .failed
.ok:
    mov si,ok_text
    xor bx,bx
    clc
    ret
.failed:
    call restore_state
    stc
    ret

restore_state:
    push si
    push es
    xor di,di
    mov es,di
    mov si,ivt_copy
    mov cx,512
    cli
    rep movsw
    mov al,[pic_masks]
    out 0x21,al
    mov al,[pic_masks+1]
    out 0xA1,al
    sti
    pop es
    pop si
    ret

; After a driver that was given %PKT%: it must answer there. Adds its name
; and MAC address to the detail. SI/BX = result, as run_command.
verify_packet_driver:
    mov byte [pkt_name],0
    push si
    mov si,int_detail
    call detail_text
    mov al,[pkt_int]
    call detail_0x
    call detail_hex2
    pop si
    mov al,[pkt_int]
    call is_packet_driver
    jnc .present
    mov si,error_text
    mov bx,no_packet_detail
    ret
.present:
    push si
    xor bx,bx
    mov bl,[pkt_int]
    shl bx,2
    push es
    xor ax,ax
    mov es,ax
    mov eax,[es:bx]
    pop es
    mov [pkt_vector],eax
    ; driver_info: the name and class.
    push ds
    mov ax,0x01FF
    xor bx,bx
    pushf
    cli
    call far [cs:pkt_vector]
    jc .info_failed
    mov [cs:pkt_class],ch
    mov di,pkt_name
    push cs
    pop es
    mov cx,15
.name:
    lodsb
    test al,al
    jz .name_end
    stosb
    loop .name
.name_end:
    mov byte [es:di],0
.info_failed:
    pop ds
    push cs
    pop es
    cld
    cmp byte [pkt_name],0
    je .address
    mov al,' '
    call detail_char
    mov si,pkt_name
    call detail_text
.address:
    ; access_type (every packet type), get_address, release_type.
    mov ah,0x02
    mov al,[pkt_class]
    mov bx,0xFFFF
    xor dl,dl
    mov si,pkt_signature              ; not read: the type length is 0
    xor cx,cx
    mov di,pkt_receiver
    pushf
    cli
    call far [pkt_vector]
    push cs
    pop ds
    push cs
    pop es
    cld
    jc .done
    mov [pkt_handle],ax
    mov bx,ax
    mov ah,0x06
    mov di,pkt_mac
    mov cx,6
    pushf
    cli
    call far [pkt_vector]
    push cs
    pop ds
    push cs
    pop es
    cld
    jc .release
    cmp cx,6
    jne .release
    mov si,mac_detail
    call detail_text
    mov si,pkt_mac
    mov cx,6
.mac:
    lodsb
    call detail_hex2
    cmp cx,1
    je .mac_next
    mov al,':'
    call detail_char
.mac_next:
    loop .mac
.release:
    mov ah,0x03
    mov bx,[pkt_handle]
    pushf
    cli
    call far [pkt_vector]
    push cs
    pop ds
    push cs
    pop es
    cld
.done:
    sti
    pop si
    xor bx,bx
    ret

; The receiver of the short-lived handle: no buffer, the packet is dropped.
pkt_receiver:
    xor di,di
    mov es,di
    retf

; ---------------------------------------------------------------------------
; Detail text (appended to the result).
detail_text:
    push di
    mov di,[detail_end]
.next:
    lodsb
    test al,al
    jz .done
    cmp di,detail+DETAIL_MAX
    jae .next
    stosb
    jmp .next
.done:
    mov byte [di],0
    mov [detail_end],di
    pop di
    ret
detail_char:
    push di
    mov di,[detail_end]
    cmp di,detail+DETAIL_MAX
    jae .full
    stosb
    mov byte [di],0
    mov [detail_end],di
.full:
    pop di
    ret
detail_0x:
    push ax
    mov al,'0'
    call detail_char
    mov al,'x'
    call detail_char
    pop ax
    ret
detail_hex4:
    push ax
    mov al,ah
    call detail_hex2
    pop ax
detail_hex2:
    push ax
    shr al,4
    call hex_digit
    call detail_char
    pop ax
    push ax
    call hex_digit
    call detail_char
    pop ax
    ret

; SI = " OK", " SKIP" or " ERROR": NAME, the result and the detail to the log
; and COM1.
report:
    mov [result],si
    mov si,loaddrv_tag
    call serial
    mov si,[name_start]
    mov cx,[name_end]
    sub cx,si
    jz .text
    push cx
.name:
    lodsb
    call serial_char
    loop .name
    pop cx
    mov dx,[name_start]
    call log_write
.text:
    mov si,[result]
    call serial
    mov si,detail
    call serial
    mov si,crlf
    call serial
    mov si,[result]
    call log_z
    mov si,detail
    call log_z
    mov dx,crlf
    mov cx,2
    jmp log_write

; SI = zero-terminated text to the log.
log_z:
    mov dx,si
    xor cx,cx
.length:
    lodsb
    test al,al
    jz log_write
    inc cx
    jmp .length

; DX, CX bytes to the log (when it could be made).
log_write:
    mov bx,[log_handle]
    cmp bx,0xFFFF
    je .done
    jcxz .done
    mov ah,0x40
    int 0x21
.done:
    ret

serial:
    lodsb
    test al,al
    jz .done
    call serial_char
    jmp serial
.done:
    ret

serial_char:
    jmp com1

blanks:
    cmp byte [si],' '
    je .next
    cmp byte [si],9
    jne .done
.next:
    inc si
    jmp blanks
.done:
    ret

word_end:
    cmp byte [si],' '
    jbe .done
    inc si
    jmp word_end
.done:
    ret

; SI = zero-terminated text -> DI.
copy_z:
    lodsb
    test al,al
    jz .done
    stosb
    jmp copy_z
.done:
    ret

; SI up to CX (an end pointer) -> DI.
copy_n:
    cmp si,cx
    jae .done
    movsb
    jmp copy_n
.done:
    ret

; ---------------------------------------------------------------------------
cfg_path db '\DRIVERS\DRIVERS.CFG',0
log_path db '\DRIVERS\LOADDRV.LOG',0
drivers_dir db '\DRIVERS\',0
inf_name db '\DRIVER.INF',0
hardware_key db 'HARDWARE=',0
cond_pci db 'PCI',0
cond_apm db 'APM',0
loaddrv_tag db '[LOADDRV] ',0
start_text db '[LOADDRV] start PCI=',0
apm_text db ' APM=',0
resident_text db '[LOADDRV] hooks chained; pass-through resident',13,10,0
ok_text db ' OK',0
skip_text db ' SKIP',0
error_text db ' ERROR',0
crlf db 13,10,0
no_drivers db '[LOADDRV] no drivers',13,10,0
no_hardware_detail db ' no matching hardware',0
no_apm_detail db ' no APM BIOS',0
bad_condition_detail db ' unknown condition',0
no_inf_detail db ' DRIVER.INF has no Hardware list',0
no_command_detail db ' no command',0
no_io_detail db ' device has no I/O port',0
no_irq_detail db ' device has no IRQ',0
no_pkt_detail db ' no free packet interrupt',0
cannot_run_detail db ' cannot run the program',0
timeout_detail db ' timed out; terminated',0
exit_detail db ' exit code 0x',0
abnormal_detail db ' abnormal termination',0
no_packet_detail db ' no packet driver answered',0
pci_detail db ' PCI ',0
int_detail db ' INT ',0
io_detail db ' IO ',0
irq_detail db ' IRQ 0x',0
video_int_detail db ' INT10 ',0
video_arrow_detail db '->',0
video_hooked_detail db ' hooked',0
video_unchanged_detail db ' unchanged',0
mac_detail db ' MAC ',0
pkt_signature db 'PKT DRVR'
pkt_candidates db 0x60,0x61,0x62,0x63,0x64,0x65,0x66
               db 0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F
               db 0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,0
; Placeholders: the name (with the closing '%') and its writer. The longer
; names first: %IOH% before %IO%.
placeholders dw ph_ioh,put_ioh, ph_io,put_io, ph_irqh,put_irqh, ph_irq,put_irq
             dw ph_pkth,put_pkth, ph_pkt,put_pkt, 0
ph_ioh db 'IOH%'
ph_io db 'IO%'
ph_irqh db 'IRQH%'
ph_irq db 'IRQ%'
ph_pkth db 'PKTH%'
ph_pkt db 'PKT%'

log_handle dw 0xFFFF
saved_ss dw 0
saved_sp dw 0
result dw 0
class_start dw 0
class_end dw 0
name_start dw 0
name_end dw 0
command_start dw 0
pci_list dw 0
detail_end dw 0
pci_vendor dw 0
pci_device dw 0
dev_io dw 0
pkt_handle dw 0
pkt_vector dd 0
have_pci db 0
have_apm db 0
q_hooked db 0
want_pci db 0
dev_found db 0
pci_bus db 0
pci_devfn db 0
pci_status db 0
dev_irq db 0
pkt_int db 0
pkt_class db 1
pic_masks db 0,0
params dw 0,0,0,0,0,0,0
fcb db 0,'           ',0,0,0,0
file_end:

; Uninitialized memory after the file (kept by the AH=4Ah resize).
absolute file_end
pkt_mac resb 6
pkt_name resb 16
detail resb DETAIL_MAX+2
tail resb 128
program resb 80
path resb 96
alignb 16
ivt_copy resb 1024
cfg resb CFG_MAX
inf resb INF_MAX
alignb 16
    resb 256
kill_stack_top:
    resb 1024
stack_top:
image_end:
