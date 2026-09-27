; CiukiOS CVSESSION video extension ABI. MASM/NASM-compatible constants.
; Owned by the video/presenter track; numbers are disjoint from the core
; (0-6), scheduler (7-9, errors 12-14, caps 20h/40h) and reserved device
; ranges. All packets live in conventional memory owned by the caller.
;
; While a session is armed, every guest (V86 or fresh-HDPMI) access to
; A0000-BFFFF faults on supervisor-only PTEs and is executed by the monitor
; against the software VGA model (latches, map mask, read/write modes,
; chain-4, odd/even). Nothing is aliased to plain RAM. INT 10h is served by
; the virtual VGA BIOS; VGA ports 3B0-3DF go to the same model.

VM_OP_VIDEO_CONFIG      equ 20h ; ES:DI -> 64-byte config packet; before BEGIN
VM_OP_VIDEO_STATE       equ 21h ; ES:DI <- 256-byte state/diagnostic packet
VM_OP_VIDEO_READ        equ 22h ; EDX=model byte offset, CX=1..4096, ES:DI <- bytes
VM_OP_VIDEO_RENDER      equ 23h ; EDX=geometry row, BX=phase flags, CX=capacity,
                                ; ES:DI <- DAC indexes; returns CX=width
VM_OP_VIDEO_PRESENT     equ 24h ; ES:DI <-> present packet; target = bound LFB
VM_OP_VIDEO_BAND        equ 25h ; ES:DI <-> present packet; target = band below
VM_OP_VIDEO_HOST_ENTER  equ 26h ; ES:DI -> host packet; banked host drawing
VM_OP_VIDEO_HOST_LEAVE  equ 27h
VM_OP_VIDEO_SHARE       equ 28h ; ES:DI <- share packet; takes a PM reference
VM_OP_VIDEO_UNSHARE     equ 29h ; drops the PM reference taken by SHARE
VM_OP_VIDEO_ARM         equ 2Ah ; ES:DI -> present packet kept by the owner;
                                ; timer-driven bounded LFB presentation
VM_OP_VIDEO_DISARM      equ 2Bh
VM_OP_VIDEO_ACCESS      equ 2Ch ; ES:DI <-> access packet: ordered aperture
VM_OP_VIDEO_DAMAGE      equ 2Eh ; EDX=window height, ECX=band height -> EDX band mask
VM_OP_VIDEO_INT10       equ 2Dh ; HDPMI-owned 0300h bridge; BIOS AX in EAX[31:16]
                                ; byte cycles for a protected-mode adapter

VM_CAP_VGA_TRAPPED      equ 0100h ; V86 memory cycles reach the model
VM_CAP_VGA_BIOS         equ 0200h ; virtual INT 10h over the model
VM_CAP_VGA_PRESENTER    equ 0400h ; bounded LFB/band presenter
VM_CAP_VGA_SHARED       equ 0800h ; model pages shareable with a PM adapter
VM_VIDEO_CAPABILITIES   equ 0F00h

VM_ERROR_VIDEO_FORMAT   equ 20h
VM_ERROR_VIDEO_GEOMETRY equ 21h
VM_ERROR_VIDEO_SHARED   equ 22h ; END refused: a PM adapter still maps the model
VM_ERROR_VIDEO_FATAL    equ 23h ; session poisoned by an unsupported cycle
VM_ERROR_VIDEO_HOST     equ 24h ; host-mode nesting/binding error

; VIDEO_CONFIG packet (64 bytes)
VM_VCFG_MAGIC           equ 043565643h ; bytes CVVC
VM_VCFG_BYTES           equ 64
VM_VCFG_VERSION_OFS     equ 4          ; word VM_ABI_VIDEO_VERSION
VM_VCFG_BYTES_OFS       equ 6          ; word 64
VM_VCFG_TSC_KHZ         equ 8          ; dword, 0 = unknown (poll-count timing)
VM_VCFG_FONT_8X8        equ 12         ; far pointers from real INT 10h/1130h
VM_VCFG_FONT_8X8_HIGH   equ 16
VM_VCFG_FONT_8X14       equ 20
VM_VCFG_FONT_8X16       equ 24
VM_VCFG_FLAGS           equ 28
VM_ABI_VIDEO_VERSION    equ 0100h

; VIDEO_STATE packet (256 bytes), dwords unless noted
VM_VSTATE_MAGIC         equ 053565643h ; bytes CVVS
VM_VSTATE_BYTES         equ 256
VM_VS_ATTACHED          equ 8
VM_VS_GENERATION        equ 12
VM_VS_FAULTS            equ 16
VM_VS_INSTRUCTIONS      equ 20
VM_VS_ELEMENTS          equ 24
VM_VS_UNSUPPORTED       equ 28
VM_VS_BUS_FAULTS        equ 32
VM_VS_PORT_READS        equ 36
VM_VS_PORT_WRITES       equ 40
VM_VS_BIOS_CALLS        equ 44
VM_VS_BIOS_UNSUPPORTED  equ 48
VM_VS_BIOS_LAST_AX      equ 52
VM_VS_MODE_SETS         equ 56
VM_VS_MODE              equ 60
VM_VS_WIDTH             equ 64
VM_VS_ROWS              equ 68
VM_VS_REPEAT            equ 72
VM_VS_TEXT              equ 76
VM_VS_DISPLAY_CHANGES   equ 80
VM_VS_CHANGES           equ 84
VM_VS_FATAL             equ 88
VM_VS_FATAL_CSIP        equ 92
VM_VS_FATAL_BYTES       equ 96         ; 16 bytes
VM_VS_PRESENTS          equ 112
VM_VS_PRESENT_ROWS      equ 116
VM_VS_PRESENT_PIXELS    equ 120
VM_VS_FRAMES            equ 124
VM_VS_FULL_REDRAWS      equ 128
VM_VS_LAST_PRESENT_TSC  equ 132
VM_VS_MAX_PRESENT_TSC   equ 136
VM_VS_TOTAL_PRESENT_TSC equ 140        ; qword
VM_VS_HOST_MODE         equ 148
VM_VS_HOST_BANK         equ 152
VM_VS_HOST_FAULTS       equ 156
VM_VS_HOST_BYTES        equ 160
VM_VS_TSC_KHZ           equ 164
VM_VS_STATUS_POLLS      equ 168
VM_VS_FAULT_TSC         equ 172        ; qword, cycles spent emulating
VM_VS_CRTC_START        equ 180
VM_VS_SHARE_COUNT       equ 184
VM_VS_STATE_BYTES       equ 188        ; sizeof(cvga_state)
VM_VS_HOST_ENTERS       equ 192
VM_VS_PORT_REJECTS      equ 196
VM_VS_LAST_FAULT_CSIP   equ 200
VM_VS_PM_FAULTS         equ 204        ; protected-mode adapter counters
VM_VS_PM_INSTRUCTIONS   equ 208
VM_VS_PM_UNSUPPORTED    equ 212
VM_VS_PM_ATTACHED       equ 216
VM_VS_PASSES            equ 220        ; presenter sweeps completed (see vga_presenter.h)
VM_VS_STRING_IO         equ 224        ; INS/OUTS instructions executed on VGA ports
VM_VS_STRING_ELEMENTS   equ 228
VM_VS_HOST_DISPLAY_SETS equ 232        ; host 4F07 set requests seen in host mode
VM_VS_DAMAGE_QUERIES    equ 236

; PRESENT/BAND packet (256 bytes): cvp_format at 16, cvp_request at 32,
; cvp_stats returned at 180 (see vga_presenter.h; C asserts these sizes).
; BAND renders into the conventional compositor band instead of the LFB:
; request coordinates are band-relative screen coordinates, format.height is
; the band row capacity and the extent is checked against the owned pages.
VM_VPRES_MAGIC          equ 050565643h ; bytes CVVP
VM_VPRES_BYTES          equ 256
VM_VP_VERSION           equ 4          ; word VM_ABI_VIDEO_VERSION
VM_VP_SIZE              equ 6          ; word 256
VM_VP_BAND_SEGMENT      equ 8          ; word, conventional band segment
VM_VP_BAND_TOP          equ 10         ; word, first screen row held by band
VM_VP_BAND_BYTES        equ 12         ; dword, band extent in bytes (<= 64 KiB)
VM_VP_FORMAT            equ 16         ; cvp_format (16 bytes)
VM_VP_REQUEST           equ 32         ; cvp_request (148 bytes)
VM_VP_STATS             equ 180        ; cvp_stats (32 bytes)
VM_VP_INTERVAL_US       equ 212        ; ARM: minimum microseconds between presents
VM_VP_TICKS             equ 216        ; ARM: timer ticks observed (written back)
VM_VP_PRESENTS          equ 220        ; ARM: presents performed (written back)
VM_VP_LAST_TSC          equ 224        ; ARM: cycles of the last present
VM_VP_MAX_TSC           equ 228        ; ARM: maximum cycles of one present

; HOST_ENTER packet (16 bytes)
VM_VHOST_MAGIC          equ 048565643h ; bytes CVVH
VM_VH_GRANULE           equ 4          ; dword, VBE window granularity in bytes
VM_VH_WINDOW            equ 8          ; dword, VBE window position in granules;
                                       ; FFFFFFFFh keeps the previous host window
VM_VH_RESERVED          equ 12

; SHARE packet: magic, page count, then physical page addresses.
VM_VSHARE_MAGIC         equ 048535643h ; bytes CVSH
VM_VSHARE_MAX_PAGES     equ 72
VM_VSHARE_BYTES         equ 8 + VM_VSHARE_MAX_PAGES*4

; ACCESS packet: magic, count (1..64), then count 8-byte entries
; {dword linear address in A0000-BFFFF, byte 0=read/1=write, byte value, word 0}.
; Entries execute in order against the model (reads load latches exactly as
; the guest's own bus cycle would); read results are written back in place.
VM_VACCESS_MAGIC        equ 041565643h ; bytes CVVA
VM_VACCESS_MAX          equ 64
VM_VACCESS_BYTES        equ 8 + VM_VACCESS_MAX*8
