bits 16
cpu 386
%ifdef UI_SFX_DRIVER
org 0
%define BOOT_SOUND 1
%else
org 0x0100
%endif

; Intel ICH-family AC'97 polling playback probe.  It uses PCI BIOS for
; discovery/resource assignment and the standard ICH native audio bus-master
; interface, so no chipset address or IRQ is assumed.

%define PCI_BIOS_PRESENT       0xB101
%define PCI_FIND_DEVICE        0xB102
%define PCI_READ_CONFIG_WORD   0xB109
%define PCI_READ_CONFIG_DWORD  0xB10A
%define PCI_WRITE_CONFIG_WORD  0xB10C
%define PCI_VENDOR_INTEL       0x8086

%define PCI_COMMAND            0x04
%define PCI_NAMBAR             0x10
%define PCI_NABMBAR            0x14
%define PCI_SUBSYSTEM_VENDOR   0x2C

%define AC97_RESET             0x00
%define AC97_MASTER_VOL        0x02
%define AC97_HEADPHONE_VOL     0x04
%define AC97_PCM_OUT_VOL       0x18
%define AC97_POWERDOWN         0x26
%define AC97_EXTENDED_ID       0x28
%define AC97_EXTENDED_STATUS   0x2A
%define AC97_CSR_ACMODE        0x5E
%define AC97_VENDOR_ID1        0x7C
%define AC97_VENDOR_ID2        0x7E

%define EC_DATA                0x62
%define EC_STATUS_COMMAND      0x66
%define EC_STATUS_OBF          0x01
%define EC_STATUS_IBF          0x02
%define EC_CMD_READ            0x80
%define EC_CMD_WRITE           0x81
%define TP_EC_AUDIO            0x30
%define TP_EC_AUDIO_MUTE       0x40
%define TP_EC_AUDIO_LEVEL_MASK 0x0F
%define TP_EC_AUDIO_LEVEL_MAX  14

%define ICH_PO_BDBAR           0x10
%define ICH_PO_LVI             0x15
%define ICH_PO_SR              0x16
%define ICH_PO_CR              0x1B
%define ICH_PI_CR              0x0B
%define ICH_MC_CR              0x2B
%define ICH_GLOB_CNT           0x2C
%define ICH_GLOB_STA           0x30
%define ICH_CAS                0x34
%define ICH_SR_ERROR           0x10
%define ICH_SR_DCH             0x01
%define ICH_CR_RESET           0x02
%define ICH_CR_RUN             0x01

; Diagnostic output is explicitly requested with /D. Preserve the complete
; caller state, including CF returned by firmware, around each observation.
%macro AUDIO_TRACE 1
    pushf
    push dx
    mov dx, %1
    call audio_trace
    pop dx
    popf
%endmacro

start:
%ifdef UI_SFX_DRIVER
    jmp near sfx_entry
    db 'CSFX0001'
    db 0
    dw sfx_image_end,1
    times 32-($-$$) db 0
%else
%ifdef BOOT_SOUND
    jmp boot_sound_start
%endif
%endif
%ifndef UI_SFX_DRIVER
    cld
    push cs
    pop ds
    push cs
    pop es
    call parse_quiet_switch

    mov dx, msg_begin
    call print_line
    call pci_find_intel_ac97
    jc .not_found
    call ac97_enable_io
    jc .failed
    call ac97_all_streams_idle
    jc .failed
    call ac97_enable_pci
    jc .failed
    call ac97_wait_codec
    jc .failed
    call ac97_prepare_codec
    jc .failed
    call thinkpad_audio_unmute
    call print_hardware_report
    call ac97_play_sample
    jc .failed
    call ac97_cleanup_output
    jc .report_failed

    mov dx, msg_done
    call print_line
    mov ax, 0x4C00
    int 0x21

.not_found:
    mov dx, msg_not_found
    call print_line
    mov ax, 0x4C01
    int 0x21
.failed:
    call ac97_cleanup_output
.report_failed:
    mov dx, msg_failed
    call print_line
    mov ax, 0x4C02
    int 0x21

