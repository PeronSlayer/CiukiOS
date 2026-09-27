; VGASEM.COM: deterministic VGA memory/register semantics probe.
; Ordinary DOS/VGA program: standard INT 10h mode sets, VGA ports and direct
; A000/B800 memory cycles using many x86 instruction forms. It hashes what the
; CPU reads back and writes one result file. The same binary runs natively on
; QEMU's VGA (independent implementation) and as a guest under CVSESSION; the
; files must match. Chain-4 and unchained accesses are never mixed, because
; QEMU compacts chained VRAM while real VGA (and the model) does not.
; Usage: VGASEM <output file> [W]
;   W: hold a static image at five display checkpoints and wait for a key,
;      so an observer can compare what is displayed.
bits 16
cpu 386
org 100h

%define RESULTS_MAX 96

start:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    cld
    mov bx,(program_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    jc fatal
    call parse_name
    jc fatal
    call parse_wait
    mov dx,hold_text                 ; observers see whether checkpoints hold
    cmp byte [wait_keys],0
    jne .hold_report
    mov dx,run_text
.hold_report:
    mov ah,9
    int 21h
    mov ax,cs
    add ax,(program_end-$$+100h+15)/16
    mov [buffer_seg],ax             ; 64 KiB scratch after the program
    mov bx,1000h
    mov ah,48h
    int 21h
    jc fatal
    mov [buffer_seg],ax

    call test_mode13
    call test_dac
    call test_unchained
    call test_planar12
    call test_mode0d
    call test_text
    call test_registers

    mov ax,0003h
    int 10h
    push cs
    pop ds
    mov dx,name_buffer
    xor cx,cx
    mov ah,3Ch
    int 21h
    jc fatal
    mov bx,ax
    mov dx,results
    mov cx,[result_bytes]
    mov ah,40h
    int 21h
    jc fatal
    mov ah,3Eh
    int 21h
    mov dx,done_text
    mov ah,9
    int 21h
    mov ax,4C00h
    int 21h
fatal:
    push cs
    pop ds
    mov ax,0003h
    int 10h
    mov dx,fail_text
    mov ah,9
    int 21h
    mov ax,4C01h
    int 21h

parse_wait:
    lodsb
    cmp al,' '
    je parse_wait
    and al,0DFh
    cmp al,'W'
    jne .done
    mov byte [wait_keys],1
.done:
    ret

; Display checkpoint: marker for observers, then wait for any key.
checkpoint:
    inc byte [checkpoint_id]
    cmp byte [wait_keys],0
    je .done
    push ax
    mov al,[checkpoint_id]
    mov [observation+8],al
    xor ah,ah
    int 16h
    mov byte [observation+8],0
    pop ax
.done:
    ret

parse_name:
    mov si,81h
    mov di,name_buffer
.skip:
    lodsb
    cmp al,' '
    je .skip
    cmp al,13
    je .bad
.copy:
    cmp al,13
    je .end
    cmp al,' '
    je .end
    stosb
    lodsb
    cmp di,name_buffer+63
    jb .copy
.bad:
    stc
    ret
.end:
    dec si                           ; SI -> terminator (space or CR)
    xor al,al
    stosb
    clc
    ret

; ---- result recording ----
; record: EAX = value (a hash or a register snapshot word)
record:
    push bx
    mov bx,[result_bytes]
    cmp bx,RESULTS_MAX*4
    jae .full
    mov [results+bx],eax
    add word [result_bytes],4
.full:
    pop bx
    ret

; FNV-1a over CX bytes at buffer_seg:0000. Returns EAX.
hash_buffer:
    push ds
    push si
    push cx
    push edx
    mov ds,[cs:buffer_seg]
    xor si,si
    mov eax,2166136261
    movzx ecx,cx
    jecxz .done
.next:
    xor al,[si]
    imul eax,eax,16777619
    inc si
    loop .next
.done:
    pop edx
    pop cx
    pop si
    pop ds
    ret

; Copy CX bytes from A000:SI to buffer via REP MOVSB (latch-loading reads).
save_video:
    push ds
    push es
    push si
    push di
    push cx
    mov ax,0A000h
    mov ds,ax
    mov es,[cs:buffer_seg]
    xor di,di
    rep movsb
    pop cx
    pop di
    pop si
    pop es
    pop ds
    ret

; Hash CX bytes of A000:0000 and record it.
record_video:
    xor si,si
    call save_video
    call hash_buffer
    jmp record

seq_write:                          ; AL=index AH=value
    mov dx,3C4h
    out dx,ax
    ret
gc_write:
    mov dx,3CEh
    out dx,ax
    ret
crtc_write:
    mov dx,3D4h
    out dx,ax
    ret

; ---- mode 13h: chain-4 addressing through many forms ----
test_mode13:
    mov ax,0013h
    int 10h
    mov ax,0A000h
    mov es,ax
    xor di,di
    mov cx,32000
    mov ax,0302h
    rep stosw                        ; word stores
    xor esi,esi                      ; deterministic index registers
    mov edi,1000
    mov ecx,2000
    mov eax,11223344h
    a32 rep stosd                    ; 67h/66h: 32-bit addressing and operand
    mov di,20000
    mov al,77h
    mov cx,321
    rep stosb
    mov bx,30000
    mov byte [es:bx],0A5h
    mov word [es:bx+si+5],1234h      ; si is 0 from preceding code paths
    mov dword [es:bx+9],89ABCDEFh
    push es
    pop fs
    mov byte [fs:bx+20],5Ah
    push es
    pop gs
    mov word [gs:bx+22],6655h
    or byte [es:bx],0Fh              ; read-modify-write forms
    and word [es:bx+22],0F0F0h
    xor byte [es:bx+20],0FFh
    add byte [es:bx+1],3
    inc word [es:bx+30]
    dec byte [es:bx+31]
    not byte [es:bx+32]
    neg byte [es:bx+33]
    shl byte [es:bx+34],1
    rol word [es:bx+36],3
    mov al,99h
    xchg al,[es:bx+40]
    mov [es:bx+41],al                ; old byte moved by the program itself
    movzx eax,byte [es:bx+9]
    mov [es:bx+42],ax
    mov si,bx                        ; MOVSB inside the aperture (DS=ES=A000)
    push es
    pop ds
    lea di,[bx+100]
    mov cx,17
    rep movsb
    std                              ; backwards
    lea si,[bx+50]
    lea di,[bx+300]
    mov cx,9
    rep movsw
    cld
    push cs
    pop ds
    mov cx,64000
    call record_video
    call checkpoint                  ; 1: chained 256-colour image
    ret

; ---- DAC and PEL mask ----
test_dac:
    mov dx,3C8h
    xor al,al
    out dx,al
    inc dx
    xor cx,cx
.write:
    mov al,cl
    and al,63
    out dx,al
    mov al,cl
    shr al,2
    out dx,al
    mov al,cl
    xor al,2Ah
    and al,63
    out dx,al
    inc cx
    cmp cx,256
    jb .write
    mov dx,3C7h
    mov al,5
    out dx,al
    mov es,[buffer_seg]
    xor di,di
    mov dx,3C9h
    mov cx,768-15
.read:
    in al,dx
    stosb
    loop .read
    mov cx,768-15
    call hash_buffer
    call record
    ; PEL mask and DAC state (3C7h). The address register readback after a
    ; read-address write is chipset-specific and deliberately not compared.
    mov dx,3C6h
    mov al,5Ah
    out dx,al
    in al,dx
    movzx eax,al
    mov dx,3C7h
    push ax
    in al,dx
    mov ah,al
    mov bx,ax
    pop ax
    mov ah,bh
    call record
    mov dx,3C6h
    mov al,0FFh
    out dx,al
    ret

; ---- unchained 256-colour: map mask, latch copies, read map ----
test_unchained:
    mov ax,0013h
    int 10h
    mov ax,0604h                     ; SR04: disable chain-4, keep odd/even off
    call seq_write
    mov ax,0014h                     ; CR14: dword mode off
    call crtc_write
    mov ax,0E317h                    ; CR17: byte mode
    call crtc_write
    mov ax,0A000h
    mov es,ax
    mov ax,0F02h                     ; clear all 256 KiB unchained
    call seq_write
    xor di,di
    xor ax,ax
    mov cx,32768
    rep stosw
    mov bl,1
.planes:
    mov ah,bl
    mov al,2
    call seq_write                   ; map mask = one plane
    xor di,di
    mov cx,8000
    mov al,bl
    mov ah,bl
    add ah,40h
.fill:
    stosw
    add al,3
    add ah,5
    loop .fill
    shl bl,1
    cmp bl,16
    jb .planes
    mov ax,0F02h
    call seq_write
    mov ax,4105h                     ; write mode 1: latch copies
    call gc_write
    push es
    pop ds
    xor si,si
    mov di,20000
    mov cx,3000
    rep movsb
    mov si,100
    mov di,40000
    mov cx,64
.copy_forms:
    mov al,[si]                      ; explicit latch load
    mov [di],al                      ; mode 1 store writes all 4 latches
    inc si
    inc di
    loop .copy_forms
    push cs
    pop ds
    mov ax,4005h
    call gc_write
    call checkpoint                  ; 2: unchained 256-colour image
    ; Read each plane back through read map select.
    mov bl,0
.read_planes:
    mov ah,bl
    mov al,4
    call gc_write
    mov cx,65535
    call record_video
    inc bl
    cmp bl,4
    jb .read_planes
    ret

; ---- mode 12h: set/reset, rotate, logical ops, write modes 2/3, read mode 1
test_planar12:
    mov ax,0012h
    int 10h
    mov ax,0A000h
    mov es,ax
    mov ax,0F02h
    call seq_write
    mov ax,0F01h                     ; enable set/reset on all planes
    call gc_write
    mov ax,0500h                     ; set/reset colour 5
    call gc_write
    xor di,di
    mov cx,4800
    mov al,0FFh
    rep stosb                        ; data ignored: colour 5
    mov ax,0001h
    call gc_write
    mov ax,0F008h
    call gc_write
    mov ax,1303h                     ; rotate 3, function OR
    call gc_write
    mov di,5000
    mov cx,4000
.rotate:
    mov ah,[es:di]                   ; load latches explicitly
    mov al,cl
    mov [es:di],al
    inc di
    loop .rotate
    mov ax,0FF08h
    call gc_write
    mov ax,0003h
    call gc_write
    ; Read-modify-write logical functions load latches first.
    mov ax,0803h                     ; AND
    call gc_write
    mov di,5100
    mov cx,200
.and_loop:
    and byte [es:di],0BBh
    inc di
    loop .and_loop
    mov ax,1803h                     ; XOR
    call gc_write
    mov di,5400
    mov cx,200
.xor_loop:
    or byte [es:di],0C3h
    inc di
    loop .xor_loop
    mov ax,0003h
    call gc_write
    mov ax,0205h                     ; write mode 2
    call gc_write
    mov ax,5A08h
    call gc_write
    mov di,12000
    mov cx,3000
.mode2:
    mov al,[es:di]                   ; load latches
    mov al,cl
    mov [es:di],al
    inc di
    loop .mode2
    mov ax,0FF08h
    call gc_write
    mov ax,0305h                     ; write mode 3: bit mask AND rotated data
    call gc_write
    mov ax,0C00h                     ; set/reset colour 12
    call gc_write
    mov ax,0203h                     ; rotate 2
    call gc_write
    mov di,20000
    mov cx,2000
.mode3:
    xchg al,[es:di]                  ; read (latches) then write
    add al,37
    inc di
    loop .mode3
    mov ax,0003h
    call gc_write
    mov ax,0005h
    call gc_write
    mov ax,0000h
    call gc_write
    mov bl,0
.read_planes:
    mov ah,bl
    mov al,4
    call gc_write
    mov cx,38400
    call record_video
    inc bl
    cmp bl,4
    jb .read_planes
    ; Read mode 1 colour compare / don't care.
    mov ax,0805h
    call gc_write
    mov ax,0502h
    call gc_write
    mov ax,0F07h
    call gc_write
    mov cx,38400
    call record_video
    mov ax,0B07h
    call gc_write
    mov ax,0302h
    call gc_write
    mov cx,38400
    call record_video
    mov ax,0005h
    call gc_write
    mov ax,0F07h
    call gc_write
    ; BIOS pixel services on top of direct writes.
    mov ax,0C0Eh
    xor bh,bh
    mov cx,100
    mov dx,50
    int 10h
    mov ax,0C8Bh                     ; XOR colour 11
    mov cx,101
    int 10h
    mov ah,0Dh
    mov cx,101
    mov dx,50
    int 10h
    movzx eax,al
    call record
    call checkpoint                  ; 3: 640x480 planar image
    ret

; ---- mode 0Dh: 40-column planar, write mode 0 + map mask ----
test_mode0d:
    mov ax,000Dh
    int 10h
    mov ax,0A000h
    mov es,ax
    mov bl,1
.planes:
    mov ah,bl
    mov al,2
    call seq_write
    xor di,di
    mov cx,4000
    mov al,bl
.fill:
    stosb
    rol al,1
    loop .fill
    shl bl,1
    cmp bl,16
    jb .planes
    mov ax,0F02h
    call seq_write
    mov bl,0
.read_planes:
    mov ah,bl
    mov al,4
    call gc_write
    mov cx,8000
    call record_video
    inc bl
    cmp bl,4
    jb .read_planes
    call checkpoint                  ; 4: 320x200 planar image
    ret

; ---- text mode 3: odd/even CPU view, BIOS TTY and scroll ----
test_text:
    mov ax,0003h
    int 10h
    mov ax,0B800h
    mov es,ax
    xor di,di
    mov cx,2000
    mov ax,1F41h
.cells:
    stosw
    inc al
    cmp al,'Z'+1
    jb .ok
    mov al,'A'
.ok:
    loop .cells
    mov si,msg_tty
.tty:
    lodsb
    test al,al
    jz .tty_done
    mov ah,0Eh
    xor bx,bx
    int 10h
    jmp .tty
.tty_done:
    mov ax,0602h                     ; scroll window up two lines
    mov bh,4Eh
    mov cx,0305h
    mov dx,0A30h
    int 10h
    push ds
    push es
    mov ax,0B800h
    mov ds,ax
    mov es,[cs:buffer_seg]
    xor si,si
    xor di,di
    mov cx,4000
    rep movsb
    pop es
    pop ds
    mov cx,4000
    call hash_buffer
    call record
    mov ah,3
    xor bh,bh
    int 10h
    movzx eax,dx
    call record
    mov ah,1                         ; hide the cursor: no blink phase to compare
    mov cx,2000h
    int 10h
    call checkpoint                  ; 5: text image
    ret

; ---- register readback after BIOS mode sets ----
test_registers:
    mov si,mode_list
.mode:
    lodsb
    cmp al,0FFh
    je .done
    push si
    xor ah,ah
    int 10h
    xor eax,eax
    mov bl,0
.seq:
    mov dx,3C4h
    mov al,bl
    out dx,al
    inc dx
    in al,dx
    call fold
    inc bl
    cmp bl,5
    jb .seq
    mov bl,0
.gc:
    mov dx,3CEh
    mov al,bl
    out dx,al
    inc dx
    in al,dx
    call fold
    inc bl
    cmp bl,9
    jb .gc
    mov bl,0
.crtc:
    mov dx,3D4h
    mov al,bl
    out dx,al
    inc dx
    in al,dx
    call fold
    inc bl
    cmp bl,25
    jb .crtc
    mov bl,0
.attr:
    mov dx,3DAh
    in al,dx
    mov dx,3C0h
    mov al,bl
    or al,20h
    out dx,al
    inc dx
    in al,dx
    call fold
    inc bl
    cmp bl,21
    jb .attr
    mov dx,3CCh
    in al,dx
    call fold
    mov eax,[fold_value]
    call record
    mov dword [fold_value],2166136261
    pop si
    jmp .mode
.done:
    ret

fold:
    push eax
    movzx eax,al
    xor [fold_value],eax
    mov eax,[fold_value]
    imul eax,eax,16777619
    mov [fold_value],eax
    pop eax
    ret

buffer_seg dw 0
result_bytes dw 0
%ifdef HOLD_ALWAYS
wait_keys db 1                       ; VGASEMW.COM: checkpoints always hold
%else
wait_keys db 0
%endif
checkpoint_id db 0
observation db 'VGASEMOB',0,0,0,0
fold_value dd 2166136261
mode_list db 03h,13h,12h,0Dh,0Eh,10h,01h,0FFh
msg_tty db 'VGA semantics',13,10,'TTY line two',13,10,0
done_text db '[VGASEM] results written',13,10,'$'
hold_text db '[VGASEM] checkpoints hold for keys',13,10,'$'
run_text db '[VGASEM] checkpoints do not hold',13,10,'$'
fail_text db '[VGASEM] FAIL',13,10,'$'
name_buffer times 64 db 0
align 4
results times RESULTS_MAX dd 0
align 16
stack_space times 1024 db 0
stack_top:
program_end:
