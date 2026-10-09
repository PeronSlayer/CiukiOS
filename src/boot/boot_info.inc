; ciuki_boot_info version 1 (NASM view). Frozen layout; the authority is
; docs/design/boot-memory.md and must match src/kernel/include/ciuki/boot_info.h.
; SPDX-License-Identifier: GPL-2.0-only

%define CIUKI_BOOT_MAGIC_EAX  0x4B554943   ; "CIUK"
%define CIUKI_BOOT_INFO_MAGIC 0x31494243   ; "CBI1"
%define CIUKI_BOOT_INFO_VER   1
%define CIUKI_BOOT_INFO_SIZE  0x10F0
%define CIUKI_E820_MAX        128
%define CIUKI_TEST_REQ_MAX    64

%define CBI_F_SAFE_MODE       (1 << 0)
%define CBI_F_SERIAL_LOG      (1 << 1)
%define CBI_F_EDID_VALID      (1 << 2)
%define CBI_F_VBE_CTRL_VALID  (1 << 3)
%define CBI_F_TEXT_MODE       (1 << 4)
%define CBI_F_SMBIOS_QEMU     (1 << 5)
%define CBI_F_TEST_REQUEST    (1 << 6)
%define CBI_F_INPUT_FORCED    (1 << 7)
%define CBI_F_A20_SHIFT       8
%define CBI_A20_ALREADY_ON    1
%define CBI_A20_INT15         2
%define CBI_A20_KBC           3
%define CBI_A20_PORT92        4

struc ciuki_e820
    .base       resq 1
    .length     resq 1
    .type       resd 1
    .ext        resd 1
endstruc

struc ciuki_boot_info
    .magic              resd 1      ; 0x000
    .version            resw 1      ; 0x004
    .size               resw 1      ; 0x006
    .flags              resd 1      ; 0x008
    .boot_drive         resb 1      ; 0x00C
    .memmap_source      resb 1      ; 0x00D
    .input_policy       resb 1      ; 0x00E
    .reserved0          resb 1      ; 0x00F
    .partition_lba      resd 1      ; 0x010
    .loader_start       resd 1      ; 0x014
    .loader_end         resd 1      ; 0x018
    .kernel_start       resd 1      ; 0x01C
    .kernel_end         resd 1      ; 0x020
    .kernel_entry_phys  resd 1      ; 0x024
    .e820_count         resd 1      ; 0x028
    .e801_low_kib       resd 1      ; 0x02C
    .e801_high_64k      resd 1      ; 0x030
    .int88_kib          resd 1      ; 0x034
    .uart_base          resd 1      ; 0x038
    .uart_divisor       resd 1      ; 0x03C
    .pci_bios           resd 1      ; 0x040
    .apm                resd 1      ; 0x044
    .acpi_rsdp_phys     resd 1      ; 0x048
    .smbios_entry_phys  resd 1      ; 0x04C
    .fb_phys            resd 1      ; 0x050
    .fb_pitch           resd 1      ; 0x054
    .fb_width           resw 1      ; 0x058
    .fb_height          resw 1      ; 0x05A
    .fb_bpp             resb 1      ; 0x05C
    .fb_red_size        resb 1      ; 0x05D
    .fb_red_pos         resb 1      ; 0x05E
    .fb_green_size      resb 1      ; 0x05F
    .fb_green_pos       resb 1      ; 0x060
    .fb_blue_size       resb 1      ; 0x061
    .fb_blue_pos        resb 1      ; 0x062
    .fb_rsvd_size       resb 1      ; 0x063
    .vbe_mode           resw 1      ; 0x064
    .test_request_len   resw 1      ; 0x066
    .test_request       resb 64     ; 0x068
    .options            resb 128    ; 0x0A8
    .edid               resb 128    ; 0x128
    .edd                resb 66     ; 0x1A8
    .reserved1          resb 6      ; 0x1EA
    .vbe_ctrl           resb 512    ; 0x1F0
    .vbe_mode_info      resb 256    ; 0x3F0
    .e820               resb 128 * ciuki_e820_size ; 0x4F0
endstruc

%if ciuki_boot_info_size != CIUKI_BOOT_INFO_SIZE
%error ciuki_boot_info size mismatch
%endif
%if ciuki_boot_info.e820 != 0x4F0
%error ciuki_boot_info e820 offset mismatch
%endif
%if ciuki_boot_info.vbe_ctrl != 0x1F0
%error ciuki_boot_info vbe_ctrl offset mismatch
%endif
