; Ciuki F0 real-mode loader. SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0
%include "src/boot/boot_info.inc"
%define LOAD_BASE 0x20000
%define B(field) boot_info + ciuki_boot_info.%+field
%define MAX_PH 32
; Fixed 16-byte header. The builder patches sectors and CRC over [16,end).
dd 0x52444c43
dw 1, (image_end-$$+511)/512
dd 0
dw entry, 16
entry:
    cli
    cld
    xor ax, ax
    mov ss, ax
    mov esp, 0x7c00
    mov fs, ax
    mov ax, cs
    mov ds, ax
    mov es, ax
    sti
    mov [B(boot_drive)], dl
    mov eax, [fs:0x7c6] ; Relocated MBR, first partition start.
    mov [B(partition_lba)], eax
    mov dword [B(magic)], CIUKI_BOOT_INFO_MAGIC
    mov word [B(version)], CIUKI_BOOT_INFO_VER
    mov word [B(size)], CIUKI_BOOT_INFO_SIZE
    mov dword [B(loader_start)], LOAD_BASE
    mov dword [B(loader_end)], LOAD_BASE+image_padded_end
    call uart_init
    ; Reject minimum CPU/firmware failures before even EDD disk discovery.
    call cpu_minimum
    call memory_collect
    call vbe_minimum
    call platform_collect
    call fwcfg_request
    test dword [B(flags)], CBI_F_INPUT_FORCED
    jnz .policy
    call input_platform_firmware_first
    jnc .policy
    mov byte [B(input_policy)], 1
.policy:
    call a20_enable
    call fat_mount
    call config_read
    call boot_menu
    call video_select
    call elf_load
    cli
    cld
    mov al, 0xff
    out 0x21, al
    out 0xa1, al
    lgdt [gdtr]
    mov eax, cr0
    and eax, 0x7fffffff
    or al, 1
    mov cr0, eax
    jmp dword 0x08:(LOAD_BASE+protected_entry)

; All errors terminate without entering the kernel. SI names a short code.
fatal:
    call print
    mov si, newline
    call print
    xor ax, ax
    int 0x16
    cli
.halt:
    hlt
    jmp .halt

%include "src/boot/ciukldr/platform.inc"
%include "src/boot/input_platform.inc"
%include "src/boot/ciukldr/memory.inc"
%include "src/boot/ciukldr/disk.inc"
%include "src/boot/ciukldr/menu.inc"
%include "src/boot/ciukldr/video.inc"
%include "src/boot/ciukldr/elf.inc"

; BIOS scratch/data are part of the reclaimable loader extent.
align 16, db 0
boot_info times CIUKI_BOOT_INFO_SIZE db 0
align 8, db 0
raw_map times CIUKI_E820_MAX*24 db 0
sector times 512 db 0
cluster times 4096 db 0
elf_header times 52 db 0
ph_table times MAX_PH*32 db 0
cfg_buffer times 512 db 0
fw_file times 64 db 0
selector_buffer times 65 db 0
mode_buffer times 256 db 0
mode_list times 512 dw 0
align 8, db 0
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
    dq 0x00009a020000ffff ; 16-bit loader code, base 0x20000
gdt_end:
gdtr dw gdt_end-gdt-1
    dd LOAD_BASE+gdt
newline db 13,10,0

bits 32
protected_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7c00
    mov ebx, LOAD_BASE+boot_info
    mov eax, CIUKI_BOOT_MAGIC_EAX
    jmp [LOAD_BASE+B(kernel_entry_phys)]
bits 16
image_end:
%if image_end-$$ > 65536
%error F0 loader must fit its 64 KiB real-mode segment
%endif
times ((($-$$)+511)/512)*512-($-$$) db 0
image_padded_end:
