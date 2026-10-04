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
; Bounded row transport for the desktop compositor. ES:DI points to an
; exact 32-byte CVFR packet; CX must be at least 32. Packet sourceSeg:off
; names the guest row buffer. direction 0 reads FB into guest memory and
; requires writable guest pages; direction 1 writes guest memory to FB and
; requires readable guest pages. The operation is system-VM only.
VM_OP_FB_ROWS          equ 11h
; Copy rows between two non-overlapping regions of the bound framebuffer.
; ES:DI points to an exact 32-byte CVFC packet; CX must be at least 32.
VM_OP_FB_PAGE_COPY     equ 12h
; Native GPU presentation: system VM, DX = viewport start scanline in the
; bound framebuffer. Success includes an asynchronous queued presentation;
; callers repeat at completed-frame/idle boundaries to service completion.
; VBE-only bindings return FB_UNBOUND and keep BIOS display-start semantics.
VM_OP_FB_PRESENT       equ 13h
; System-VM read-only native display state. ES:DI destination, CX >=192.
; Header CVGD/version 0100h/size192; u32 fields at 8 backend (0 firmware,
; 1 VirtIO, 2 legacy scanout), 12 phase, 16 width,20 height,24 pitch,28 bpp,
; 32 error,36 submits,40 completions,44 physical,48 bytes,52 EDID bytes,
; 56 active DOS session; reserved60; optional base EDID128 at64.
VM_OP_DISPLAY_INFO     equ 14h
VM_DISPLAY_INFO_SIZE   equ 192
VM_DISPLAY_INFO_MAGIC  equ 044475643h
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
; SCHED_INFO: BX:DX = bound scheduler descriptor (segment:offset, for a
; protected host's -cSSSS:OOOO option), ECX = its CVSCHED_STATE.
VM_OP_SCHED_INFO       equ 0Bh
; Trusted system-VM probe for Jemm-backed, owner-tagged page allocation.
; It allocates, checks, reuses and releases pages inside ring 0 and returns
; no pointer to the V86 caller. This is a precursor to native processes.
VM_OP_NATIVE_PAGE_PROBE equ 0Ch
; Synchronous native CN32 v1 invocation from the system V86 VM. DS:SI is
; the complete image in a conventional block, ECX is its byte length
; (36..61440, no 16-bit offset wrap). The monitor validates and copies the
; payload before switching address spaces. On successful dispatch CF=0,
; EAX=0 normal exit/1 fault/2 budget/3 setup failure/4 busy, EBX=last
; report tag, ECX=last report value, EDX=report count, ESI=fault vector,
; EDI=steps, EBP=exit status when EAX=0, otherwise fault error.
; CF=1 uses VM_ERROR_*.
VM_OP_NATIVE_RUN       equ 0Dh
; Retained native processes. START uses the same validated CN32 DS:SI/ECX
; input and returns EBX=(generation<<8)|(slot+1), EAX=0. STATE takes that
; handle in EBX and returns EAX state (1 ready, 2 running, 3 ended,
; 4 cleanup pending), EBX last report tag, ECX cumulative steps, EDX terminal
; result (0 exit, 1 fault, 3 setup failure, 5 STOP), ESI cumulative reports,
; EDI exit status on normal exit or fault vector otherwise, EBP last report
; value. STOP takes the handle in EBX, releases its pages, and retains an
; ended record until slot reuse. All three return CF=1/VM_ERROR_* on failure.
VM_OP_NATIVE_START     equ 0Eh
VM_OP_NATIVE_STATE     equ 0Fh
VM_OP_NATIVE_STOP      equ 10h
; VM manager (several DOS VMs, session_vmm.inc):
; VMM_INIT   EBX = CiukiDOS kernel segment and ECX = its InDOS offset (INT 2Fh
;            1607h/BX=15h DOSMGR table), EDX = first arena MCB segment (INT
;            21h/52h). Must match the build's kernel layout.
; VMM_CREATE Fork (from \VM\VMFORK.COM): a new VM is a copy of the calling
;            system VM's first megabyte at this call. The caller gets CX = 0
;            (parent) and BX = the new VM; the new VM continues after the
;            same call with CX = 1 (child) and BX = its own number.
; VMM_EXIT   From a VM (not the system VM): BX = exit code; the VM ends.
; VMM_STATE  BX = VMs, CX = current, ECX high word = the VM with the keyboard,
;            EDX = switches, ESI = gated ticks,
;            EDI = last exit code (FFFFh until one ends).
; VMM_FOCUS  BX = the VM that receives the keyboard. When it is the VM that
;            owns a DOS window session, the device model takes the physical
;            keys for it; Ctrl+Esc there gives the keyboard back to the system
;            VM (not seen by the VM). The physical mouse goes with the focus to
;            a session with a mouse, unless CX bit 0 is set: then the system
;            VM (the desktop) keeps it and feeds the session with DEV_MOUSE.
; VMM_KILL   BX = another VM (not the system VM): ends it at once, with its
;            DOS window session; VMM_STATE then reports exit code FFFEh.
; VMM_IVT    ES:DI <- 1024 bytes: the interrupt vectors as they were when the
;            module loaded. VMFORK restores from them, in the new VM, every
;            vector that points into the blocks it frees there.
; VMM_TARGET BX = the VM whose DOS window session this VM's QUERY, READBACK,
;            VIDEO_STATE/READ/RENDER/PRESENT/BAND/DAMAGE, DEV_STATE,
;            DEV_KEY and DEV_MOUSE address (itself: its own). A VM that
;            ended falls back to the caller's own session.
; VMM_LIST   ES:DI <- 4 bytes per VM (VMM_MAX of them): state (0 free,
;            1 running, 2 ended), SESS_* flags of its DOS window session
;            (1 video, 2 devices, 4 audio, 8 keyboard, 10h mouse), 1 when it
;            has the focus, 1 when it runs now (Task Manager).
; VMM_YIELD  From a VM's own V86 IRQ0 handler (after the BIOS tick): a timer
;            tick its DPMI host reflected to it. A host in protected mode owns
;            the IDT, so that tick never entered the monitor's IRQ0 path; it
;            is counted and the time slice ends here as it would there.
;            Ticks the monitor reflected itself are not counted twice. Not
;            for a nested execution (a VM idling in HLT). Returns AX = 0.
; VMM_EXIT_CODE BX = a VM: BX = its state (0 free, 1 running, 2 ended),
;            DX = its exit code (FFFFh while it runs; FFFEh killed) and CX =
;            the creations of that slot, kept until the slot is reused (the
;            desktop's DOS windows tell their VM from a later one).
; VMM_UMB_ALLOC BX = requested paragraphs for the current VM's VMFORK
;            resident copy. On success BX = exact Jemm UMB segment and CX =
;            actual paragraphs. CVSESSION atomically claims and records at
;            most two blocks, releasing them on normal exit or forced kill.
; TSC_KHZ    ECX = the TSC rate in kHz CVSESSION measured in ring 0 against
;            PIT channel 2 when it loaded (no other VM existed), 0 if unknown.
;            A VM's own measurement through a BIOS wait is not reliable: the
;            VM manager runs other VMs while it waits in HLT.
; VMM_INPUT_YIELD From a DPMI host, after it acknowledged a physical IRQ1/
;            IRQ12 that DEV_PHYSICAL_IRQ routed to another VM (BX=2 there):
;            that VM runs now (the DOS/FAT gates apply). Returns AX = 0.
; Each VM can own one DOS window session (BEGIN), with its own VGA and
; device model; its traps and hooks act for it while it runs.
; A VM that began the DOS window session (BEGIN) owns it: its traps and hooks
; act for that VM only. VMM_EXIT or VMM_KILL of that VM ends the session.
VM_OP_VMM_INIT         equ 40h
VM_OP_VMM_CREATE       equ 41h
VM_OP_VMM_EXIT         equ 42h
VM_OP_VMM_STATE        equ 43h
VM_OP_VMM_FOCUS        equ 44h
VM_OP_VMM_KILL         equ 45h
VM_OP_VMM_IVT          equ 46h
VM_OP_VMM_TARGET       equ 47h
VM_OP_VMM_LIST         equ 48h
VM_OP_VMM_YIELD        equ 49h
VM_OP_VMM_EXIT_CODE    equ 4Ah
VM_OP_VMM_UMB_ALLOC    equ 4Bh
VM_OP_VMM_INPUT_YIELD  equ 4Ch
VM_OP_TSC_KHZ          equ 4Dh
; VMM_PRESENT From a DPMI host after its client flipped the VGA start address:
;            an idle desktop VM runs now to present the frame (no boost).
VM_OP_VMM_PRESENT      equ 4Eh
; Physical host tick from HDPMI; BX reports independent-clock ownership.
VM_OP_VMM_CLOCK        equ 4Fh
; Ored into an operation by a DPMI host whose client is single-stepping a
; virtual CLI region: no VM switch at that call's return to V86.
VM_OP_NO_SWITCH        equ 100h
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
VM_CAP_DESKTOP_ROWS    equ 00010000h
; Runtime flag in QUERY's packet, set only for an active native GPU binding.
VM_CAP_GPU_PRESENT    equ 00020000h
VM_CAP_GPU_ASYNC      equ 00040000h
VM_FB_MODE_BANKED     equ 8000h
VM_CAPABILITIES_FULL   equ VM_CAPABILITIES + VM_CAP_DESKTOP_ROWS
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
; Optional extended BIND packet carries firmware mode geometry for native
; legacy scanout drivers. The original 16-byte form remains accepted.
VM_FB_MODE_PACKET_SIZE equ 24
VM_FB_PACKET_MAGIC     equ 042465643h ; bytes CVFB
VM_FB_PACKET_VERSION   equ 4
VM_FB_PACKET_BYTES     equ 6
VM_FB_PACKET_PHYSICAL  equ 8
VM_FB_PACKET_LENGTH    equ 12
; Restricted first profile: reject lower MMIO, RAM and the native XMS pool.
VM_FB_MIN_PHYSICAL     equ 80000000h
VM_FB_MAX_BYTES        equ 04000000h
VM_FB_ROWS_PACKET_SIZE equ 32
VM_FB_ROWS_MAGIC       equ 052465643h ; bytes CVFR
VM_FB_ROWS_SEG         equ 8
VM_FB_ROWS_OFF         equ 10
VM_FB_ROWS_ROW_BYTES   equ 12
VM_FB_ROWS_COUNT       equ 14
VM_FB_ROWS_SRC_STRIDE  equ 16
VM_FB_ROWS_DIRECTION   equ 18
VM_FB_ROWS_DST_PITCH   equ 20
VM_FB_ROWS_DST_OFFSET  equ 24
VM_FB_ROWS_RESERVED    equ 28
VM_FB_PAGE_PACKET_SIZE equ 32
VM_FB_PAGE_MAGIC       equ 043465643h ; bytes CVFC
VM_FB_PAGE_SRC_OFFSET  equ 8
VM_FB_PAGE_DST_OFFSET  equ 12
VM_FB_PAGE_ROW_BYTES   equ 16
VM_FB_PAGE_COUNT       equ 18
VM_FB_PAGE_PITCH       equ 20
VM_FB_PAGE_RESERVED0   equ 24
VM_FB_PAGE_RESERVED1   equ 28
VM_FB_ROWS_MAX_BYTES   equ 04000h
VM_FB_ROWS_MAX_COUNT   equ 64
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
VM_ERROR_VMM_LAYOUT    equ 40h        ; kernel layout differs from the build's
VM_ERROR_VMM_FULL      equ 41h        ; no free VM slot or memory
VM_ERROR_VMM_BUSY      equ 42h        ; IRQ0 host scheduler owned elsewhere
VM_ERROR_VMM_INIT      equ 43h        ; VMM_INIT not done
