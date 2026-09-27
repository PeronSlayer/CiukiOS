; CiukiOS Jemm V86 session foundation ABI. MASM/NASM-compatible constants.
; Discover with INT 2Fh AX=1684h BX=VM_DEVICE_ID; call returned ES:DI far entry.
; AX=operation. Returned CF=1, AX=VM_ERROR_* on error; otherwise CF=0.
; QUERY with CX=0 returns AX=version, BX=capabilities, DX=active, SI=video mode.
; QUERY with CX>=64 also writes a VM_INFO_SIZE packet to ES:DI.
; READBACK: EDX=aperture byte offset, CX=1..4096, ES:DI=conventional destination.
; BEGIN/END/READBACK success returns AX=0. Guest flat offset may be 32 bits.
VM_DEVICE_ID           equ 04349h
VM_ABI_VERSION         equ 0100h
VM_OP_QUERY            equ 0
VM_OP_BEGIN            equ 1
VM_OP_END              equ 2
VM_OP_READBACK         equ 3
; Trusted host framebuffer transport. BIND uses the 16-byte packet below,
; requires no active session and no existing binding. FB_COPY transfers at
; most 4096 bytes (EDX=byte offset, CX=count, ES:DI=conventional buffer),
; BX=0 reads framebuffer to buffer, BX=1 writes buffer to framebuffer.
; The host must obtain the physical extent from a validated VBE mode result.
; This is not arbitrary guest device discovery or a security boundary.
VM_OP_BIND_FB          equ 4
VM_OP_UNBIND_FB        equ 5
VM_OP_FB_COPY          equ 6
; Optional monitored-host scheduler. BIND_SCHED receives a 256-byte
; cvsched_descriptor at ES:DI while inactive. BEGIN arms the Jemm physical
; IRQ service; END refuses while HDPMI still reports host/client/callback
; ownership. UNBIND_SCHED is legal only after a successful END. DPMI_RELEASE
; is called by the real-mode owner only after INT 2Fh/1687h proves that the
; resident host was removed; it cannot remove a live protected callback.
VM_OP_BIND_SCHED       equ 7
VM_OP_UNBIND_SCHED     equ 8
VM_OP_DPMI_RELEASE     equ 9
; Run-time negotiation of Jemm's V86 virtual-IF profile (Host_Scheduler_Profile
; 1.0). BX=0 query, 1 allow (default), 2 decline. A running session requests
; or releases immediately; otherwise the choice applies at the next BEGIN.
; Returns AX=0, EBX=Jemm flags (bit0 available, bit1 requested, bit2 active,
; bit3 guest IF, bit4 IRQ filter; high word 0100h), CX=1 when allowed.
VM_OP_IF_PROFILE       equ 0Ah
VM_IF_AVAILABLE        equ 1
VM_IF_REQUESTED        equ 2
VM_IF_ACTIVE           equ 4
VM_IF_GUEST_IF         equ 8
VM_IF_FILTER           equ 10h
VM_CAP_SHADOW_APERTURE  equ 1
VM_CAP_BLOCK_VGA_PORTS  equ 2
VM_CAP_BIOS_03_13       equ 4
VM_CAP_READBACK         equ 8
VM_CAP_PM_FB_COPY       equ 10h
VM_CAP_HOST_SCHEDULER   equ 20h
VM_CAP_HDP324_ADAPTER   equ 40h
; Set when the loaded Jemm reports the negotiable V86 virtual-IF profile.
VM_CAP_V86_VIRTUAL_IF   equ 80h
; Planar VGA, guest audio and independent background VMs remain absent.
; The HDPMI bit is added dynamically only while the shipped 3.24 callback is
; actually installed for a client.  Merely compiling or loading the adapter
; is not a successful DPMI handshake.
VM_CAPABILITIES        equ 003Fh
VM_INFO_SIZE           equ 64
VM_INFO_MAGIC          equ 0534D5643h ; bytes CVMS
VM_INFO_VERSION        equ 4
VM_INFO_BYTES          equ 6
VM_INFO_CAPABILITIES   equ 8
VM_INFO_ACTIVE         equ 12
VM_INFO_MODE           equ 16
VM_INFO_GENERATION     equ 20
VM_INFO_IO_COUNT       equ 24
VM_INFO_REJECT_COUNT   equ 28
VM_INFO_OWNER_CR3      equ 32
VM_INFO_SHADOW_BYTES   equ 36
VM_INFO_FB_BOUND       equ 40
VM_INFO_FB_BYTES       equ 44
VM_INFO_FB_PHYSICAL    equ 48
VM_INFO_FATAL_CODE     equ 52
VM_INFO_FATAL_PORT     equ 56
VM_INFO_FATAL_IO_TYPE  equ 60
; Unsupported string I/O invalidates the session; caller must END it.
VM_FATAL_STRING_IO     equ 1
VM_FB_PACKET_SIZE      equ 16
VM_FB_PACKET_MAGIC     equ 042465643h ; bytes CVFB
VM_FB_PACKET_VERSION   equ 4
VM_FB_PACKET_BYTES     equ 6
VM_FB_PACKET_PHYSICAL  equ 8
VM_FB_PACKET_LENGTH    equ 12
; Restricted first profile: reject lower MMIO, RAM and the native XMS pool.
VM_FB_MIN_PHYSICAL     equ 80000000h
VM_FB_MAX_BYTES        equ 04000000h
VM_ERROR_OPERATION     equ 1
VM_ERROR_CONTEXT       equ 2
VM_ERROR_ACTIVE        equ 3
VM_ERROR_INACTIVE      equ 4
VM_ERROR_MEMORY        equ 5
VM_ERROR_TRAP          equ 6
VM_ERROR_ADDRESS       equ 7
VM_ERROR_MAPPING       equ 8
VM_ERROR_ABI           equ 9
VM_ERROR_FB_BOUND      equ 10
VM_ERROR_FB_UNBOUND    equ 11
VM_ERROR_SCHED_BOUND   equ 12
VM_ERROR_SCHED_UNBOUND equ 13
VM_ERROR_DPMI_OWNED    equ 14