%endif
pci_find_intel_ac97:
    AUDIO_TRACE audio_diag_pci_check
    mov ax, PCI_BIOS_PRESENT
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jc .fail
    mov si, intel_ac97_ids
.next_id:
    lodsw
    or ax, ax
    jz .fail
    mov [pci_device_id], ax
    mov [pci_id_next], si
    mov cx, ax
    mov dx, PCI_VENDOR_INTEL
    xor si, si
    mov ax, PCI_FIND_DEVICE
    AUDIO_TRACE audio_diag_pci_find
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jnc .found
    ; PCI BIOS preserves neither SI nor DS by contract on all machines.
    mov si, [pci_id_next]
    jmp .next_id
.found:
    mov [pci_bdf], bx

    mov ax, PCI_READ_CONFIG_DWORD
    mov di, PCI_NAMBAR
    AUDIO_TRACE audio_diag_pci_bar
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jc .fail
    test cl, 1
    jz .fail
    test ecx, 0xFFFF0000
    jnz .fail
    and cx, 0xFFFC
    jz .fail
    mov [nam_base], cx

    mov bx, [pci_bdf]
    mov ax, PCI_READ_CONFIG_DWORD
    mov di, PCI_NABMBAR
    AUDIO_TRACE audio_diag_pci_bar
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jc .fail
    test cl, 1
    jz .fail
    test ecx, 0xFFFF0000
    jnz .fail
    and cx, 0xFFFC
    jz .fail
    mov [nabm_base], cx

    ; This optional word lets the driver identify IBM without scanning ROM
    ; memory.  Failure is non-fatal because the generic ICH path remains valid.
    mov bx, [pci_bdf]
    mov ax, PCI_READ_CONFIG_WORD
    mov di, PCI_SUBSYSTEM_VENDOR
    AUDIO_TRACE audio_diag_pci_vendor
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jc .no_subsystem
    mov [pci_subsystem_vendor], cx
    jmp .resources_ready
.no_subsystem:
    mov word [pci_subsystem_vendor], 0
.resources_ready:
    clc
    ret
.fail:
    stc
    ret

ac97_enable_io:
    ; Inspect the DMA control register only after enabling its I/O decoder.
    ; Do not enable bus mastering here: stale firmware descriptors must not
    ; start running merely because SOUND wants to check whether output is busy.
    mov bx, [pci_bdf]
    mov ax, PCI_READ_CONFIG_WORD
    mov di, PCI_COMMAND
    AUDIO_TRACE audio_diag_pci_command_read
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jc .fail
    mov [boot_pci_command], cx
    mov byte [boot_pci_saved], 1
    or cx, 0x0001
    mov bx, [pci_bdf]
    mov di, PCI_COMMAND
    mov ax, PCI_WRITE_CONFIG_WORD
    AUDIO_TRACE audio_diag_pci_command_write
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
.fail:
    ret

; CF=busy/unavailable. PCI command bit 2 is global to PI/PO/MC, so neither
; diagnostic player may enable it while any pre-existing stream has RUN set.
ac97_all_streams_idle:
    push ax
    push dx
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    in al,dx
    test al,ICH_CR_RUN
    jnz .busy
    mov dx,[nabm_base]
    add dx,ICH_PI_CR
    in al,dx
    test al,ICH_CR_RUN
    jnz .busy
    mov dx,[nabm_base]
    add dx,ICH_MC_CR
    in al,dx
    test al,ICH_CR_RUN
    jnz .busy
    clc
    jmp .out
.busy:
    stc
.out:
    pop dx
    pop ax
    ret

ac97_enable_pci:
    mov bx, [pci_bdf]
    mov ax, PCI_READ_CONFIG_WORD
    mov di, PCI_COMMAND
    AUDIO_TRACE audio_diag_pci_command_read
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jc .fail
    cmp byte [boot_pci_saved], 1
    je .command_saved
    mov [boot_pci_command], cx
    mov byte [boot_pci_saved], 1
.command_saved:
    ; Recheck immediately before BME, after any potentially long PCM load.
    call ac97_all_streams_idle
    jc .fail
    or cx, 0x0005                 ; I/O space + bus mastering
    mov bx, [pci_bdf]
    mov di, PCI_COMMAND
    mov ax, PCI_WRITE_CONFIG_WORD
    AUDIO_TRACE audio_diag_pci_command_write
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jc .fail
    mov byte [ac97_output_owned],1
    clc
    ret
.fail:
    stc
    ret

; Stop only the output this player claimed. DCH is the hardware halt proof;
; then reset the channel before detaching its BDL (ICH3 13.2.4 / 13.2.7).
; All-ones reads are absent hardware, never successful halt/reset evidence.
ac97_quiesce_output:
    push ax
    push cx
    push dx
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    xor al,al
    out dx,al
    mov dx,[nabm_base]
    add dx,ICH_PO_SR
    mov cx,0xFFFF
.halt:
    in ax,dx
    cmp ax,0xFFFF
    je .fail
    test ax,ICH_SR_DCH
    jnz .reset
    in al,0x80
    loop .halt
    jmp .fail
.reset:
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    in al,dx
    cmp al,0xFF
    je .fail
    test al,ICH_CR_RUN
    jnz .fail
    mov al,ICH_CR_RESET
    out dx,al
    mov cx,0xFFFF
.reset_wait:
    in al,dx
    cmp al,0xFF
    je .fail
    test al,ICH_CR_RESET | ICH_CR_RUN
    jz .ready
    in al,0x80
    loop .reset_wait
.fail:
    stc
    jmp .out
.ready:
    clc
.out:
    pop dx
    pop cx
    pop ax
    ret

; Confirm global DMA is disabled even when firmware rejects the write.
; Readback, rather than the BIOS return status alone, is the safety proof.
ac97_disable_bme_checked:
    mov bx,[pci_bdf]
    mov di,PCI_COMMAND
    mov cx,[boot_pci_command]
    or cx,1
    and cx,~4
    mov ax,PCI_WRITE_CONFIG_WORD
    stc
    int 0x1A
    push cs
    pop ds
    ; Fall through regardless of write CF: it may already have been off.
ac97_check_bme_off:
    mov bx,[pci_bdf]
    mov di,PCI_COMMAND
    mov ax,PCI_READ_CONFIG_WORD
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jc .fail
    test cx,4
    jnz .fail
    clc
    ret
.fail:
    stc
    ret

ac97_cleanup_output:
    cmp byte [ac97_output_owned],1
    jne .restore
    call ac97_quiesce_output
    jnc .detached
    mov byte [ac97_dma_unquiesced],1
    call ac97_disable_bme_checked
    jc ac97_dma_fatal
.detached:
    ; DMA is either halted/reset or independently blocked by verified BME=0.
    mov dx,[nabm_base]
    add dx,ICH_PO_BDBAR
    xor eax,eax
    out dx,eax
    mov byte [ac97_output_owned],0
.restore:
    call boot_restore_pci
    jc .failed
    cmp byte [ac97_dma_unquiesced],0
    jne .failed
    clc
    ret
.failed:
    stc
    ret

boot_restore_pci:
    cmp byte [boot_pci_saved],1
    jne .done
    mov bx,[pci_bdf]
    mov di,PCI_COMMAND
    mov cx,[boot_pci_command]
    cmp byte [ac97_dma_unquiesced],0
    je .write
    and cx,~4                  ; never revive a stream that did not quiesce
.write:
    mov ax,PCI_WRITE_CONFIG_WORD
    AUDIO_TRACE audio_diag_restore
    stc
    int 0x1A
    pushf
    push cs
    pop ds
    popf
    jc .write_failed
    cmp byte [ac97_dma_unquiesced],0
    je .restored
    call ac97_check_bme_off
    jc ac97_dma_fatal
.restored:
    mov byte [boot_pci_saved],0
.done:
    clc
    ret
.write_failed:
    cmp byte [ac97_dma_unquiesced],0
    je .failed
    call ac97_check_bme_off
    jc ac97_dma_fatal
.failed:
    stc
    ret

ac97_dma_fatal:
    ; No DOS exit/TSR/free here: process termination also releases the COM
    ; holding the BDL. If neither halt nor BME-off can be verified, retain
    ; every allocation and make the failure visible even during /Q startup.
    mov byte [audio_diagnostic],1
    AUDIO_TRACE audio_diag_dma_fatal
.hold:
    sti
    hlt
    jmp .hold

ac97_wait_codec:
    push ax
    push cx
    push dx
    ; Bring up an AC-link left disabled by firmware, then wait for codec 0.
    mov dx, [nabm_base]
    add dx, ICH_GLOB_CNT
    in eax, dx
    cmp eax,0xFFFFFFFF
    je .timeout
    and eax, 0xFFCFFFF7          ; AC-link on; 2-channel PCM, not inherited 4/6
    or eax, 0x00000002           ; cold-reset/run bit
    out dx, eax
%ifdef BOOT_SOUND
    xor ah,ah
    int 0x1A
    mov [boot_codec_tick],dx
    mov dword [boot_poll_budget],0x200000
%endif
    mov cx, 0xFFFF
.wait:
    mov dx, [nabm_base]
    add dx, ICH_GLOB_STA
    in eax, dx
    cmp eax,0xFFFFFFFF
    je .timeout
    test eax, 0x00000100          ; primary codec ready
    jnz .ready
%ifdef BOOT_SOUND
    in al,0x80
    xor ah,ah
    int 0x1A
    sub dx,[boot_codec_tick]
    cmp dx,36
    jae .timeout
    dec dword [boot_poll_budget]
    jnz .wait
%else
    loop .wait
%endif
.timeout:
    stc
    jmp .out
.ready:
    clc
.out:
    pop dx
    pop cx
    pop ax
    ret

; Intel ICH3 section 5.18.1.23: NAM writes are posted across the AC-link.
; Wait for CAS before EVERY codec read/write; a CPU OUT does not mean that
; the codec has received the previous write. Keep timeout independent of
; BIOS ticks/interrupt delivery and propagate failure to the caller.
ac97_codec_wait:
    push ax
    push cx
    push dx
    mov dx, [nabm_base]
    add dx, ICH_CAS
    mov cx, 0xFFFF
.poll:
    in al, dx
    test al, 1
    jz .ready
    in al, 0x80
    loop .poll
    stc
    jmp .out
.ready:
    clc
.out:
    pop dx
    pop cx
    pop ax
    ret

; DX=codec I/O port; AX=value for writes/result for reads; CF=timeout.
ac97_codec_read:
    call ac97_codec_wait
    jc .out
    in ax, dx
    ; An absent/timed-out AC-link read returns all ones. In particular,
    ; FFFFh must not satisfy the analogue-ready mask in register 26h.
    cmp ax, 0xFFFF
    je .failed
    clc
    ret
.failed:
    stc
.out:
    ret

ac97_codec_write:
    call ac97_codec_wait
    jc .out
    out dx, ax
.out:
    ret

ac97_prepare_codec:
    push ax
    push bx
    push cx
    push dx
%ifndef BOOT_SOUND
    ; Reset the codec as well as the ICH AC-link.  Real notebook firmware can
    ; leave the CS4299 alive but with its analogue path powered down.
    mov dx, [nam_base]
    add dx, AC97_RESET
    xor ax, ax
    call ac97_codec_write
    jc .fail
    mov cx, 0x2000
.reset_delay:
    in al, 0x80
    loop .reset_delay
%endif

    mov dx, [nam_base]
    add dx, AC97_POWERDOWN
%ifdef BOOT_SOUND
    ; Match the working ICH driver: preserve firmware's EAPD polarity and
    ; ADC policy, waking only the DAC/mixer/reference playback path.
    call ac97_codec_read
    jc .fail
    cmp ax, 0xFFFF
    je .fail
    and ax, ~0x7E00
%else
    xor ax, ax
%endif
    call ac97_codec_write
    jc .fail
    ; Bits 0..3 report analogue-reference, mixer and DAC readiness.  The
    ; CS4299 on the T23 needs much longer than an emulated codec after reset.
%ifdef BOOT_SOUND
    push dx
    in al,0x80
    xor ah,ah
    int 0x1A
    mov [boot_codec_tick],dx
    mov dword [boot_poll_budget],0x200000
    pop dx
%endif
    mov cx, 0xFFFF
.power_wait:
    call ac97_codec_read
    jc .fail
    mov [codec_power_status], ax
    mov bx, ax
    and bx, 0x000E              ; playback does not require ADC ready (bit 0)
    cmp bx, 0x000E
    je .power_ready
%ifdef BOOT_SOUND
    push dx
    in al,0x80
    xor ah,ah
    int 0x1A
    sub dx,[boot_codec_tick]
    cmp dx,36
    pop dx
    jae .fail
    dec dword [boot_poll_budget]
    jnz .power_wait
    jmp .fail
%else
    in al, 0x80
    loop .power_wait
    jmp .fail
%endif
.power_ready:
    ; Do not request generic SPDIF.  CS4299 exposes SPDIF through Cirrus
    ; vendor registers and the standard bit can disconnect analogue PCM.
    mov dx, [nam_base]
    add dx, AC97_EXTENDED_STATUS
    call ac97_codec_read
    jc .fail
    and ax, ~0x0007             ; fixed 48 kHz, single rate, analogue output
    call ac97_codec_write
    jc .fail
    mov dx, [nam_base]
    add dx, AC97_MASTER_VOL
    xor ax, ax
    call ac97_codec_write
    jc .fail
    mov dx, [nam_base]
    add dx, AC97_HEADPHONE_VOL
    xor ax, ax
    call ac97_codec_write
    jc .fail
    mov dx, [nam_base]
    add dx, AC97_PCM_OUT_VOL
    xor ax, ax
    call ac97_codec_write
    jc .fail

    mov dx, [nam_base]
    add dx, AC97_VENDOR_ID1
    call ac97_codec_read
    jc .fail
    mov [codec_vendor_1], ax
    mov dx, [nam_base]
    add dx, AC97_VENDOR_ID2
    call ac97_codec_read
    jc .fail
    mov [codec_vendor_2], ax
    mov ax, [codec_vendor_1]
    cmp ax, 0xFFFF
    je .fail
    or ax, [codec_vendor_2]
    jz .fail

    ; ALSA's CS4299 initialization selects the Cirrus AC-link analogue mode
    ; through vendor register 5Eh.  Restrict the write to IDs CRY0..CRY7.
    cmp word [codec_vendor_1], 0x4352
    jne .codec_ready
    mov ax, [codec_vendor_2]
    and ax, 0xFFF8
    cmp ax, 0x5930
    jne .codec_ready
    mov dx, [nam_base]
    add dx, AC97_CSR_ACMODE
    mov ax, 0x0080
    call ac97_codec_write
    jc .fail
.codec_ready:
    clc
    jmp .out
.fail:
    stc
.out:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_hardware_report:
    cmp byte [quiet_mode], 0
    jne .done
    mov dx, msg_device
    call print_line
    mov ax, [pci_device_id]
    call print_hex16
    mov dx, msg_nam
    call print_line
    mov ax, [nam_base]
    call print_hex16
    mov dx, msg_nabm
    call print_line
    mov ax, [nabm_base]
    call print_hex16
    mov dx, msg_subvendor
    call print_line
    mov ax, [pci_subsystem_vendor]
    call print_hex16
    mov dx, msg_codec
    call print_line
    mov ax, [codec_vendor_1]
    call print_hex16
    mov dl, ':'
    mov ah, 0x02
    int 0x21
    mov ax, [codec_vendor_2]
    call print_hex16
    mov dx, msg_crlf
    call print_line
    cmp byte [thinkpad_ec_seen], 0
    je .done
    mov dx, msg_ec_audio
    call print_line
    xor ax, ax
    mov al, [thinkpad_ec_before]
    call print_hex16
    mov dx, msg_ec_arrow
    call print_line
    xor ax, ax
    mov al, [thinkpad_ec_after]
    call print_hex16
    mov dx, msg_crlf
    call print_line
.done:
    ret

; IBM ThinkPads have a second hardware mute/volume gate in EC register 30h,
; independent from the AC'97 codec mixer.  Linux thinkpad-acpi documents bit
; 6 as mute and bits 3..0 as the volume.  Touch it only when PCI subsystem
; vendor 1014h identifies IBM, and bound every polling loop.
thinkpad_audio_unmute:
    push ax
    push bx
    ; The firmware also owns this EC. Without an ACPI global-lock/transaction
    ; driver, raw EC writes are an explicit /E diagnostic only. Ordinary
    ; playback leaves the notebook's volume/mute keys in control.
    cmp byte [ec_explicit], 1
    jne .done
    cmp word [pci_subsystem_vendor], 0x1014
    jne .done
    AUDIO_TRACE audio_diag_ec
    call ec_read_audio
    jc .done
    mov [thinkpad_ec_before], al
    mov byte [thinkpad_ec_seen], 1
    and al, ~TP_EC_AUDIO_MUTE
    mov bl, al
    and bl, TP_EC_AUDIO_LEVEL_MASK
    jnz .have_level
    and al, ~TP_EC_AUDIO_LEVEL_MASK
    or al, TP_EC_AUDIO_LEVEL_MAX
.have_level:
    mov bl, al
    call ec_write_audio
    jc .done
    mov [thinkpad_ec_after], bl
.done:
    pop bx
    pop ax
    ret

ec_wait_input_clear:
    push cx
    mov cx, 0xFFFF
.loop:
    in al, EC_STATUS_COMMAND
    test al, EC_STATUS_IBF
    jz .ok
    loop .loop
    stc
    jmp .done
.ok:
    clc
.done:
    pop cx
    ret

ec_wait_output_full:
    push cx
    mov cx, 0xFFFF
.loop:
    in al, EC_STATUS_COMMAND
    test al, EC_STATUS_OBF
    jnz .ok
    loop .loop
    stc
    jmp .done
.ok:
    clc
.done:
    pop cx
    ret

ec_read_audio:
    call ec_wait_input_clear
    jc .fail
    mov al, EC_CMD_READ
    out EC_STATUS_COMMAND, al
    call ec_wait_input_clear
    jc .fail
    mov al, TP_EC_AUDIO
    out EC_DATA, al
    call ec_wait_output_full
    jc .fail
    in al, EC_DATA
    clc
    ret
.fail:
    stc
    ret

ec_write_audio:
    ; IN BL=new complete EC audio byte.
    call ec_wait_input_clear
    jc .fail
    mov al, EC_CMD_WRITE
    out EC_STATUS_COMMAND, al
    call ec_wait_input_clear
    jc .fail
    mov al, TP_EC_AUDIO
    out EC_DATA, al
    call ec_wait_input_clear
    jc .fail
    mov al, bl
    out EC_DATA, al
    clc
    ret
.fail:
    stc
    ret

print_hex16:
    push ax
    push bx
    push cx
    push dx
    mov bx, ax
    mov cx, 4
.nibble:
    rol bx, 4
    mov dl, bl
    and dl, 0x0F
    cmp dl, 10
    jb .decimal
    add dl, 'A' - 10
    jmp .emit
.decimal:
    add dl, '0'
.emit:
    mov ah, 0x02
    int 0x21
    loop .nibble
    pop dx
    pop cx
    pop bx
    pop ax
    ret

%ifndef UI_SFX_DRIVER
ac97_play_sample:
    push ax
    push bx
    push cx
    push dx

    ; Reset the PCM-out DMA engine and clear sticky completion/error bits.
    mov dx, [nabm_base]
    add dx, ICH_PO_CR
    AUDIO_TRACE audio_diag_dma_reset
    mov al, ICH_CR_RESET
    out dx, al
    mov cx, 0xFFFF
.wait_reset:
    in al, dx
    test al, ICH_CR_RESET
    jz .reset_done
    loop .wait_reset
    jmp .fail
.reset_done:
    mov dx, [nabm_base]
    add dx, ICH_PO_SR
    mov ax, 0x001C
    out dx, ax

