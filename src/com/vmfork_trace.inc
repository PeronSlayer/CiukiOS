; CVFL v1 launch checkpoints. Called only by VMFORK's foreground launch path.
; All code/data is inside image_end, so both live-PSP relocations copy it.
; Diagnostics are best effort; no open handle survives a checkpoint.
trace_dos_result:
    pushf
    pop word [cs:trace_dos_flags]
    mov [cs:trace_dos_ax],ax
    ret

; Read-only pre-mutation memory measurement, bounded to 64 conventional MCBs.
; An invalid/incomplete chain leaves trace_memory_valid=0; never grants safety.
trace_initial_memory:
    pushfd
    pushad
    push ds
    push es
    mov word [cs:trace_largest_free],0
    mov word [cs:trace_total_free],0
    mov word [cs:trace_memory_valid],0
    int 12h
    mov [cs:trace_conventional_kb],ax
    mov ah,52h
    int 21h
    jc .done
    mov ax,[es:bx-2]
    mov cx,64
.walk:
    cmp ax,0A000h
    jae .done
    mov es,ax
    mov dl,[es:0]
    cmp dl,'M'
    je .type_ok
    cmp dl,'Z'
    jne .done
.type_ok:
    mov bx,[es:3]
    mov si,ax
    add si,bx
    jc .done
    inc si
    jz .done
    cmp si,0A000h
    ja .done
    cmp word [es:1],0
    jne .next
    add [cs:trace_total_free],bx
    cmp bx,[cs:trace_largest_free]
    jbe .next
    mov [cs:trace_largest_free],bx
.next:
    cmp dl,'Z'
    je .valid
    mov ax,si
    loop .walk
    jmp .done
.valid:
    mov word [cs:trace_memory_valid],1
.done:
    pop es
    pop ds
    popad
    popfd
    ret

; Measure the post-cleanup arena with the same bounded walker, preserving
; the original pre-preparation measurements and every caller register/flag.
trace_remaining_memory:
    pushfd
    pushad
    push word [cs:trace_conventional_kb]
    push word [cs:trace_memory_valid]
    push word [cs:trace_largest_free]
    push word [cs:trace_total_free]
    call trace_initial_memory
    mov ax,[cs:trace_total_free]
    mov [cs:trace_remaining_free],ax
    mov ax,[cs:trace_largest_free]
    mov [cs:trace_remaining_largest],ax
    mov ax,[cs:trace_memory_valid]
    mov [cs:trace_remaining_valid],ax
    pop word [cs:trace_total_free]
    pop word [cs:trace_largest_free]
    pop word [cs:trace_memory_valid]
    pop word [cs:trace_conventional_kb]
    popad
    popfd
    ret

trace_checkpoint:
    call disk_trace_snapshot
    pushfd
    pushad
    push ds
    push es
    push fs
    push gs
    push cs
    pop ds
    push cs
    pop es
    cld
    inc dword [trace_sequence]
    mov [trace_cs],cs
    mov ax,[root_psp]
    mov [trace_root_psp],ax
    mov ax,[vector_slot]
    mov [trace_vector],ax
    mov ah,62h
    int 21h
    mov [trace_psp],bx
    mov si,program
    mov di,trace_target
    mov cx,64
    rep movsw
    cmp byte [trace_parent],0
    je .file
    mov di,trace_record
    mov bx,[trace_generation]
    call launch_mailbox_publish
    jmp .done
.file:
    mov ax,[trace_vm]
    cmp ax,1
    jb .done
    cmp ax,3
    ja .done
    add al,'0'
    mov [trace_filename_digit],al
    mov dx,trace_filename
    xor cx,cx
    mov ah,3Ch
    int 21h
    jc .io_error
    mov bx,ax
    mov dx,trace_record
    mov cx,trace_record_end-trace_record
    mov ah,40h
    int 21h
    jc .write_error
    cmp ax,trace_record_end-trace_record
    je .close
    mov word [trace_io_error],0FFFFh    ; short write, distinct from DOS errors
    jmp .close
