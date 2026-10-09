; VMSTART.COM - load the CiukiOS VM session stack for the DOS window.
;
;   VMSTART [Jemm options]
;
; The desktop runs it at the start of every normal session. Runs
; \VM\JEMMEX.EXE LOAD <options> (default: the options the DOS window
; profile is validated with, see below) and then \VM\JLOAD.EXE \VM\CVSESS.DLL,
; unless the session module is already resident. Messages are English; a
; failed step returns ERRORLEVEL 1. On a graphics screen (the boot splash at
; startup) nothing is drawn: its messages and the console output of Jemm
; and JLOAD go to COM1 only.
;
; Default options: no EMS, UMBs only in the qualified C000-EFFF region, and
; no VME. JemmEx owns the BIOS-discovered RAM and allocates it on demand.
bits 16
cpu 386
org 100h

%include "src/vm/session_abi.inc"

start:
    cld
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    mov si,no_memory
    jc fail
    call quiet_detect

    ; Already resident (the desktop starts it at boot): nothing to do.
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov ax,es
    or ax,di
    push cs
    pop es
    jz .load
    call network_owner
    mov si,network_failed
    jc fail
    mov ax,4C00h
    int 21h
.load:
    ; Jemm386 tail: " LOAD " + options (default or the command tail).
    mov di,jemm_tail+1
    mov si,load_word
    call copy_z
    mov si,81h
.skip:
    lodsb
    cmp al,' '
    je .skip
    cmp al,9
    je .skip
    dec si
    cmp al,13
    jne .copy_tail
    call detect_hypervisor
    test al,al
    jnz .is_qemu
    call vm_platform_is_t23
    test al,al
    jz .other_physical
    mov si,bare_metal_options
    jmp .copy_def
.other_physical:
    mov si,physical_discovery_options
    jmp .copy_def
.is_qemu:
    mov si,qemu_options
.copy_def:
    call copy_z
    jmp .tail_done
.copy_tail:
    lodsb
    cmp al,13
    je .tail_done
    stosb
    cmp di,jemm_tail+126
    jb .copy_tail
.tail_done:
    mov byte [di],13
    mov ax,di
    sub ax,jemm_tail+1
    mov [jemm_tail],al

    mov si,msg_jemm
    call print
    call silence
    mov word [params+2],jemm_tail
    mov dx,jemm_path
    call exec
    mov si,jemm_failed
    jc fail
    mov ah,4Dh
    int 21h
    test al,al
    jnz fail

    mov si,msg_jload
    call print
    mov word [params+2],jload_tail
    mov dx,jload_path
    call exec
    mov si,jload_failed
    jc fail
    mov ah,4Dh
    int 21h
    test al,al
    jnz fail

    call vm_memory_report
    call baseline_ownership
    mov si,baseline_failed
    jc fail
    call speak
    call network_owner
    mov si,network_failed
    jc fail
    mov si,msg_ready
    call print
    mov ax,4C00h
    int 21h

fail:
    push si
    call speak
    pop si
    call print
    mov ax,4C01h
    int 21h

; Register while the conventional arena still predates every GUI app heap.
baseline_ownership:
    pushad
    push ds
    push es
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    xor di,di
    mov es,di
    int 2Fh
    mov ax,es
    or ax,di
    jz .error
    mov [cs:baseline_entry],di
    mov [cs:baseline_entry+2],es
    mov ax,1607h
    mov bx,15h
    xor cx,cx
    int 2Fh
    mov ax,es
    mov si,[es:bx+6]
    push ax
    mov ah,52h
    int 21h
    pop dx
    jc .error
    mov ax,[es:bx-2]
    mov bx,dx
    mov dx,ax
    mov cx,si
    mov ax,VM_OP_VMM_IVT_BASELINE
    call far [cs:baseline_entry]
    jc .error
    pop es
    pop ds
    popad
    clc
    ret
.error:
    pop es
    pop ds
    popad
    stc
    ret

; Native networking starts before Jemm at boot. Bind its resident IRQ metadata
; once CVSESSION exists, before any VM can inherit the packet driver's state.
network_owner:
    push ds
    push es
    mov ax,3561h
    int 21h
    cmp dword [es:bx+3],'PKT '
    jne .absent
    cmp dword [es:bx+7],'DRVR'
    jne .absent
    cmp dword [es:bx+11],'CIUK'
    jne .absent
    mov ax,0FE08h
    int 61h
    jc .error
    cmp bx,4943h
    jne .error
    test cx,cx
    jz .absent
    mov [cs:network_mask],cx
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    xor di,di
    mov es,di
    int 2Fh
    mov ax,es
    test ax,ax
    jz .error
    mov [cs:network_entry],di
    mov [cs:network_entry+2],es
    mov eax,VM_OP_VMM_NET_IRQ
    mov bx,[cs:network_mask]
    call far [cs:network_entry]
    jc .error
    push cs
    pop ds
    mov si,network_ready
    call print
.absent:
    pop es
    pop ds
    clc
    ret
.error:
    pop es
    pop ds
    stc
    ret


; DS:SI zero-terminated -> ES:DI (DI advanced).
copy_z:
    lodsb
    test al,al
    jz .done
    stosb
    jmp copy_z
.done:
    ret

; DS:DX program, params -> tail. CF from EXEC.
exec:
    mov [params+4],cs
    mov [params+8],cs
    mov [params+12],cs
    mov bx,params
    mov ax,4B00h
    mov [saved_sp],sp
    int 21h
    cli
    mov bx,cs
    mov ss,bx
    mov sp,[cs:saved_sp]
    sti
    push cs
    pop ds
    push cs
    pop es
    cld
    ret

print:
    lodsb
    test al,al
    jz .done
    cmp byte [quiet],0
    jne .serial
    mov dl,al
    mov ah,2
    int 21h
    jmp print
.serial:
    call com1
    jmp print
.done:
    ret

%include "src/com/quiet_console.inc"
%include "src/com/vm_memory_report.inc"
%include "src/com/vm_platform_options.inc"

load_word db ' LOAD ',0
network_mask dw 0
network_entry dw 0,0
baseline_entry dw 0,0
baseline_failed db 'VMSTART: cannot validate baseline interrupt ownership.',13,10,0
network_ready db 'VMSTART: NIC interrupts owned by the desktop VM.',13,10,0
network_failed db 'VMSTART: cannot bind the network interrupt owner.',13,10,0
%ifdef VMSTART_JEMMEX
qemu_options db 'NOEMS X=A000-CCFF I=CD00-E7FF X=E800-FFFF NOVME',0
bare_metal_options db 'NOEMS X=A000-CFFF I=D000-DBFF X=DC00-FFFF MAX=4194303 NOVME',0
physical_discovery_options db 'NOEMS X=A000-CFFF X=DC00-FFFF MAX=4194303 NOVME',0
jemm_path db '\VM\JEMMEX.EXE',0
%else
qemu_options db 'NOEMS NOHI X=A000-CCFF I=CD00-E7FF X=E800-FFFF NODYN MAX=32M MIN=32M NOVME',0
bare_metal_options db 'NOEMS NOHI X=A000-CFFF I=D000-DBFF X=DC00-FFFF NODYN MAX=32M MIN=32M NOVME',0
physical_discovery_options db 'NOEMS NOHI X=A000-CFFF X=DC00-FFFF NODYN MAX=32M MIN=32M NOVME',0
jemm_path db '\VM\JEMM386.EXE',0
%endif

detect_hypervisor:
    push bx
    push cx
    push dx
    push si
    push ds

    ; 1. Check CPUID.1:ECX bit 31
    pushfd
    pop eax
    mov edx,eax
    xor eax,00200000h
    push eax
    popfd
    pushfd
    pop eax
    push edx
    popfd
    cmp eax,edx
    je .check_pci
    mov eax,1
    db 0x0F,0xA2                ; cpuid
    test ecx,80000000h
    jnz .found

.check_pci:
    ; 2. Check PCI Bus 0 Device 2 Function 0 (VGA in QEMU: 11111234h)
    mov eax,80001000h
    mov dx,0CF8h
    out dx,eax
    mov dx,0CFCh
    in eax,dx
    cmp eax,11111234h
    je .found

.check_bios:
    ; 3. Check for 'QEMU' or 'SeaB' in F000:E000 .. F000:FFFF
    mov ax,0F000h
    mov ds,ax
    mov si,0E000h
.bios_loop:
    cmp dword [si],'QEMU'
    je .found
    cmp dword [si],'SeaB'
    je .found
    inc si
    cmp si,0FFF0h
    jb .bios_loop

    xor al,al
    jmp .done

.found:
    mov al,1

.done:
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    ret
jload_path db '\VM\JLOAD.EXE',0
jload_tail db jload_tail_end-jload_tail-1
    db ' \VM\CVSESS.DLL'
jload_tail_end:
    db 13
msg_jemm db 'VMSTART: loading Jemm memory manager and V86 monitor...',13,10,0
msg_jload db 'VMSTART: loading the CiukiOS session module (CVSESS.DLL)...',13,10,0
msg_ready db 'VMSTART: ready.',13,10,0
no_memory db 'VMSTART: cannot resize its memory block.',13,10,0
jemm_failed db 'VMSTART: Jemm memory manager failed (see its message).',13,10,0
jload_failed db 'VMSTART: \VM\JLOAD.EXE failed (see its message).',13,10,0

params dw 0,0,0,5Ch,0,6Ch,0
saved_sp dw 0
jemm_tail times 128 db 0
    align 2
    times 512 db 0
stack_top:
image_end:
