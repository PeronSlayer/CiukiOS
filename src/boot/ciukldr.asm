; Ciuki F0 real-mode loader. SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0
%include "src/boot/boot_info.inc"
%define LOAD_BASE 0x20000
%define B(field) boot_info + ciuki_boot_info.%+field
%define MAX_PH 32
; Version shown in the menu title; build_image.py passes it from
; config/version.json (-dCIUKI_VERSION='...'), bare assemblies get 'unknown'.
%ifndef CIUKI_VERSION
%define CIUKI_VERSION 'unknown'
%endif
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
    ; Do not inherit boot policy or discovery data across a warm entry.
    ; ES=CS, DF=0; DX still contains the incoming BIOS boot drive.
    xor ax, ax
    mov di, boot_info
    mov cx, CIUKI_BOOT_INFO_SIZE
    rep stosb
    sti
    mov [B(boot_drive)], dl
    mov eax, [fs:0x7c6] ; Relocated MBR, first partition start.
    mov [B(partition_lba)], eax
    mov dword [B(magic)], CIUKI_BOOT_INFO_MAGIC
    mov word [B(version)], CIUKI_BOOT_INFO_VER
    mov word [B(size)], CIUKI_BOOT_INFO_SIZE
    mov dword [B(loader_start)], LOAD_BASE
    mov dword [B(loader_end)], LOAD_BASE+loader_bss_end
    call uart_init
    call progress_init
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
    or dword [B(flags)], CBI_F_INPUT_DETECTED
.policy:
    call a20_enable
    call fat_mount
    call config_read
    call bootstat_read
    mov al, 0
    call progress_begin
    call boot_menu
    call hwprofiles_boot
    call autoscan_check
    call bootstat_write
    call progress_end
    mov al, 1
    call progress_begin
    call video_select
    call progress_end
    mov al, 2
    call progress_begin
    call elf_load
    call progress_require
    call progress_end
    mov al, 3
    call progress_begin
    call progress_end
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
    mov byte [screen_quiet], 0
    call progress_failure
    call print
    mov si, newline
    call print
    cli
.halt:
    hlt
    jmp .halt

%include "src/boot/ciukldr/platform.inc"
%include "src/boot/ciukldr/progress.inc"
%include "src/boot/input_platform.inc"
%include "src/boot/ciukldr/memory.inc"
%include "src/boot/ciukldr/disk.inc"
%include "src/boot/ciukldr/menu.inc"
%include "src/boot/ciukldr/startup.inc"
%include "src/boot/ciukldr/hwprofiles.inc"
%include "src/boot/ciukldr/video.inc"
%include "src/boot/ciukldr/elf.inc"

; BIOS scratch/data are part of the reclaimable loader extent.
align 16, db 0
boot_info times CIUKI_BOOT_INFO_SIZE db 0
align 8, db 0
raw_map times CIUKI_E820_MAX*24 db 0
; f0-54: only memory_collect/map_normalize use raw_map, before A20, FAT and
; the menu; later RAM checks read B(e820). elf_load, the last stage, fills
; all ph_count*32 bytes before reading a program header, so the program
; headers reuse that scratch instead of 1 KiB more of the fixed file.
ph_table equ raw_map
%if MAX_PH*32 > CIUKI_E820_MAX*24
%error ELF program headers must fit the raw E820 scratch
%endif
elf_header times 52 db 0
fw_file times 64 db 0
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
; Fixed extent: 48 sectors (tests/host/test_boot_modes.py checks it).
%if image_end-$$ > 24576
%error F0 loader must fit its fixed 24576-byte image
%endif
; Private, versioned loader trail in the last 32 bytes of the fixed image.
; core/loader_progress.inc copies it before the loader extent is reclaimed.
times 24536-($-$$) db 0
db "G29DCVT1"
progress_trail db 'CLDRPRG1'
    dd 0,0,0,0 ; entered, completed, stalled, failed stage masks
    dq 0
image_padded_end:
; Loader-only scratch beyond the file image but inside the reported loader
; extent (loader_end); its contents are undefined until video_select writes
; them, as the ranking loop always does before reading an entry.
absolute image_padded_end
mode_scores resw 512
; Both input paths initialize every byte covered by their explicit length
; before selector_validate/accept read it. Keep this scratch out of the fixed
; 48-sector image so another runtime selector needs no larger boot extent.
selector_buffer resb 65
; ELF segment bounce buffer: file_read initializes exactly copy_count bytes
; before the copy loop consumes them. Keep scratch out of the 48-sector file.
cfg_buffer resb 512
; Every successful disk_read fills all 512 bytes before a consumer runs.
; Keep scratch out of the fixed file, leaving room for progress diagnostics.
sector resb 512
; G29D: the cluster buffer covers FAT16 through 32 KiB per cluster.
; cluster_read fills the BPB cluster size before directory_find/file_getc
; consume it (file_offset starts at cluster_bytes, file_loaded at 0), so it needs no
; initial zero fill; loader_end covers it.
cluster resb 32768
cv_header resb 512
cv_data resb 512
hw_line resb 160
hw_names resb 256
loader_segment_end:
%if loader_segment_end-mode_scores > 40960
%error Converter buffers must fit the loader segment
%endif
; A separate 32 KiB GS segment holds the journal target index. The boot-info
; loader extent covers both segments until the protected-mode kernel reclaims it.
cv_targets equ 0
; Hardware-profile bytes occupy GS:8000..DFFF, after the converter index.
hw_raw equ 0x8000
loader_bss_end equ 0x1e000