.write_error:
    mov [trace_io_error],ax
.close:
    mov ah,3Eh
    int 21h
    jnc .done
.io_error:
    mov [trace_io_error],ax
.done:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popfd
    ret

trace_filename db 'C:\SYSTEM\VMFORK'
trace_filename_digit db '1'
    db '.BIN',0
    align 2
trace_record:
    db 'CVFL'
    dw 1,256
trace_vm dw 0                         ; 8
trace_generation dw 0                 ; 10
trace_stage dw 0                      ; 12
trace_reason dw 0                     ; 14
trace_progress dw 0                   ; 16
trace_dos_ax dw 0                     ; 18
trace_dos_flags dw 0                  ; 20: bit 0 = last meaningful DOS CF
trace_cs dw 0                         ; 22
trace_psp dw 0                        ; 24
trace_root_psp dw 0                   ; 26
trace_parent_count dw 0               ; 28
trace_conventional_kb dw 0            ; 30
trace_largest_free dw 0               ; 32: paragraphs, before mutation
trace_memory_valid dw 0               ; 34
trace_vector dw 0                     ; 36
trace_exit dw 0                       ; 38
trace_sequence dd 0                   ; 40
trace_io_error dw 0                   ; 44: sticky previous I/O error
trace_cleanup_reason dw 0             ; 46: retained refused ancestor free
trace_vector_ref dw 0                 ; 48: rejected IVT byte offset
trace_s3_psp dw 0                     ; 50: identified child S3VBEFIX PSP
trace_s3_previous dd 0                ; 52: validated INT10 predecessor
trace_s3_paras dw 0                   ; 56: private copied resident size
trace_vector_target dd 0              ; 58: inspected or rejected IVT far pointer
trace_lfn_immediate dw 0              ; 62: observed LFN byte at PSP:105h
    times 64-($-trace_record) db 0
trace_target times 128 db 0           ; 64: ASCIIZ target path
trace_pin_events dw 0                 ; 192: observations; sweeps may repeat
trace_pin_mcb dw 0                    ; 194: first retained MCB segment
trace_pin_owner dw 0                  ; 196: retained allocation's owner PSP
trace_pin_paras dw 0                  ; 198: retained data size, paragraphs
trace_pin_vector dw 0                 ; 200: referring IVT byte offset
trace_pin_target dd 0                 ; 202: exact segment:offset alias
trace_total_free dw 0                 ; 206: initial free paragraphs
trace_remaining_free dw 0             ; 208: post-cleanup free paragraphs
trace_remaining_largest dw 0          ; 210: post-cleanup largest free block
trace_remaining_valid dw 0            ; 212: post-cleanup walk complete=1
    dd 0                              ; 214: mailbox writes PLOG here
trace_disk_state dw 0                 ; 218: 0 unbound,1 prepared,2 native,3 owned failure
trace_disk_error dw 0                 ; 220: CVATA error
trace_disk_ports dd 0                 ; 222: command/control u16
trace_disk_reads dd 0                 ; 226: completed native sector reads
trace_disk_writes dd 0                ; 230: completed native sector writes
trace_disk_lba dd 0                   ; 234: last native LBA
trace_disk_control dw 0               ; 238: control byte / observed flag
trace_disk_prepare dw 0               ; 240: parent binding stage/error
    db 'ATA1'                         ; 242: disk extension present
trace_ivt_tag dw 0                    ; 246: IV only in VMFORK, DP in DPMIRUN
trace_ivt_ready dw 0                  ; 248: sealed load-time ownership
trace_ivt_classified dw 0             ; 250: baseline targets in free payloads
trace_ivt_filtered dw 0               ; 252: equal targets replaced in output
trace_ivt_first dw 0                  ; 254: first filtered vector byte offset
    times 256-($-trace_record) db 0
trace_record_end:
%if trace_record_end-trace_record != 256
    %error CVFL_record_size_mismatch
%endif
trace_parent db 0