%ifdef BOOT_SOUND
    call boot_fill_bdl
%else
    ; Mirror a short sample over all 32 ICH descriptors.  One copy lasts only
    ; about 64 ms and was effectively inaudible on a real laptop; the complete
    ; BDL produces a deterministic two-second diagnostic tone.
    xor eax, eax
    mov ax, cs
    shl eax, 4
    mov ebx, eax
    add eax, ac97_sample
    mov [sample_physical], eax
    add ebx, ac97_bdl

    push ds
    pop es
    mov di, ac97_bdl
    mov cx, 32
.fill_bdl:
    mov eax, [sample_physical]
    stosd
    mov ax, ac97_sample_words
    stosw
    xor ax, ax
    stosw
    loop .fill_bdl
%endif

    mov dx, [nabm_base]
    add dx, ICH_PO_BDBAR
    mov eax, ebx
    out dx, eax
    mov dx, [nabm_base]
    add dx, ICH_PO_LVI
%ifdef BOOT_SOUND
    mov al, 14
%else
    mov al, 31
%endif
    out dx, al
    mov dx, [nabm_base]
    add dx, ICH_PO_CR
    AUDIO_TRACE audio_diag_dma_start
    mov al, ICH_CR_RUN
    out dx, al

    ; Poll for halt/completion with a BIOS-tick deadline.  No IRQ routing is
    ; required, which is important on notebooks with shared PCI interrupts.
    mov ah, 0
    int 0x1A
    mov bx, dx
    mov dword [ac97_dma_poll_budget],0x800000
.poll:
    in al,0x80
    mov dx, [nabm_base]
    add dx, ICH_PO_SR
    in ax, dx
    test al, ICH_SR_ERROR
    jnz .stop_fail
    test al, ICH_SR_DCH
    jz .deadline
    ; DCH is also set before the first DMA fetch. Only a last-buffer
    ; completion proves that the entire melody reached the DAC.
    test al, 0x04               ; LVBCI: last valid buffer completed
    jnz .ok
.deadline:
    mov ah, 0
    int 0x1A
    mov ax, dx
    sub ax, bx
    cmp ax, 72
    jae .stop_fail
    dec dword [ac97_dma_poll_budget]
    jnz .poll
.stop_fail:
    mov dx, [nabm_base]
    add dx, ICH_PO_CR
    xor al, al
    out dx, al
.fail:
    stc
    jmp .out
.ok:
    mov dx, [nabm_base]
    add dx, ICH_PO_CR
    xor al, al
    out dx, al
    clc
.out:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

%endif
parse_quiet_switch:
    mov byte [quiet_mode], 0
    mov byte [audio_diagnostic], 0
    mov byte [ec_explicit], 0
    mov si, 0x0081
    mov cl, [0x0080]
    xor ch, ch
.scan:
    jcxz .done
    lodsb
    dec cx
    cmp al, '/'
    jne .scan
    jcxz .done
    lodsb
    dec cx
    and al, 0xDF
    cmp al, 'Q'
    je .quiet
    cmp al, 'D'
    je .diagnostic
    cmp al, 'E'
    jne .scan
    mov byte [ec_explicit], 1
    jmp .scan
.diagnostic:
    mov byte [audio_diagnostic], 1
    jmp .scan
.quiet:
    mov byte [quiet_mode], 1
    jmp .scan
.done:
    ret

audio_trace:
    pushf
    pushad
    push ds
    push es
    push cs
    pop ds
    cmp byte [audio_diagnostic], 0
    je .done
    ; Do not depend on DOS services: the first marker must be visible even
    ; if the following AH=4Ah resize never returns. Emit through the existing
    ; BIOS text console and the already initialized COM1 console, with a
    ; bounded transmitter wait. No trace occurs while our DMA is running.
    mov si, dx
.next:
    mov al, [cs:si]
    inc si
    cmp al, '$'
    je .done
    mov bl, al
    mov dx, 0x3FD
    mov cx, 0x0400
.serial_wait:
    in al, dx
    test al, 0x20
    jnz .serial_ready
    loop .serial_wait
    jmp .video
.serial_ready:
    mov al, bl
    mov dx, 0x3F8
    out dx, al
.video:
    mov al, bl
    mov ah, 0x0E
    mov bx, 0x0007
    push si
    int 0x10
    pop si
    jmp .next
.done:
    pop es
    pop ds
    popad
    popf
    ret

print_line:
    cmp byte [quiet_mode], 0
    jne .done
    mov ah, 0x09
    int 0x21
.done:
    ret

intel_ac97_ids dw 0x2415, 0x2425, 0x2445, 0x2485, 0x24C5
                 dw 0x24D5, 0x25A6, 0x266E, 0x2698, 0x27DE, 0
pci_bdf dw 0
pci_device_id dw 0
pci_id_next dw 0
pci_subsystem_vendor dw 0
nam_base dw 0
nabm_base dw 0
codec_vendor_1 dw 0
codec_vendor_2 dw 0
codec_power_status dw 0
sample_physical dd 0
quiet_mode db 0
audio_diagnostic db 0
ec_explicit db 0
boot_pci_command dw 0
boot_pci_saved db 0
ac97_output_owned db 0
ac97_dma_unquiesced db 0
ac97_dma_poll_budget dd 0
thinkpad_ec_seen db 0
thinkpad_ec_before db 0
thinkpad_ec_after db 0

msg_begin db '[AC97INIT] PROBE Intel ICH AC97', 13, 10, '$'
msg_not_found db '[AC97INIT] NO SUPPORTED CONTROLLER', 13, 10, '$'
msg_failed db '[AC97INIT] CONTROLLER FOUND, PLAYBACK FAILED', 13, 10, '$'
msg_done db '[AC97INIT] PCM PLAYBACK OK', 13, 10, '$'
msg_device db '[AC97INIT] PCI 8086:', '$'
msg_nam db ' NAM=', '$'
msg_nabm db ' NABM=', '$'
msg_subvendor db ' SUBSYS=', '$'
msg_codec db ' CODEC=', '$'
msg_ec_audio db '[AC97INIT] IBM EC AUDIO 0x', '$'
msg_ec_arrow db ' -> 0x', '$'
msg_crlf db 13, 10, '$'
audio_diag_pci_check db '[SOUND:D] PCI BIOS presence',13,10,'$'
audio_diag_pci_find db '[SOUND:D] PCI find controller',13,10,'$'
audio_diag_pci_bar db '[SOUND:D] PCI read I/O base',13,10,'$'
audio_diag_pci_vendor db '[SOUND:D] PCI read subsystem',13,10,'$'
audio_diag_pci_command_read db '[SOUND:D] PCI read command',13,10,'$'
audio_diag_pci_command_write db '[SOUND:D] PCI write command',13,10,'$'
audio_diag_ec db '[SOUND:D] Explicit EC access (/E)',13,10,'$'
audio_diag_dma_reset db '[SOUND:D] DMA reset',13,10,'$'
audio_diag_dma_start db '[SOUND:D] DMA start and bounded poll',13,10,'$'
audio_diag_restore db '[SOUND:D] Restore PCI command',13,10,'$'
audio_diag_dma_fatal db 13,10,'Audio DMA stop could not be confirmed.',13,10
                    db 'Memory retained; power off the computer.',13,10,'$'

align 16
ac97_bdl:
    times 32 dq 0

align 16
; 48 kHz, signed 16-bit stereo square/ramp tone.  The descriptor length is
; expressed in 16-bit samples, not bytes.
%ifdef UI_SFX_DRIVER
%include "src/com/ui_sound_driver.inc"
%elifndef BOOT_SOUND
ac97_sample:
    times 384 dw -12000, -12000, -6000, -6000, 0, 0, 6000, 6000
    times 384 dw 12000, 12000, 6000, 6000, 0, 0, -6000, -6000
ac97_sample_end:
ac97_sample_words equ (ac97_sample_end - ac97_sample) / 2
%else
%include "src/com/boot_sound.inc"
%endif
