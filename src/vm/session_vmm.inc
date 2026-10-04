; CiukiOS VM manager for CVSESSION: several V86 DOS contexts (VMs) on the one
; Jemm V86 monitor, Windows 3.x "386 enhanced" style. Included by
; session_jlm.asm; see docs/design-multi-vm-2026-09-28.md.
;
; Memory: linear pages 00h-9Fh (IVT, BIOS data, the CiukiDOS kernel and the
; DOS arena) are private per VM, as the kernel's Windows instance table asks
; for. The video aperture A0h-BFh has per-VM page-table entries (the desktop's
; LFB bank window, or a DOS window session's trapped shadow). UMBs, ROMs and
; extended memory stay global.
;
; Devices: the DOS window session (VGA model, device model) belongs to the VM
; that began it (session_vm). Its port traps, INT 10h hook, timer presenter
; and scheduler service act for that VM only; the other VMs reach the
; hardware. Virtual IRQs for a VM that is not running are left pending in
; its saved virtual PIC. The keyboard (IRQ1) goes to the focus VM, the mouse
; (IRQ12) to the system VM (the desktop), through Jemm's IRQ1/IRQ12 filter;
; Ctrl+Esc in the session's VM gives the keyboard back to the system VM.
; ISA DMA of the session's VM is read through its own page table entries.
;
; Time: every VM is owed the IRQ0 ticks that reach another VM, and gets them
; when it runs again, so each BIOS clock keeps real time. The CMOS index
; register (port 70h) is kept per VM.
;
; Ending: VMM_EXIT or VMM_KILL of the VM that owns the session ends the
; session on its behalf (vmm_force_end). VMFORK gives a forked VM the module
; load-time value of every vector that points into the blocks it frees.
;
; A VM is created by fork: \VM\VMFORK.COM, EXEC'd normally by the system VM,
; calls VMM_CREATE; the first megabyte is copied as it is at that call, so the
; kernel's own process, allocator and EXEC state are coherent in both copies.
; The system VM gets the parent answer (CX=0) and VMFORK ends there; the new
; VM resumes right after the same API call (the dispatcher already simulated
; its far return) with the child answer (CX=1, BX = its number), frees the
; desktop's blocks in its copy and runs the program.
;
; Switching happens at the IRQ0 host-scheduler callback, where EBP is the
; V86 client frame at the top of Jemm's monitor stack. Only a VM outside DOS
; (InDOS 0) whose single-sector FAT write-back cache is clean is switched away
; from, and the resumed VM's FAT cache is invalidated, so every VM re-reads
; the FAT the others wrote. A VM idling in HLT (Jemm waits for an interrupt in
; ring 0 and resumes it through the frame at the top of its stack) is left
; too, and then also inside DOS when that DOS call is keyboard input
; (01h/06h-08h/0Ah/0Ch), which never leaves a file-system change half done.
; The switch saves and restores the client frame
; (except the interrupt number being reflected), the 160 PTEs and Jemm's
; per-VM interrupt state (profile functions 9/10).

include kernel_layout.inc

VMM_MAX equ 4
VMM_PAGES equ 0A0h                      ; private memory, pages 00h-9Fh
VMM_PTES equ 0C0h                       ; per-VM PTEs, with the video aperture
VMM_BIOS_FIRST equ 0C0h                 ; BIOS RAM in upper memory: private too
VMM_BIOS_END equ 0F0h
VMM_BIOS_MAX equ VMM_BIOS_END-VMM_BIOS_FIRST
VMM_FRAME equ 76                        ; sizeof Client_Reg_Struc
VMM_CV_BYTES equ 64                     ; >= Jemm's CV_VM_STATE_BYTES (26)
; Two ticks let a resumed BIOS INT 16h wait consume its queued key before
; another VM switch. One tick reproducibly stranded VM2 after its prompt.
; The return-to-V86 audio-urgent path below still schedules pending DMA IRQs.
VMM_SLICE equ 2                         ; IRQ0 ticks per time slice
VMM_BOOST equ 4                         ; ticks the system VM keeps after input
VMM_MISSED_MAX equ 1092                 ; ticks owed to a VM at most (1 min)
VMM_CTX_BYTES equ 2048                  ; >= the per-session variables (sess_ctx)
SESS_VIDEO equ 1                        ; VMREC.sess: it owns a session
SESS_DEVICES equ 2                      ; ... with a device model
SESS_AUDIO equ 4                        ; ... that plays through the AC'97
SESS_KBD equ 8                          ; ... that has the keyboard
SESS_MOUSE equ 10h                      ; ... that has the mouse

VM_SLOT_FREE equ 0
VM_SLOT_READY equ 1
VM_SLOT_DEAD equ 2

VMREC struct
state dd ?
cv_valid dd ?                           ; cv holds a real virtual-PIC state
missed dd ?                             ; IRQ0 ticks owed (it was not running)
cmos_index dd ?                         ; last byte it wrote to port 70h
sess dd ?                               ; SESS_* of its DOS window session
vid_inst dd ?                           ; its C video instance block (0: none)
dev_inst dd ?                           ; its C device instance block (0: none)
target dd ?                             ; VM its video/device operations address
pull_lines dd ?                         ; IRQ1/IRQ12 bytes its model is to take
exit_code dd ?                          ; FFFFh running, FFFEh killed, else its code
generation dd ?                         ; creations of this slot (VMM_EXIT_CODE)
umb_resident0 dd ?                      ; exact Jemm XMS UMBs owned by this VM
umb_resident1 dd ?                      ; released at normal exit and forced kill
ctx db VMM_CTX_BYTES dup (?)            ; its per-session variables when not live
block dd ?                              ; linear of its private pages (0: VM 0)
frame db VMM_FRAME dup (?)
cv db VMM_CV_BYTES dup (?)
ptes dd VMM_PTES dup (?)
bios_ptes dd VMM_BIOS_MAX dup (?)       ; its entries for vmm_bios_pages
VMREC ends

.data
vmm_ready dd 0
vmm_kbase dd 0                          ; linear base of the kernel image
vmm_first_mcb dd 0
vmm_count dd 1
vmm_current dd 0
vmm_live dd 0                           ; VM whose interrupt state is Jemm's live one
vmm_installed dd 0
vmm_cmos_trapped dd 0
vmm_stat_magic db 'CVMMSTAT'              ; counters, found in RAM by the gates
vmm_ticks dd 0
vmm_switches dd 0
vmm_gated dd 0
vmm_yields dd 0                         ; switches from VMM_YIELD
vmm_yield_calls dd 0                    ; VMM_YIELD calls
vmm_yield_seen dd 0                     ; ... for ticks the monitor had counted
vmm_last_exit dd 0FFFFh
vmm_idle dd 0                           ; this tick interrupted the HLT wait
vmm_focus dd 0                          ; VM that receives the keyboard
vmm_audio_urgent dd -1                  ; background PM audio IRQ needs CPU time
vmm_mouse_home dd 0                     ; 1: the system VM keeps the mouse (M4)
vmm_seen dd 0                           ; bit per VM: the monitor reflects the
                                        ; IRQ0 it gets next (VMM_YIELD)
tick_owners dd 0                        ; TICK_SCHEDULER | TICK_VMM
ivt_snapshot dd 256 dup (0)             ; interrupt vectors at module load (VMSTART)
guest_window dd 0                       ; linear page mapped onto another VM's page
guest_window_pte dd 0                   ; its own PTE, restored on unload
guest_window_frame dd 0                 ; frame mapped there now (0: its own)
vmm_bios_count dd 0                     ; upper-memory pages of BIOS RAM
vmm_bios_pages db VMM_BIOS_MAX dup (0)  ; their page numbers
vmm_input_magic db 'CVINPUT1'            ; input scheduling state, found in RAM
vmm_input_urgent dd -1                  ; VM with keyboard/mouse input pending
vmm_boost_vm dd -1                      ; system VM handling input: no slice end
vmm_boost_end dd 0                      ; vmm_ticks at which that boost ends
vmm_halt_switches dd 0                  ; switches at a guest HLT
vmm_halt_calls dd 0                     ; HLT callbacks
vmm_resume_vm dd -1                     ; boosted VM urgent audio interrupted
vmm_resume_tsc dd 0                     ; TSC (low) at that switch
vmm_resuming dd 0                       ; input_urgent holds a resume
vmm_idle_mask dd 0                      ; bit per VM: it gave the CPU away at HLT
vmm_desk_tsc dd 0                       ; TSC (low) when the desktop yielded
VMM_DESK_MS equ 8                       ; idle desktop looks for new frames
vmm_tsc_khz dd 0                        ; from DEV_BEGIN (0: unknown)
vmm_tsc_measured dd 0                   ; ring-0 PIT calibration at load
vmm_poll_no_switch dd 0                 ; that API call's client steps a virtual CLI
vmm_poll_input_skip dd 0                ; the return of DEV_PHYSICAL_IRQ: its
                                        ; DPMI host has not acknowledged the IRQ
vmm_table VMREC VMM_MAX dup (<>)
; A regular slice that expired inside a DPMI host's physical ISR.  The
; canonical V86 frame may switch only after that host has issued its EOI.
vmm_defer_magic db 'CVDEFER1'
vmm_defer_vm dd -1
vmm_defer_irqs dd 0
vmm_defer_switches dd 0

.code

; ESI = record of VM number EAX.
vmm_record proc
 imul esi,eax,sizeof VMREC
 add esi,offset vmm_table
 ret
vmm_record endp

vmm_init proc uses esi edi ebx
 mov eax,VM_ERROR_VMM_LAYOUT
 if KL_PRESENT eq 0
 ret
 else
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_ECX
 cmp ecx,KL_INDOS
 jne done
 movzx ebx,word ptr [ebp].Client_Reg_Struc.Client_EBX
 shl ebx,4
 cmp ebx,vmm_kbase
 je same_kernel
 cmp vmm_ready,0
 jne done                               ; initialised for another kernel
same_kernel:
 mov vmm_kbase,ebx
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EDX
 test eax,eax
 jz done_layout
 mov vmm_first_mcb,eax
 cmp vmm_ready,0
 jne ok
 call guest_window_init
 mov eax,VM_ERROR_MEMORY
 jc done
 xor eax,eax
 call vmm_record
 mov [esi].VMREC.state,VM_SLOT_READY
 mov [esi].VMREC.block,0
 lea edi,[esi].VMREC.ptes
 mov esi,PAGE_MAP
 mov ecx,VMM_PTES
 cld
 rep movsd
 call bios_ram_scan
 xor eax,eax
 call vmm_record
 call bios_ptes_out
 mov vmm_ready,1
ok:
 xor eax,eax
 ret
done_layout:
 mov eax,VM_ERROR_VMM_LAYOUT
done:
 ret
 endif
vmm_init endp

vmm_create proc uses esi edi ebx
 cmp vmm_ready,0
 je not_ready
 cmp vmm_current,0                      ; only the system VM forks
 jne full
 mov eax,1
find_slot:
 cmp eax,VMM_MAX
 jae full
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_FREE
 je slot_found
 inc eax
 jmp find_slot
slot_found:
 push eax                               ; preserve slot across CMOS copy
 mov ebx,esi                            ; EBX = new record
 mov [ebx].VMREC.missed,0
 push esi
 mov esi,vmm_current                    ; the creator's CMOS index
 imul esi,esi,sizeof VMREC
 mov eax,vmm_table[esi].cmos_index
 pop esi
 mov [ebx].VMREC.cmos_index,eax
 mov ecx,vmm_bios_count
 add ecx,VMM_PAGES
 push 0
 push ecx
 push PR_SYSTEM
 @VMMCall _PageReserve
 add esp,12
 cmp eax,-1
 je memory_error_pop
 mov [ebx].VMREC.block,eax
 shr eax,12
 mov ecx,vmm_bios_count
 add ecx,VMM_PAGES
 push PC_FIXED or PC_WRITEABLE
 push 0
 push PD_FIXEDZERO
 push ecx
 push eax
 @VMMCall _PageCommit
 add esp,20
 test eax,eax
 jz commit_error_pop
 ; The calling VM's first megabyte, as it is now.
 xor esi,esi
 mov edi,[ebx].VMREC.block
 mov ecx,VMM_PAGES*1024
 cld
 rep movsd
 ; PTEs: the new pages with the system VM's attribute bits.
 mov esi,[ebx].VMREC.block
 shr esi,12
 lea esi,[PAGE_MAP+esi*4]
 xor ecx,ecx
pte_next:
 mov eax,[esi+ecx*4]
 and eax,0FFFFF000h
 mov edx,[PAGE_MAP+ecx*4]
 and edx,0FFFh
 or eax,edx
 mov [ebx].VMREC.ptes[ecx*4],eax
 inc ecx
 cmp ecx,VMM_PAGES
 jb pte_next
 call bios_ram_fork
 ; The video aperture as the calling VM maps it, without its session: a
 ; session's trapped shadow is that session's alone.
pte_aperture:
 mov eax,[PAGE_MAP+ecx*4]
 cmp mapped,0
 je pte_live
 mov eax,saved_ptes[ecx*4-VGA_FIRST_PAGE*4]
pte_live:
 mov [ebx].VMREC.ptes[ecx*4],eax
 inc ecx
 cmp ecx,VMM_PTES
 jb pte_aperture
 ; No session of its own yet.
 push esi
 mov esi,ebx
 call ctx_clear
 pop esi
 mov [ebx].VMREC.sess,0
 mov [ebx].VMREC.pull_lines,0
 mov [ebx].VMREC.exit_code,0FFFFh
 mov [ebx].VMREC.umb_resident0,0
 mov [ebx].VMREC.umb_resident1,0
 inc [ebx].VMREC.generation
 mov eax,[esp]
 mov [ebx].VMREC.target,eax
 btr vmm_seen,eax
 ; Its registers: this very call returning the child answer (the dispatcher
 ; simulated the far return already; success is AX=0 with CF clear).
 mov esi,ebp
 lea edi,[ebx].VMREC.frame
 mov ecx,VMM_FRAME/4
 rep movsd
 mov eax,[esp]                          ; VM number
 lea edi,[ebx].VMREC.frame
 mov word ptr [edi+28],0                ; Client_EAX
 mov word ptr [edi+16],ax               ; Client_EBX
 mov word ptr [edi+24],1                ; Client_ECX
 and dword ptr [edi+48],not 1           ; Client_EFlags.CF
 ; Interrupt state: the system VM's virtual PIC, nothing pending.
 ; Before the profile is active there is none yet: the first switch into the
 ; VM takes the leaving VM's (bases, masks), never an all-zero PIC whose
 ; vector base 0 would inject IRQ0 as INT 0.
 mov [ebx].VMREC.cv_valid,0
 lea ecx,[ebx].VMREC.cv
 mov eax,9
 @VMMCall Host_Scheduler_Profile
 jc cv_ready
 mov dword ptr [ebx].VMREC.cv+4,0       ; pending
 mov dword ptr [ebx].VMREC.cv+8,0       ; in service
 mov [ebx].VMREC.cv_valid,1
cv_ready:
 call vmm_arm
 test eax,eax
 jnz arm_error_pop
 pop eax
 call clock_begin_vm
 mov [ebx].VMREC.state,VM_SLOT_READY
 inc vmm_count
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,ax
 mov word ptr [ebp].Client_Reg_Struc.Client_ECX,0
 xor eax,eax
 ret
arm_error_pop:
 push eax
 call vmm_free_block
 pop eax
 pop ecx
 ret
commit_error_pop:
 call vmm_free_block
memory_error_pop:
 pop eax
full:
 mov eax,VM_ERROR_VMM_FULL
 ret
not_ready:
 mov eax,VM_ERROR_VMM_INIT
 ret
vmm_create endp

; EBX = record: free its private pages.
vmm_free_block proc
 mov eax,[ebx].VMREC.block
 test eax,eax
 jz freed
 push 0
 push eax
 @VMMCall _PageFree
 add esp,8
 mov [ebx].VMREC.block,0
freed:
 ret
vmm_free_block endp

; BIOS RAM in upper memory (pages C0h-EFh the BIOS left writable in place,
; not Jemm's): the BIOS keeps state there across a call, e.g. SeaBIOS's
; extra stack and its stack position, and a VM may be left in the middle of
; a BIOS call (INT 16h waiting for a key). Each VM has its own copy, like
; its first 640 KB. Into vmm_bios_pages.
bios_ram_scan proc uses esi ebx
 mov vmm_bios_count,0
 mov ebx,VMM_BIOS_FIRST
next_page:
 mov eax,[PAGE_MAP+ebx*4]
 test al,1                              ; present
 jz skip
 test al,2                              ; writable
 jz skip
 and eax,0FFFFF000h
 mov esi,ebx
 shl esi,12
 cmp eax,esi
 jne skip                               ; Jemm's (UMB, page frame)
 pushfd
 cli
 mov eax,[esi]
 mov ecx,eax
 not ecx
 mov [esi],ecx
 mov edx,[esi]
 mov [esi],eax
 popfd
 cmp edx,ecx
 jne skip                               ; ROM or nothing
 mov ecx,vmm_bios_count
 mov vmm_bios_pages[ecx],bl
 inc vmm_bios_count
skip:
 inc ebx
 cmp ebx,VMM_BIOS_END
 jb next_page
 ret
bios_ram_scan endp

; EBX = new record (its block committed): its copy of the BIOS RAM pages as
; they are now, and its entries for them. Preserves every register.
bios_ram_fork proc
 pushad
 xor edx,edx
next_page:
 cmp edx,vmm_bios_count
 jae done
 movzx esi,vmm_bios_pages[edx]
 mov eax,[PAGE_MAP+esi*4]
 and eax,0FFFh
 push eax                               ; its attribute bits
 shl esi,12
 lea edi,[edx+VMM_PAGES]
 shl edi,12
 add edi,[ebx].VMREC.block
 mov eax,edi
 mov ecx,1024
 cld
 rep movsd
 shr eax,12
 mov eax,[PAGE_MAP+eax*4]
 and eax,0FFFFF000h
 pop ecx
 or eax,ecx
 mov [ebx].VMREC.bios_ptes[edx*4],eax
 inc edx
 jmp next_page
done:
 popad
 ret
bios_ram_fork endp

; ESI = record: the live entries of the BIOS RAM pages into it.
bios_ptes_out proc uses ecx edx
 xor ecx,ecx
next_page:
 cmp ecx,vmm_bios_count
 jae done
 movzx edx,vmm_bios_pages[ecx]
 mov edx,[PAGE_MAP+edx*4]
 mov [esi].VMREC.bios_ptes[ecx*4],edx
 inc ecx
 jmp next_page
done:
 ret
bios_ptes_out endp

; ESI = record: its entries for the BIOS RAM pages live (the caller reloads
; CR3).
bios_ptes_in proc uses ecx edx eax
 xor ecx,ecx
next_page:
 cmp ecx,vmm_bios_count
 jae done
 movzx edx,vmm_bios_pages[ecx]
 mov eax,[esi].VMREC.bios_ptes[ecx*4]
 mov [PAGE_MAP+edx*4],eax
 inc ecx
 jmp next_page
done:
 ret
bios_ptes_in endp

; The IRQ0 host scheduler: our own callback when nothing else owns it, and
; Jemm's interrupt profile (per-VM IF and virtual PIC). EAX=0 or error.
vmm_arm proc
 cmp vmm_installed,0
 jne armed
 mov eax,TICK_VMM
 call tick_acquire
 jc busy
 call clock_arm
 jc clock_failed
 mov vmm_installed,1
 call vmm_profile_start
 call vmm_cmos_trap
armed:
 xor eax,eax
 ret
clock_failed:
 mov eax,TICK_VMM
 call tick_release
busy:
 mov eax,VM_ERROR_VMM_BUSY
 ret
vmm_arm endp

; Install the profile callbacks together, also used to undo a failed park.
; In the pinned profile, setters 1/4/6/11 unconditionally store their value
; and return CF clear. Restoring them requires no new resource acquisition.
vmm_profile_start proc
 mov ecx,clock_enabled
 mov eax,12
 @VMMCall Host_Scheduler_Profile
 mov eax,1
 @VMMCall Host_Scheduler_Profile
 ; Keyboard and mouse routing between the VMs, owed timer ticks.
 mov ecx,offset jlm_filter
 mov eax,4
 @VMMCall Host_Scheduler_Profile
 mov ecx,offset jlm_poll
 mov eax,6
 @VMMCall Host_Scheduler_Profile
 mov ecx,offset vmm_halt
 mov eax,11
 @VMMCall Host_Scheduler_Profile
 ret
vmm_profile_start endp

; Return the sole desktop to Jemm's ordinary V86 interrupt path. The first
; fork/native start re-arms it; unloading CVSESSION is not required. Called
; only from the canonical poll after owed ticks and native work are done.
vmm_park proc
 pushfd
 cli
 pushad
 cmp vmm_installed,0
 je keep
 cmp vmm_count,1
 jne keep
 cmp vmm_current,0
 jne keep
 cmp vmm_live,0
 jne keep
 cmp vmm_borrowed,-1
 jne keep
 cmp ebp,vmm_client_frame
 jne keep
 cmp sessions,0
 jne keep
 cmp dev_sessions,0
 jne keep
 cmp dev_active,0
 jne keep
 cmp dev_busy,0
 jne keep
 cmp dev_stream,0
 jne keep
 cmp dev_audio_sessions,0
 jne keep
 cmp scheduler_installed,0
 jne keep
 cmp tick_owners,TICK_VMM
 jne keep
 cmp native_page_live,0                 ; native tasks still need jlm_poll
 jne keep
 cmp nproc_busy,0
 jne keep
 cmp [vmm_table].VMREC.missed,0
 jne keep
 test [ebp].Client_Reg_Struc.Client_EFlags,200h
 jz keep                               ; do not withdraw a virtual CLI
 call vmm_phys_idle
 jnz keep
 mov eax,5
 @VMMCall Host_Scheduler_Profile
 jc keep
 or ecx,edx                            ; pending or in-service virtual IRQ
 jnz keep                              ; its handler/EOI must finish first
 mov eax,2
 @VMMCall Host_Scheduler_Profile
 jc keep
 mov eax,TICK_VMM
 call tick_release
 jc profile_restore                    ; the tick owner is still retained
 call clock_stop
 mov vmm_installed,0
 mov vmm_focus,0
 mov vmm_mouse_home,0
 mov vmm_audio_urgent,-1
 mov vmm_input_urgent,-1
 mov vmm_boost_vm,-1
 mov vmm_boost_end,0
 mov vmm_resume_vm,-1
 mov vmm_resuming,0
 mov vmm_idle,0
 mov vmm_idle_mask,0
 mov vmm_seen,0
 mov vmm_poll_no_switch,0
 mov vmm_poll_input_skip,0
 mov vmm_defer_vm,-1
 mov cvdev_release_request,0
 mov [vmm_table].VMREC.target,0
 mov [vmm_table].VMREC.pull_lines,0
 mov [vmm_table].VMREC.cv_valid,0        ; next activation uses the live PIC
 jmp keep
profile_restore:
 ; tick_release keeps the owner bit on error. Reinstall the profile hooks
 ; while IF is still clear; the next canonical return can retry the park.
 call vmm_profile_start
keep:
 popad
 popfd
 ret
vmm_park endp

; Claim and register one exact Jemm UMB in the same ring-0 operation.  A KILL
; cannot intervene between allocation and ownership as it could with two
; separate V86 calls.  The block is private by address: each VM gets a
; distinct Jemm UMB segment even though upper-memory PTEs stay global.
vmm_umb_alloc proc uses esi edx
 mov eax,vmm_current
 test eax,eax
 jz reject
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne reject
 movzx edx,word ptr [ebp].Client_Reg_Struc.Client_EBX
 cmp edx,20h
 jb reject
 cmp edx,500h
 ja reject
 cmp [esi].VMREC.umb_resident0,0
 je slot_ready
 cmp [esi].VMREC.umb_resident1,0
 jne reject
slot_ready:
 @VMMCall Claim_Ciuki_UMB
 test eax,eax
 jz reject
 cmp eax,0CD00h
 jb release_invalid
 cmp eax,0EC00h
 jae release_invalid
 cmp [esi].VMREC.umb_resident0,0
 jne store_second
 mov [esi].VMREC.umb_resident0,eax
 jmp publish
store_second:
 mov [esi].VMREC.umb_resident1,eax
publish:
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,ax
 mov word ptr [ebp].Client_Reg_Struc.Client_ECX,cx
 xor eax,eax
 ret
release_invalid:
 mov edx,eax
 @VMMCall Release_Ciuki_UMB
reject:
 mov eax,VM_ERROR_OPERATION
 ret
vmm_umb_alloc endp

; EBX = VM record.  The pinned Jemm service checks that each segment is a
; currently allocated UMB and returns one only when it cleared that entry.
; Keep a failed segment recorded so a later KILL can retry; never silently
; discard ownership of a global UMB allocation.
vmm_umb_release proc uses edx
 mov edx,[ebx].VMREC.umb_resident1
 test edx,edx
 jz first
 @VMMCall Release_Ciuki_UMB
 cmp eax,1
 jne failed
 mov [ebx].VMREC.umb_resident1,0
first:
 mov edx,[ebx].VMREC.umb_resident0
 test edx,edx
 jz done
 @VMMCall Release_Ciuki_UMB
 cmp eax,1
 jne failed
 mov [ebx].VMREC.umb_resident0,0
done:
 xor eax,eax
 ret
failed:
 mov eax,VM_ERROR_OPERATION
 ret
vmm_umb_release endp

; Defensive unload check: a VM should have released both blocks before its
; state becomes free, but JLOAD must never drop the owner table if it has not.
vmm_umb_live proc uses esi ecx
 mov esi,offset vmm_table
 mov ecx,VMM_MAX
scan:
 cmp [esi].VMREC.umb_resident0,0
 jne live
 cmp [esi].VMREC.umb_resident1,0
 jne live
 add esi,sizeof VMREC
 loop scan
 xor eax,eax
 ret
live:
 mov eax,1
 ret
vmm_umb_live endp

vmm_exit proc
 mov eax,vmm_current
 test eax,eax
 jz refuse                              ; the system VM does not exit
 cmp active,0                           ; its DOS window session ends here
 je no_session
 call vmm_force_end
 test eax,eax
 jnz exit_failed
 mov eax,vmm_current
no_session:
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_EBX
 push esi
 push ebx
 call vmm_record
 mov ebx,esi
 call vmm_umb_release
 test eax,eax
 jnz release_failed
 pop ebx
 mov vmm_last_exit,ecx
 mov [esi].VMREC.state,VM_SLOT_DEAD
 mov [esi].VMREC.exit_code,ecx
 pop esi
 xor eax,eax
 ret
release_failed:
 pop ebx
 pop esi
 ret
refuse:
 mov eax,VM_ERROR_OPERATION
 ret
exit_failed:
 ret
vmm_exit endp

; VMM_KILL: BX = a VM other than the caller and the system VM. Its DOS
; window session is torn down on its behalf and the VM is released at once.
; Like Windows' terminate: a file it was writing may lose its last changes.
vmm_kill proc uses esi ebx
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 test eax,eax
 jz refuse
 cmp eax,VMM_MAX
 jae refuse
 cmp eax,vmm_current
 je refuse
 mov ebx,eax
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_FREE
 je refuse
 test [esi].VMREC.sess,SESS_VIDEO
 jz no_session
 mov eax,ebx
 call vmm_force_end
 test eax,eax
 jnz failed
no_session:
 mov eax,ebx
 call vmm_record
 push ebx
 mov ebx,esi
 call vmm_umb_release
 pop ebx
 test eax,eax
 jnz failed
 cmp ebx,vmm_focus
 jne focus_kept
 mov vmm_focus,0
 call vmm_input_route
focus_kept:
 mov eax,ebx
 call vmm_record
 push ebx
 mov ebx,esi
 call vmm_free_block
 call vmm_free_instances
 mov [ebx].VMREC.state,VM_SLOT_FREE
 mov [ebx].VMREC.cv_valid,0
 mov [ebx].VMREC.exit_code,0FFFEh
 pop ebx
 dec vmm_count
 mov vmm_last_exit,0FFFEh               ; killed
 xor eax,eax
 ret
refuse:
 mov eax,VM_ERROR_OPERATION
failed:
 ret
vmm_kill endp

; EAX = the VM that owns the DOS window session: end it on that VM's behalf.
; Its page table entries are mapped (and vmm_current names it) while the
; normal teardown runs; none of its code runs. EAX = 0 or VM_ERROR_*.
vmm_force_end proc uses esi edi ebx
 mov ebx,eax
 cmp ebx,vmm_current
 je teardown_here
 ; This VM's entries and session variables out, the owner's in.
 mov eax,vmm_current
 call vmm_record
 call ctx_save
 call bios_ptes_out
 lea edi,[esi].VMREC.ptes
 mov esi,PAGE_MAP
 mov ecx,VMM_PTES
 cld
 rep movsd
 mov eax,ebx
 call vmm_record
 call bios_ptes_in
 lea esi,[esi].VMREC.ptes
 mov edi,PAGE_MAP
 mov ecx,VMM_PTES
 rep movsd
 mov eax,cr3
 mov cr3,eax
 push vmm_current
 mov vmm_current,ebx
 mov eax,ebx
 call vmm_record
 call ctx_load
 mov eax,ebx
 call vmm_select
 call force_teardown
 pop ecx
 mov vmm_current,ecx
 push eax
 ; The owner's entries and variables (changed by the teardown) back to its
 ; record, ours in.
 mov eax,ebx
 call vmm_record
 call ctx_save
 call bios_ptes_out
 lea edi,[esi].VMREC.ptes
 mov esi,PAGE_MAP
 mov ecx,VMM_PTES
 cld
 rep movsd
 mov eax,vmm_current
 call vmm_record
 call ctx_load
 call bios_ptes_in
 lea esi,[esi].VMREC.ptes
 mov edi,PAGE_MAP
 mov ecx,VMM_PTES
 rep movsd
 mov eax,cr3
 mov cr3,eax
 mov eax,vmm_current
 call vmm_select
 pop eax
 ret
teardown_here:
 call force_teardown
 ret
vmm_force_end endp

; The session's normal teardown, without the checks for a live protected-mode
; host (it dies with its VM). The scheduler descriptor lived in that VM.
force_teardown proc
 call rollback
 test eax,eax
 jnz done
 mov scheduler_descriptor,0
done:
 ret
force_teardown endp

; VMM_TARGET: BX = the VM whose session this VM's read and input operations
; address (itself: its own session).
vmm_set_target proc uses esi
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 cmp eax,VMM_MAX
 jae bad
 cmp eax,vmm_current
 je set
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne bad
set:
 mov ecx,eax
 mov eax,vmm_current
 call vmm_record
 mov [esi].VMREC.target,ecx
 xor eax,eax
 ret
bad:
 mov eax,VM_ERROR_OPERATION
 ret
vmm_set_target endp

; Operation EAX from the running VM: when it is one that reads or feeds a
; session and the caller addresses another VM's (VMM_TARGET), that VM's
; session variables and instances are live for the call. The caller's own
; memory stays mapped (its buffers). Preserves every register.
vmm_target_enter proc
 pushad
 cmp eax,VM_OP_QUERY
 je borrowable
 cmp eax,VM_OP_READBACK
 je borrowable
 cmp eax,VM_OP_VIDEO_STATE
 jb done
 cmp eax,VM_OP_VIDEO_BAND
 jbe borrowable                         ; STATE, READ, RENDER, PRESENT, BAND
 cmp eax,VM_OP_VIDEO_DAMAGE
 je borrowable
 cmp eax,VM_OP_DEV_STATE
 je borrowable
 cmp eax,VM_OP_DEV_KEY
 je borrowable
 cmp eax,VM_OP_DEV_MOUSE
 je borrowable
 jmp done
borrowable:
 mov eax,vmm_current
 call vmm_record
 mov ebx,[esi].VMREC.target
 cmp ebx,eax
 je done
 cmp ebx,VMM_MAX
 jae done
 mov edi,esi                            ; caller's record
 mov eax,ebx
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne done                               ; gone: the caller's own session
 xchg esi,edi
 call ctx_save                          ; the caller's variables out
 mov esi,edi
 call ctx_load                          ; the target's in
 mov eax,ebx
 call vmm_select
 mov vmm_borrowed,ebx
done:
 popad
 ret
vmm_target_enter endp

; After such a call: the target's variables back to its record, the
; caller's live again. Preserves every register.
vmm_target_leave proc
 pushad
 mov eax,vmm_borrowed
 cmp eax,-1
 je done
 mov vmm_borrowed,-1
 call vmm_record
 call ctx_save
 mov eax,vmm_current
 call vmm_record
 call ctx_load
 mov eax,vmm_current
 call vmm_select
done:
 popad
 ret
vmm_target_leave endp

; ---- Several sessions: per-VM state of this module ----

; The per-session variables (address, bytes): live in the globals for the VM
; CVSESSION acts for, saved in the VMREC of every other VM.
.data
sess_ctx dd offset shadow, 12                   ; shadow, active, mapped
 dd offset fatal_status, 12                     ; status, port, I/O type
 dd offset video_mode, 4
 dd offset saved_ptes, VGA_PAGES*4
 dd offset port_bytes, (offset info_packet) - (offset port_bytes)
 dd offset video_block, 4
 dd offset dev_active, 4
 dd offset dev_pm_claimed, 16                   ; claimed .. refused
 dd offset dev_granted, 8                       ; granted, audio
 dd offset dev_ports, 8                         ; ports, trapped
 dd offset scheduler_descriptor, 16             ; descriptor .. if_flags
 dd 0
; Position of dev_pm_claimed in VMREC.ctx, matching the byte spans above.
VMM_CTX_PM_OFFSET equ 12+12+4+VGA_PAGES*4+(offset info_packet)-(offset port_bytes)+4+4
sessions dd 0                           ; VMs that own a DOS window session
.code

; ESI = record: the live per-session variables into its ctx.
ctx_save proc uses esi edi ebx ecx
 lea edi,[esi].VMREC.ctx
 mov ebx,offset sess_ctx
 cld
next_var:
 mov esi,[ebx]
 test esi,esi
 jz done
 mov ecx,[ebx+4]
 rep movsb
 add ebx,8
 jmp next_var
done:
 ret
ctx_save endp

; ESI = record: its ctx into the live per-session variables.
ctx_load proc uses esi edi ebx ecx
 lea esi,[esi].VMREC.ctx
 mov ebx,offset sess_ctx
 cld
next_var:
 mov edi,[ebx]
 test edi,edi
 jz done
 mov ecx,[ebx+4]
 rep movsb
 add ebx,8
 jmp next_var
done:
 ret
ctx_load endp

; A new VM (or a freed session) has no session: variables at their idle
; values in its ctx. ESI = record.
ctx_clear proc uses edi ecx eax
 lea edi,[esi].VMREC.ctx
 mov ecx,VMM_CTX_BYTES
 xor eax,eax
 cld
 rep stosb
 mov dword ptr [esi].VMREC.ctx+12+12,3  ; video_mode 3 (after shadow.., fatal..)
 ret
ctx_clear endp

; EAX = VM: the C instances of its session are the ones the models act on.
; Preserves every register.
vmm_select proc
 pushad
 call vmm_record
 mov ecx,[esi].VMREC.vid_inst
 mov video_args,ecx
 push eax
 mov eax,offset cvvid_select
 mov ecx,1
 call video_call
 pop eax
 mov ecx,[esi].VMREC.dev_inst
 mov dev_args,ecx
 mov [dev_args+4],eax
 mov eax,offset cvdev_select
 mov ecx,2
 call dev_call
 popad
 ret
vmm_select endp

; ECX = bytes: a zero-filled block of system pages in EAX, or 0.
vmm_alloc_block proc uses esi
 add ecx,4095
 shr ecx,12
 mov esi,ecx
 push 0
 push ecx
 push PR_SYSTEM
 @VMMCall _PageReserve
 add esp,12
 cmp eax,-1
 je failed
 push eax
 shr eax,12
 push PC_FIXED or PC_WRITEABLE
 push 0
 push PD_FIXEDZERO
 push esi
 push eax
 @VMMCall _PageCommit
 add esp,20
 pop ecx
 test eax,eax
 jz free_block
 mov eax,ecx
 ret
free_block:
 push 0
 push ecx
 @VMMCall _PageFree
 add esp,8
failed:
 xor eax,eax
 ret
vmm_alloc_block endp

; The running VM's video instance, allocated once. EAX = 0 or VM_ERROR_*.
vmm_video_instance proc uses esi
 mov eax,vmm_current
 call vmm_record
 cmp [esi].VMREC.vid_inst,0
 jne ready
 mov eax,offset cvvid_instance_bytes
 xor ecx,ecx
 call video_call
 mov ecx,eax
 call vmm_alloc_block
 test eax,eax
 jz nomem
 mov [esi].VMREC.vid_inst,eax
 mov eax,vmm_current
 call vmm_select
ready:
 xor eax,eax
 ret
nomem:
 mov eax,VM_ERROR_MEMORY
 ret
vmm_video_instance endp

; The running VM's device instance, allocated once. EAX = 0 or VM_ERROR_*.
vmm_device_instance proc uses esi
 mov eax,vmm_current
 call vmm_record
 cmp [esi].VMREC.dev_inst,0
 jne ready
 mov eax,offset cvdev_instance_bytes
 xor ecx,ecx
 call dev_call
 mov ecx,eax
 call vmm_alloc_block
 test eax,eax
 jz nomem
 mov [esi].VMREC.dev_inst,eax
 mov eax,vmm_current
 call vmm_select
ready:
 xor eax,eax
 ret
nomem:
 mov eax,VM_ERROR_MEMORY
 ret
vmm_device_instance endp

; ESI = record of a VM being released: its instance blocks go.
vmm_free_instances proc uses eax ecx
 mov eax,[esi].VMREC.vid_inst
 test eax,eax
 jz no_video
 push 0
 push eax
 @VMMCall _PageFree
 add esp,8
 mov [esi].VMREC.vid_inst,0
no_video:
 mov eax,[esi].VMREC.dev_inst
 test eax,eax
 jz no_devices
 push 0
 push eax
 @VMMCall _PageFree
 add esp,8
 mov [esi].VMREC.dev_inst,0
no_devices:
 mov [esi].VMREC.sess,0
 ret
vmm_free_instances endp

; Guest byte PHYSICAL (< A0000h) of VM VM as a linear address
; valid now, or 0. The running VM's own memory is linear; another VM's page
; is mapped through the one-page guest window from its saved page table
; entries. cdecl for the device model (ISA DMA reads).
cvdev_guest_linear proc c public vm:dword, physical:dword
 mov eax,physical
 cmp eax,0A0000h
 jae none
 push esi
 push edx
 push ecx
 mov ecx,vmm_current
 cmp ecx,vm
 je done                                ; that VM is mapped: linear
 cmp guest_window,0
 je fail
 mov edx,eax
 shr edx,12
 mov eax,vm
 cmp eax,VMM_MAX
 jae fail
 call vmm_record
 mov ecx,[esi].VMREC.ptes[edx*4]
 test cl,1
 jz fail
 and ecx,0FFFFF000h
 cmp ecx,guest_window_frame
 je window_ready
 mov guest_window_frame,ecx
 or ecx,3                               ; present, writable, supervisor
 mov eax,guest_window
 shr eax,12
 mov [PAGE_MAP+eax*4],ecx
 mov eax,guest_window
 db 0Fh,01h,38h                         ; INVLPG [EAX]
window_ready:
 mov eax,physical
 and eax,0FFFh
 add eax,guest_window
 jmp done
fail:
 xor eax,eax
done:
 pop ecx
 pop edx
 pop esi
 ret
none:
 xor eax,eax
 ret
cvdev_guest_linear endp

; The interrupt vectors as they are when the module loads (VMSTART runs it
; before the desktop hooks anything): the defaults a forked VM gets back for
; vectors that point into the blocks VMFORK frees there.
vmm_snapshot_ivt proc
 push esi
 push edi
 push ecx
 xor esi,esi
 mov edi,offset ivt_snapshot
 mov ecx,256
 cld
 rep movsd
 pop ecx
 pop edi
 pop esi
 ret
vmm_snapshot_ivt endp

; One linear page for cvdev_guest_linear, taken at VMM_INIT. CF=1 on failure.
guest_window_init proc
 cmp guest_window,0
 jne ready
 push 0
 push 1
 push PR_SYSTEM
 @VMMCall _PageReserve
 add esp,12
 cmp eax,-1
 je failed
 push eax
 shr eax,12
 push PC_FIXED or PC_WRITEABLE
 push 0
 push PD_FIXEDZERO
 push 1
 push eax
 @VMMCall _PageCommit
 add esp,20
 pop ecx
 test eax,eax
 jz free_failed
 mov guest_window,ecx
 shr ecx,12
 mov eax,[PAGE_MAP+ecx*4]
 mov guest_window_pte,eax
 mov guest_window_frame,0
ready:
 clc
 ret
free_failed:
 push 0
 push ecx
 @VMMCall _PageFree
 add esp,8
failed:
 stc
 ret
guest_window_init endp

; Give the guest window its own page back and free it (unload).
guest_window_free proc
 mov ecx,guest_window
 test ecx,ecx
 jz done
 shr ecx,12
 mov eax,guest_window_pte
 mov [PAGE_MAP+ecx*4],eax
 mov eax,guest_window
 db 0Fh,01h,38h                         ; INVLPG [EAX]
 push 0
 push guest_window
 @VMMCall _PageFree
 add esp,8
 mov guest_window,0
done:
 ret
guest_window_free endp


; Virtual IRQ line ECX for VM EAX: raised in Jemm's live virtual PIC when
; that VM runs, else left pending in its saved state for its next slice.
; CF=1 when it cannot be delivered. Preserves EBX, ESI, EDI, EBP.
vmm_pend proc
 cmp eax,vmm_live
 jne pend_saved
 mov eax,3
 @VMMCall Host_Scheduler_Profile
 ret
pend_saved:
 cmp eax,VMM_MAX
 jae pend_failed
 push esi
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne pend_dropped
 bts dword ptr [esi].VMREC.cv+4,ecx
 pop esi
 clc
 ret
pend_dropped:
 pop esi
pend_failed:
 stc
 ret
vmm_pend endp

; Jemm's IRQ1/IRQ12 filter (EAX = line), installed while the VM manager or a
; device model runs. The keyboard belongs to the focus VM; the mouse too when
; that VM's session has one, else to the system VM. The running VM's model
; takes its byte at once; the model of a VM that does not run takes it when
; that VM runs next (jlm_poll); a VM without a model reads the controller
; itself, so its line is only pended. CF=1 when the IRQ was taken here.
jlm_filter proc
 call vmm_line_target                   ; EDX = VM for line EAX
 call vmm_mark_input
 cmp edx,vmm_current
 jne other_vm
 cmp dev_active,0
 je pass
 jmp dev_filter
other_vm:
 push esi
 push eax
 mov eax,edx
 call vmm_record
 pop eax
 mov ecx,SESS_KBD
 cmp eax,12
 jne line_flag
 mov ecx,SESS_MOUSE
line_flag:
 test [esi].VMREC.sess,ecx
 jz pend_line
 ; The target model lives in system pages and can take the controller byte
 ; now. Leaving it in the 8042 until the target's next slice loses rapid
 ; Ctrl+Esc and mouse packets while another VM is executing.
 cmp dev_busy,0
 jne defer_model
 pushad
 mov eax,[esi].VMREC.dev_inst
 mov dev_args,eax
 mov [dev_args+4],edx
 mov eax,offset cvdev_select
 mov ecx,2
 call dev_call
 mov eax,[esp+28]                    ; IRQ line saved by PUSHAD
 mov dev_args,eax
 mov eax,offset cvdev_irq_filter
 mov ecx,1
 call dev_call
 call dev_hotkey_check
 mov eax,vmm_current
 call vmm_select
 popad
 pop esi
 stc
 ret
defer_model:
 bts [esi].VMREC.pull_lines,eax
 pop esi
 stc
 ret
pend_line:
 pop esi
 mov ecx,eax
 mov eax,edx
 call vmm_pend
 stc
 ret
pass:
 clc
 ret
jlm_filter endp

; Called from HDPMI's physical IRQ1/IRQ12 path while a protected-mode DOS
; client is active.  Jemm's V86 IRQ filter is bypassed in that path.  Route
; the byte through the same owner decision and report whether HDPMI should
; acknowledge the hardware IRQ itself.  EBP is the V86 bridge client frame.
vmm_physical_irq proc
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 cmp eax,1
 je line_ok
 cmp eax,12
 jne invalid
line_ok:
 cmp vmm_installed,0
 je invalid
 call jlm_filter
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,0
 jnc routed
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,1
 ; The host acknowledges the physical IRQ only after this call returns: no
 ; VM switch at its return to V86. BX=2 asks it for VMM_INPUT_YIELD after
 ; its EOI when the byte is for a VM that does not run.
 mov vmm_poll_input_skip,1
 mov eax,vmm_input_urgent
 cmp eax,VMM_MAX
 jae routed
 cmp eax,vmm_current
 je routed
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,2
routed:
 xor eax,eax
 ret
invalid:
 mov eax,VM_ERROR_OPERATION
 ret
vmm_physical_irq endp

; A controller byte the running session's model left there (another VM owns
; its line): that VM gets the line, as jlm_filter would have given it had
; the IRQ not reached a DPMI host's IDT. cdecl for the device model.
cvdev_route_line proc c public irq:dword
 pushad
 mov eax,irq
 call vmm_line_target
 cmp edx,vmm_current
 je done
 push eax
 mov eax,edx
 call vmm_record
 pop eax
 mov ecx,SESS_KBD
 cmp eax,12
 jne line_flag
 mov ecx,SESS_MOUSE
line_flag:
 test [esi].VMREC.sess,ecx
 jz pend_line
 bts [esi].VMREC.pull_lines,eax
 jmp done
pend_line:
 mov ecx,eax
 mov eax,edx
 call vmm_pend
done:
 popad
 ret
cvdev_route_line endp

; EAX = 1 or 12: EDX = the VM that owns that line now. Preserves the rest.
vmm_line_target proc
 mov edx,vmm_focus
 cmp eax,12
 jne done
 cmp vmm_mouse_home,0                   ; the desktop feeds it (DEV_MOUSE)
 jne home
 push esi
 push eax
 mov eax,edx
 call vmm_record
 pop eax
 test [esi].VMREC.sess,SESS_MOUSE
 pop esi
 jnz done
home:
 xor edx,edx
done:
 ret
vmm_line_target endp

; The running VM's model takes the controller bytes left for it while it did
; not run (from jlm_poll). Preserves every register.
vmm_take_lines proc
 pushad
 cmp dev_active,0
 je done
 mov eax,vmm_current
 call vmm_record
 xor eax,eax
 xchg eax,[esi].VMREC.pull_lines
 test eax,eax
 jz done
 mov ebx,eax
 bt ebx,1
 jnc no_keyboard
 mov eax,1
 call dev_filter
no_keyboard:
 bt ebx,12
 jnc done
 mov eax,12
 call dev_filter
done:
 popad
 ret
vmm_take_lines endp

; The device model took Ctrl+Esc: the keyboard goes back to the system VM.
dev_hotkey_check proc
 cmp cvdev_release_request,0
 je done
 mov cvdev_release_request,0
 cmp vmm_focus,0
 je done
 mov vmm_focus,0
 call vmm_input_route
done:
 ret
dev_hotkey_check endp

; Every VM's device model learns what it owns now: keyboard and mouse bytes
; it may take from the controller, its focus (held keys are released when it
; loses it) and Ctrl+Esc outside the system VM. The system VM's own session
; keeps both, its focus managed by the DOS window runtime (DEV_FOCUS). Then
; the AC'97 stream plays the focus VM's session, or else the first one with
; audio. Preserves every register.
vmm_input_route proc
 pushad
 xor ebx,ebx
next_vm:
 mov eax,ebx
 call vmm_record
 test [esi].VMREC.sess,SESS_DEVICES
 jz skip
 mov eax,[esi].VMREC.dev_inst
 mov dev_args,eax
 mov [dev_args+4],ebx
 mov eax,offset cvdev_select
 mov ecx,2
 call dev_call
 xor eax,eax                            ; Ctrl+Esc outside the system VM
 test ebx,ebx
 setnz al
 mov dev_args,eax
 mov eax,offset cvdev_set_hotkey
 mov ecx,1
 call dev_call
 xor eax,eax                            ; keyboard: the focus VM
 cmp ebx,vmm_focus
 sete al
 mov dev_args,eax
 mov eax,12                             ; mouse: the owner of IRQ12
 call vmm_line_target
 xor eax,eax
 cmp ebx,edx
 sete al
 mov [dev_args+4],eax
 mov eax,offset cvdev_set_pull
 mov ecx,2
 call dev_call
 test ebx,ebx                           ; the runtime owns VM 0's focus
 jz skip
 xor eax,eax
 cmp ebx,vmm_focus
 sete al
 mov dev_args,eax
 mov eax,offset cvdev_focus
 mov ecx,1
 call dev_call
skip:
 inc ebx
 cmp ebx,VMM_MAX
 jb next_vm
 mov eax,vmm_current
 call vmm_select
 call vmm_pick_audio
 popad
 ret
vmm_input_route endp

; The AC'97 stream's owner: the focus VM's session if it plays audio, else
; the first session that does, else none (silence). Preserves registers.
vmm_pick_audio proc
 pushad
 mov eax,vmm_focus
 call vmm_record
 test [esi].VMREC.sess,SESS_AUDIO
 jnz owner
 xor eax,eax
next_vm:
 call vmm_record
 test [esi].VMREC.sess,SESS_AUDIO
 jnz owner
 inc eax
 cmp eax,VMM_MAX
 jb next_vm
 xor ecx,ecx
 jmp set_owner
owner:
 mov ecx,[esi].VMREC.dev_inst
set_owner:
 mov dev_args,ecx
 mov eax,offset cvdev_audio_owner
 mov ecx,1
 call dev_call
 popad
 ret
vmm_pick_audio endp

; VMM_FOCUS: BX = VM for the keyboard (and the mouse when its session has
; one, unless CX bit 0 keeps the mouse with the system VM).
vmm_set_focus proc
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 cmp eax,VMM_MAX
 jae bad
 push esi
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 pop esi
 jne bad
 mov vmm_focus,eax
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_ECX
 and ecx,1
 mov vmm_mouse_home,ecx
 call vmm_input_route
 xor eax,eax
 ret
bad:
 mov eax,VM_ERROR_OPERATION
 ret
vmm_set_focus endp

; One IRQ0 host-scheduler callback for the session scheduler and the VM
; manager (Jemm has one slot). Preserves every register and flag.
TICK_SCHEDULER equ 1
TICK_VMM equ 2

host_tick proc
 test tick_owners,TICK_SCHEDULER
 jz no_scheduler
 cmp scheduler_installed,0              ; the running VM's session's
 je no_scheduler
 call vm_scheduler_irq
no_scheduler:
 test tick_owners,TICK_VMM
 jz no_vmm
 call vmm_irq
no_vmm:
 ret
host_tick endp

; EAX = TICK_* owner. CF=1 when Jemm's slot belongs to someone else.
tick_acquire proc
 test tick_owners,eax
 jnz acquired
 cmp tick_owners,0
 jne add_owner
 push esi
 mov esi,offset host_tick
 push eax
 @VMMCall Install_Host_Scheduler
 pop eax
 pop esi
 jc refused
add_owner:
 or tick_owners,eax
acquired:
 clc
refused:
 ret
tick_acquire endp

; EAX = TICK_* owner. CF=1 when Jemm refused the removal.
tick_release proc
 test tick_owners,eax
 jz released
 not eax
 and eax,tick_owners
 jnz keep
 push esi
 push eax
 mov esi,offset host_tick
 @VMMCall Remove_Host_Scheduler
 pop eax
 pop esi
 jc refused
keep:
 mov tick_owners,eax
released:
 clc
refused:
 ret
tick_release endp

vmm_state proc
 mov eax,vmm_count
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,ax
 mov eax,vmm_focus
 shl eax,16
 mov ax,word ptr vmm_current
 mov [ebp].Client_Reg_Struc.Client_ECX,eax
 mov eax,vmm_switches
 mov [ebp].Client_Reg_Struc.Client_EDX,eax
 mov eax,vmm_gated
 mov [ebp].Client_Reg_Struc.Client_ESI,eax
 mov eax,vmm_last_exit
 mov word ptr [ebp].Client_Reg_Struc.Client_EDI,ax
 xor eax,eax
 ret
vmm_state endp

; Host-scheduler callback (IRQ0 from V86, EBP = client frame, IF clear).
vmm_irq proc
 pushfd
 cli
 pushad
 inc vmm_ticks
 inc clock_host_ticks
 xor ebx,ebx                            ; 1: V86 frame, the IRQ0 is reflected
 cmp ebp,vmm_client_frame               ; into the VM that runs after this
 sete bl
 cmp vmm_count,2
 jb irq_seen
 cmp clock_enabled,0
 jne @F
 call vmm_owe_tick
@@:
 ; Jemm also calls this for an IRQ0 that arrived in ring 0 (EBP is not the
 ; V86 client frame then). Switch from the canonical frame at the top of the
 ; monitor stack (seen by every API call), or from ring 0 only while Jemm
 ; waits in its HLT emulation: that frame is then the #GP of the guest's HLT
 ; with EIP already past the F4h opcode, and Jemm returns to V86 through it.
 mov vmm_idle,0
 test ebx,ebx
 jnz irq_frame
 mov ebp,vmm_client_frame
 test ebp,ebp
 jz irq_done
 cmp byte ptr [ebp].Client_Reg_Struc.Client_Int,0Dh
 jne irq_done
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_CS
 shl eax,4
 add eax,[ebp].Client_Reg_Struc.Client_EIP
 cmp byte ptr [eax-1],0F4h
 jne irq_done
 mov vmm_idle,1
 jmp irq_switch                         ; an idle VM gives its slice away now
irq_frame:
 mov eax,vmm_audio_urgent
 cmp eax,-1
 je irq_regular_slice
 cmp eax,vmm_current
 jne irq_switch
 mov vmm_audio_urgent,-1
irq_regular_slice:
 call vmm_input_due
 jc irq_switch
 jnz irq_seen
 mov eax,vmm_ticks
 and eax,VMM_SLICE-1
 jnz irq_seen
irq_switch:
 ; The monitor acknowledges this IRQ0 after its callback returns (or
 ; before the callback for a ring-0 IRQ).  It does NOT acknowledge a
 ; different physical ISR interrupted by this tick, e.g. HDPMI's IRQ12
 ; bridge.  Giving that ISR's EOI to another VM strands its virtual PIC.
 push ebx
 cmp vmm_poll_no_switch,0
 jne irq_defer
 call vmm_phys_idle
 test ebx,ebx
 jz irq_phys_checked
 and ax,0FFFEh                         ; only this monitor IRQ0 is safe
irq_phys_checked:
 test ax,ax
 jnz irq_defer
 call vmm_switch
 pop ebx
 jmp irq_seen
irq_defer:
 mov eax,vmm_current
 mov vmm_defer_vm,eax
 inc vmm_defer_irqs
 pop ebx
irq_seen:
 cmp clock_enabled,0
 jne irq_done
 ; Its V86 IRQ0 handler (VMM_YIELD) must not count this tick again.
 test ebx,ebx
 jz irq_done
 mov eax,vmm_current
 bts vmm_seen,eax
irq_done:
 popad
 popfd
 ret
vmm_irq endp

; VMM_YIELD (EBP = the API frame, its result already stored): an IRQ0 that
; the VM's DPMI host reflected to it, the monitor's IRQ0 path having never
; seen it (the host owned the IDT). Counted, and the slice ends, as there.
vmm_dpmi_tick proc
 inc vmm_yield_calls
 cmp vmm_installed,0
 je done
 mov eax,vmm_current
 btr vmm_seen,eax                       ; reflected by the monitor: counted
 jc seen
 inc vmm_ticks
 cmp vmm_count,2
 jb done
 cmp clock_enabled,0
 jne @F
 call vmm_owe_tick
@@:
 mov vmm_idle,0
 mov eax,vmm_audio_urgent
 cmp eax,-1
 je dpmi_regular_slice
 cmp eax,vmm_current
 jne dpmi_switch
 mov vmm_audio_urgent,-1
dpmi_regular_slice:
 call vmm_input_due
 jc dpmi_switch
 jnz done
 mov eax,vmm_ticks
 and eax,VMM_SLICE-1
 jnz done
dpmi_switch:
 ; DPMIRUN normally chains to the BIOS EOI first.  A nested physical ISR
 ; can still be outstanding; unlike the monitor callback none is ours to
 ; acknowledge here.  Retain the slice request until a safe V86 return.
 cmp vmm_poll_no_switch,0
 jne dpmi_defer
 call vmm_phys_idle
 jnz dpmi_defer
 inc vmm_yields
 call vmm_switch
 jmp done
dpmi_defer:
 mov eax,vmm_current
 mov vmm_defer_vm,eax
 inc vmm_defer_irqs
done:
 ret
seen:
 inc vmm_yield_seen
 ret
vmm_dpmi_tick endp

; CMOS/RTC ports 70h/71h under the VM manager: the index register is shared
; hardware, so each VM keeps its own. An access to 71h selects that VM's index
; first; both steps run here, with no VM switch between them.
cmos_port proc
 push esi
 push eax
 mov eax,vmm_current
 call vmm_record
 pop eax
 cmp dx,70h
 jne data_port
 test ecx,OUTPUT
 jz physical
 test ecx,STRING_IO or WORD_IO or DWORD_IO
 jnz physical
 mov byte ptr [esi].VMREC.cmos_index,al
 jmp physical
data_port:
 push eax
 mov al,byte ptr [esi].VMREC.cmos_index
 out 70h,al
 pop eax
physical:
 pop esi
 @VMMCall Simulate_IO
 ret
cmos_port endp

; Trap 70h/71h while several VMs may run. CF=1 when Jemm refused.
vmm_cmos_trap proc
 cmp vmm_cmos_trapped,0
 jne done
 push esi
 push edx
 mov esi,offset cmos_port
 mov edx,70h
 @VMMCall Install_IO_Handler
 jc failed
 mov edx,71h
 @VMMCall Install_IO_Handler
 jc failed_71
 mov vmm_cmos_trapped,1
 pop edx
 pop esi
done:
 clc
 ret
failed_71:
 mov edx,70h
 @VMMCall Remove_IO_Handler
failed:
 pop edx
 pop esi
 stc
 ret
vmm_cmos_trap endp

vmm_cmos_untrap proc
 cmp vmm_cmos_trapped,0
 je done
 push edx
 mov edx,70h
 @VMMCall Remove_IO_Handler
 mov edx,71h
 @VMMCall Remove_IO_Handler
 pop edx
 mov vmm_cmos_trapped,0
done:
 ret
vmm_cmos_untrap endp

; One physical IRQ0 reached the running VM only: every other VM is owed it.
vmm_owe_tick proc
 xor eax,eax
next_vm:
 cmp eax,vmm_live
 je skip
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne skip
 cmp [esi].VMREC.missed,VMM_MISSED_MAX
 jae skip
 inc [esi].VMREC.missed
skip:
 inc eax
 cmp eax,VMM_MAX
 jb next_vm
 ret
vmm_owe_tick endp

; EDX = VM that receives a keyboard/mouse byte. Another VM runs it next at
; the first safe boundary (return to V86 or IRQ0), as urgent audio; the
; system VM (desktop) then keeps the CPU until it idles, at most VMM_BOOST
; ticks, so a response is not cut by the slice end. Preserves every register.
vmm_mark_input proc
 pushad
 cmp vmm_count,2
 jb done
 cmp edx,VMM_MAX
 jae done
 test edx,edx
 jnz not_desktop
 cmp edx,vmm_current
 jne not_desktop
 mov vmm_boost_vm,0                     ; the running desktop: boost only
 mov eax,vmm_ticks
 add eax,VMM_BOOST
 mov vmm_boost_end,eax
 jmp done
not_desktop:
 cmp edx,vmm_current
 je done
 mov eax,edx
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne done
 mov vmm_input_urgent,edx
 mov vmm_resuming,0                     ; real input: the desktop is boosted
done:
 popad
 ret
vmm_mark_input endp

; At module load (one VM, ring 0, IRQs off for 20 ms): the TSC rate against
; PIT channel 2 in mode 0, as Linux's pit_calibrate_tsc does. Speaker data
; stays off; port 61h is restored. vmm_tsc_measured = kHz or 0.
VMM_PIT_LATCH equ 23864                ; 1193182 Hz * 20 ms
vmm_measure_tsc proc
 pushfd
 cli
 pushad
 in al,61h
 mov bl,al
 and al,not 2                           ; speaker data off
 or al,1                                ; channel 2 gate on
 out 61h,al
 mov al,0B0h                            ; channel 2, lobyte/hibyte, mode 0
 out 43h,al
 mov al,VMM_PIT_LATCH and 0FFh
 out 42h,al
 mov al,VMM_PIT_LATCH shr 8
 out 42h,al
 db 0Fh,31h                             ; rdtsc
 mov esi,eax
 mov edi,edx
 mov ecx,4000000h                       ; bounded: never hang on absent hardware
wait_out2:
 in al,61h
 test al,20h
 jnz counted
 dec ecx
 jnz wait_out2
 jmp restore
counted:
 db 0Fh,31h
 sub eax,esi
 sbb edx,edi
 mov ecx,20                             ; cycles per ms over 20 ms
 cmp edx,ecx
 jae restore
 div ecx
 cmp eax,1000                           ; 1 MHz .. 20 GHz
 jb restore
 cmp eax,20000000
 ja restore
 mov vmm_tsc_measured,eax
 cmp vmm_tsc_khz,0
 jne restore
 mov vmm_tsc_khz,eax
restore:
 mov al,bl
 out 61h,al
 popad
 popfd
 ret
vmm_measure_tsc endp

; ZF=1 when no physical IRQ is in service on either 8259. A VM switch while
; one is would hand its EOI to a VM that never took it: Jemm then completes a
; virtual EOI on the physical PIC and the next VM's virtual IRQ (the mouse)
; stays in service for good. Changes EAX; read mode restored to IRR.
vmm_phys_idle proc
 mov al,0Bh
 out 0A0h,al
 in al,0A0h
 mov ah,al                              ; slave ISR
 mov al,0Ah
 out 0A0h,al
 mov al,0Bh
 out 20h,al
 in al,20h                              ; master ISR
 push eax
 mov al,0Ah
 out 20h,al
 pop eax
 ; A real IRQ driver may have issued early EOI while its line is still
 ; self-masked. Keep the owning VM until it finishes and unmasks the line.
 push ebx
 push ecx
 push edx
 mov ebx,eax
 mov eax,13
 @VMMCall Host_Scheduler_Profile
 jc no_physical_mask_owner
 or ebx,edx
no_physical_mask_owner:
 mov eax,ebx
 pop edx
 pop ecx
 pop ebx
 test ax,ax
 ret
vmm_phys_idle endp

; The VM that urgent audio interrupted runs again once the audio VM has run
; for 3 ms (time to service its interrupt). Changes EAX, ECX, EDX.
vmm_resume_due proc
 mov eax,vmm_resume_vm
 cmp eax,VMM_MAX
 jae done
 cmp eax,vmm_current
 je clear
 mov ecx,vmm_tsc_khz
 jecxz done                             ; unknown rate: the next tick
 db 0Fh,31h                             ; rdtsc
 sub eax,vmm_resume_tsc
 lea ecx,[ecx+ecx*2]
 cmp eax,ecx
 jb done
 mov eax,vmm_resume_vm
 mov vmm_input_urgent,eax
 mov vmm_resuming,1                     ; a resume, not input: no boost
clear:
 mov vmm_resume_vm,-1
done:
 ret
vmm_resume_due endp

; The desktop (VM 0) gave the CPU away at HLT while another VM runs: let it
; run again (no boost) to present that VM's new frames and answer timers.
; A DOS window would otherwise update only at the 2-tick slice boundary.
; Changes EAX.
vmm_desk_wake proc
 test vmm_idle_mask,1
 jz done
 cmp vmm_current,0
 je done
 cmp vmm_input_urgent,VMM_MAX
 jb done
 mov vmm_input_urgent,0
 mov vmm_resuming,1
done:
 ret
vmm_desk_wake endp

; VMM_INPUT_YIELD (EBP = the API frame, its result already stored).
vmm_input_switch proc
 cmp vmm_installed,0
 je done
 cmp vmm_count,2
 jb done
 mov eax,vmm_input_urgent
 cmp eax,VMM_MAX
 jae done
 cmp eax,vmm_current
 je done
 call vmm_phys_idle
 jnz done
 call vmm_switch
done:
 ret
vmm_input_switch endp

; At an IRQ0 time-slice boundary: CF=1 to switch now (input for another VM),
; else ZF=0 when the system VM is boosted after input (no slice end yet),
; ZF=1 for the regular slice. Changes EAX.
vmm_input_due proc
 call vmm_desk_wake
 mov eax,vmm_resume_vm
 cmp eax,VMM_MAX
 jae no_resume
 mov vmm_resume_vm,-1
 cmp eax,vmm_current
 je no_resume
 mov vmm_input_urgent,eax
 mov vmm_resuming,1
no_resume:
 mov eax,vmm_input_urgent
 cmp eax,VMM_MAX
 jae no_input
 cmp eax,vmm_current
 jne switch_now
 mov vmm_input_urgent,-1
no_input:
 mov eax,vmm_boost_vm
 cmp eax,vmm_current
 jne regular
 mov eax,vmm_ticks
 cmp eax,vmm_boost_end
 jb boosted
 mov vmm_boost_vm,-1
regular:
 xor eax,eax                            ; ZF=1, CF=0
 ret
boosted:
 or eax,1                               ; ZF=0, CF=0
 ret
switch_now:
 stc
 ret
vmm_input_due endp

; Jemm profile function 11: the running VM executed HLT (EBP = its client
; frame, EIP past the HLT). Another ready VM runs now instead of the CPU
; halting until the next interrupt. CF=1 when the frame belongs to that VM.
; The DOS/FAT gates of vmm_switch apply (an idle keyboard wait may switch).
; Preserves every register.
vmm_halt proc
 pushad
 inc vmm_halt_calls
 cmp vmm_installed,0
 je keep
 cmp vmm_count,2
 jb keep
 cmp vmm_borrowed,-1
 jne keep
 cmp ebp,vmm_client_frame
 jne keep
 mov eax,5
 @VMMCall Host_Scheduler_Profile        ; ECX pending: this VM wakes at once
 test ecx,ecx
 jnz keep
 call vmm_phys_idle
 jnz keep
 mov eax,vmm_current
 cmp eax,vmm_boost_vm                   ; the desktop has answered its input
 jne not_boosted
 mov vmm_boost_vm,-1
not_boosted:
 push eax                               ; vmm_switch changes every register
 mov vmm_idle,1
 call vmm_switch
 mov vmm_idle,0
 pop eax
 cmp eax,vmm_current
 je keep
 bts vmm_idle_mask,eax
 test eax,eax
 jnz halt_not_desktop
 push edx
 db 0Fh,31h                             ; rdtsc
 mov vmm_desk_tsc,eax
 pop edx
halt_not_desktop:
 inc vmm_halt_switches
 popad
 stc
 ret
keep:
 popad
 clc
 ret
vmm_halt endp

; Return-to-V86 callback (Jemm profile function 6), shared by the device
; model and the VM manager: the running VM gets the IRQ0 ticks it is owed,
; one at a time, whenever its virtual PIC has no IRQ0 pending or in service.
; Preserves every register.
jlm_poll proc
 cmp dev_active,0
 je no_devices
 call vmm_take_lines
 call dev_poll
no_devices:
 cmp dev_stream,0                       ; the AC'97 stream, whichever VM runs
 je no_stream
 call dev_audio_poll
no_stream:
 cmp vmm_installed,0
 je done
 ; The audio model can raise a protected-mode IRQ for a different VM during
 ; this return-to-V86 poll. Run that VM now, while this canonical Jemm frame
 ; can be switched, instead of waiting for another physical IRQ0 time slice.
 ; vmm_switch still enforces the DOS and FAT gates and checks READY state.
 pushfd
 cli
 pushad
 cmp vmm_borrowed,-1
 jne poll_switch_done
 cmp ebp,vmm_client_frame
 jne poll_switch_done
 xor eax,eax
 xchg eax,vmm_poll_no_switch
 test eax,eax
 jnz poll_switch_done
 mov eax,vmm_audio_urgent
 cmp eax,VMM_MAX
 jb poll_urgent
 ; Pending input: Jemm's V86 IRQ path acknowledged the IRQ before this
 ; return; a DPMI host's DEV_PHYSICAL_IRQ call has not (VMM_INPUT_YIELD).
 xor eax,eax
 xchg eax,vmm_poll_input_skip
 test eax,eax
 jnz poll_switch_done
 call vmm_resume_due
 cmp vmm_input_urgent,VMM_MAX
 jb poll_input_ready
 mov eax,vmm_defer_vm
 cmp eax,vmm_current
 je poll_deferred_slice
 db 0Fh,31h                             ; rdtsc
 sub eax,vmm_desk_tsc
 mov ecx,vmm_tsc_khz
 jecxz poll_switch_done
 imul ecx,ecx,VMM_DESK_MS
 cmp eax,ecx
 jb poll_switch_done
 call vmm_desk_wake
poll_input_ready:
 cmp vmm_input_urgent,VMM_MAX
 jae poll_switch_done
 mov eax,vmm_input_urgent
poll_urgent:
 cmp eax,vmm_current
 je poll_switch_done
 ; Not inside an IRQ a DPMI host reflected to this VM before its EOI (the
 ; BIOS IRQ0 handler): see vmm_phys_idle. Audio and input both wait.
 call vmm_phys_idle
 jnz poll_switch_done
 mov eax,vmm_audio_urgent
 cmp eax,VMM_MAX
 jb poll_switch_now
 mov eax,vmm_input_urgent
poll_switch_now:
 call vmm_switch
 jmp poll_switch_done
poll_deferred_slice:
 ; Finish a regular time slice postponed by a physical ISR.  Urgent
 ; audio/input above retain their priority, and vmm_switch retains all
 ; DOS/FAT gates.  The pending request is cleared by an actual switch.
 mov eax,vmm_boost_vm
 cmp eax,vmm_current
 jne poll_deferred_idle
 mov eax,vmm_ticks
 cmp eax,vmm_boost_end
 jb poll_switch_done
poll_deferred_idle:
 call vmm_phys_idle
 jnz poll_switch_done
 inc vmm_yields
 call vmm_switch
poll_switch_done:
 popad
 popfd
 call clock_poll
 pushad
 cmp clock_enabled,0
 jne owed_none
 mov eax,vmm_live
 call vmm_record
 cmp [esi].VMREC.missed,0
 je owed_none
 mov eax,5
 @VMMCall Host_Scheduler_Profile        ; EDX in service, ECX pending
 test ecx,(1 shl 1) or (1 shl 12)       ; input before owed timer ticks
 jnz owed_none
 test ecx,1
 jnz owed_none
 test edx,1
 jnz owed_none
 xor ecx,ecx
 mov eax,3
 @VMMCall Host_Scheduler_Profile
 jc owed_none
 dec [esi].VMREC.missed
 mov eax,vmm_live
 bts vmm_seen,eax                       ; not a DPMI-host tick (VMM_YIELD)
owed_none:
 popad
done:
 ; A retained native process receives at most one bounded quantum at this
 ; canonical return-to-V86 boundary; nproc_poll applies DOS/audio gates.
 pushfd
 cli
 pushad
 call nproc_poll
 call vmm_park
 popad
 popfd
 ret
jlm_poll endp

; Round robin to the next ready VM. EBP = client frame.
vmm_switch proc
 mov eax,vmm_current
 call vmm_record
 mov edi,esi                            ; EDI = current record
 cmp [edi].VMREC.state,VM_SLOT_DEAD
 je gate_open                           ; a finished VM never resumes
 ; Leave a VM only outside DOS with a clean FAT write-back cache.
 mov ecx,vmm_kbase
 cmp byte ptr [ecx+KL_FAT_DIRTY],0
 jne gated
 cmp byte ptr [ecx+KL_INDOS],0
 je gate_open
 cmp vmm_idle,0                         ; inside DOS: waiting for a key only
 je gated
 mov al,[ecx+KL_LAST_AH]
 cmp al,1
 je gate_open
 cmp al,0Ah
 je gate_open
 cmp al,0Ch
 je gate_open
 cmp al,6
 jb gated
 cmp al,8
 ja gated
gate_open:
 mov eax,vmm_audio_urgent
 cmp eax,VMM_MAX
 jae regular_candidate
 cmp eax,vmm_current
 je clear_urgent
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne clear_urgent
 mov eax,vmm_audio_urgent
 dec eax                                 ; next_candidate increments first
 jmp candidate_start
clear_urgent:
 mov vmm_audio_urgent,-1
regular_candidate:
 mov eax,vmm_input_urgent
 cmp eax,VMM_MAX
 jae plain_candidate
 cmp eax,vmm_current
 je clear_input
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne clear_input
 mov eax,vmm_input_urgent
 dec eax                                 ; next_candidate increments first
 jmp candidate_start
clear_input:
 mov vmm_input_urgent,-1
plain_candidate:
 mov eax,vmm_current
candidate_start:
 mov edx,VMM_MAX
next_candidate:
 inc eax
 cmp eax,VMM_MAX
 jb candidate
 xor eax,eax
candidate:
 push eax
 call vmm_record
 pop eax
 cmp [esi].VMREC.state,VM_SLOT_READY
 je switch_to
 dec edx
 jnz next_candidate
 ret                                    ; nothing else to run
gated:
 inc vmm_gated
 ret
switch_to:
 mov ebx,eax                            ; EBX = next VM number, ESI its record
 mov eax,vmm_defer_vm
 cmp eax,vmm_current
 jne no_deferred_slice
 mov vmm_defer_vm,-1
 inc vmm_defer_switches
no_deferred_slice:
 btr vmm_idle_mask,ebx
 cmp ebx,vmm_audio_urgent
 jne not_audio_urgent
 mov vmm_audio_urgent,-1
 ; Urgent audio interrupts a VM: the audio VM services its IRQ, then that
 ; VM comes back at the next safe point instead of losing the rest of its
 ; time slice (a game's SB16 IRQ every 20-40 ms otherwise left the desktop
 ; about a tenth of the processor).
 mov eax,vmm_current
 mov vmm_resume_vm,eax
 push edx
 db 0Fh,31h                             ; rdtsc
 mov vmm_resume_tsc,eax
 pop edx
not_audio_urgent:
 cmp ebx,vmm_input_urgent
 jne not_input_urgent
 mov vmm_input_urgent,-1
 xor eax,eax
 xchg eax,vmm_resuming
 test eax,eax                            ; resumed after urgent audio
 jnz not_input_urgent
 test ebx,ebx                            ; the desktop finishes its response
 jnz not_input_urgent
 mov vmm_boost_vm,0
 mov eax,vmm_ticks
 add eax,VMM_BOOST
 mov vmm_boost_end,eax
not_input_urgent:
 push esi
 ; Save the running VM.
 push edi
 mov esi,ebp
 lea edi,[edi].VMREC.frame
 mov ecx,VMM_FRAME/4
 cld
 rep movsd
 pop edi
 lea ecx,[edi].VMREC.cv
 mov eax,9
 @VMMCall Host_Scheduler_Profile
 jc cv_saved
 mov [edi].VMREC.cv_valid,1
 ; A VM entered for the first time inherits this state, nothing pending.
 mov esi,[esp]
 cmp [esi].VMREC.cv_valid,0
 jne cv_saved
 push esi                               ; next record
 push edi                               ; current record
 lea edi,[esi].VMREC.cv                 ; to the next VM's state
 mov esi,[esp]
 lea esi,[esi].VMREC.cv                 ; from the leaving VM's
 mov ecx,VMM_CV_BYTES/4
 rep movsd
 pop edi
 pop esi
 mov dword ptr [esi].VMREC.cv+4,0
 mov dword ptr [esi].VMREC.cv+8,0
 mov word ptr [esi].VMREC.cv+12,0
 mov [esi].VMREC.cv_valid,1
cv_saved:
 ; Its per-session variables out, its first megabyte out (the aperture
 ; entries change at run time), the next VM's in.
 mov esi,edi
 call ctx_save
 call bios_ptes_out
 mov esi,PAGE_MAP
 lea edi,[edi].VMREC.ptes
 mov ecx,VMM_PTES
 rep movsd
 pop esi
 push esi
 call bios_ptes_in
 lea esi,[esi].VMREC.ptes
 mov edi,PAGE_MAP
 mov ecx,VMM_PTES
 rep movsd
 mov eax,cr3
 mov cr3,eax
 pop esi
 call ctx_load
 mov eax,ebx
 call vmm_select
 lea ecx,[esi].VMREC.cv
 mov eax,10
 @VMMCall Host_Scheduler_Profile
 ; Its registers, keeping the interrupt being reflected now.
 mov eax,[ebp].Client_Reg_Struc.Client_Int
 mov edx,[ebp].Client_Reg_Struc.Client_Error
 push esi
 lea esi,[esi].VMREC.frame
 mov edi,ebp
 mov ecx,VMM_FRAME/4
 rep movsd
 pop esi
 mov [ebp].Client_Reg_Struc.Client_Int,eax
 mov [ebp].Client_Reg_Struc.Client_Error,edx
 ; It re-reads the FAT the others may have written.
 mov ecx,vmm_kbase
 mov byte ptr [ecx+KL_FAT_VALID],0
 mov word ptr [ecx+KL_FAT_SECTOR],0FFFFh
 ; A finished VM left behind is released now that it is not mapped.
 mov eax,vmm_current
 mov vmm_current,ebx
 mov vmm_live,ebx
 inc vmm_switches
 push esi
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_DEAD
 jne released
 cmp eax,vmm_focus                      ; its keyboard goes back home
 jne focus_kept
 mov vmm_focus,0
focus_kept:
 push ebx
 mov ebx,esi
 call vmm_free_block
 call vmm_free_instances
 mov [ebx].VMREC.state,VM_SLOT_FREE
 dec vmm_count
 pop ebx
released:
 pop esi
 ret
vmm_switch endp

; V86 operations 40h-43h (EBP = client frame). EAX=0 or VM_ERROR_*.
vmm_dispatch proc
 mov vmm_client_frame,ebp
 cmp eax,VM_OP_VMM_INIT
 je op_init
 cmp eax,VM_OP_VMM_CREATE
 je op_create
 cmp eax,VM_OP_VMM_EXIT
 je op_exit
 cmp eax,VM_OP_VMM_STATE
 je op_state
 cmp eax,VM_OP_VMM_FOCUS
 je op_focus
 cmp eax,VM_OP_VMM_KILL
 je op_kill
 cmp eax,VM_OP_VMM_IVT
 je op_ivt
 cmp eax,VM_OP_VMM_TARGET
 je op_target
 cmp eax,VM_OP_VMM_LIST
 je op_list
 cmp eax,VM_OP_VMM_EXIT_CODE
 je op_exit_code
 cmp eax,VM_OP_VMM_UMB_ALLOC
 je op_umb_alloc
 mov eax,VM_ERROR_OPERATION
 ret
; VMM_EXIT_CODE: BX = VM -> AX state, DX exit code, CX its generation.
op_exit_code:
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 cmp eax,VMM_MAX
 jae exit_code_bad
 call vmm_record
 mov eax,[esi].VMREC.exit_code
 mov word ptr [ebp].Client_Reg_Struc.Client_EDX,ax
 mov eax,[esi].VMREC.generation
 mov word ptr [ebp].Client_Reg_Struc.Client_ECX,ax
 mov eax,[esi].VMREC.state
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,ax
 xor eax,eax
 ret
exit_code_bad:
 mov eax,VM_ERROR_OPERATION
 ret
op_focus:
 jmp vmm_set_focus
op_kill:
 jmp vmm_kill
op_umb_alloc:
 jmp vmm_umb_alloc
op_target:
 jmp vmm_set_target
; VMM_LIST: four bytes per VM (state, SESS_* flags, has the focus, runs).
op_list:
 mov ecx,VMM_MAX*4
 call guest_destination
 test eax,eax
 jnz list_done
 xor ecx,ecx
list_next:
 mov eax,ecx
 call vmm_record
 mov al,byte ptr [esi].VMREC.state
 mov [edi],al
 mov al,byte ptr [esi].VMREC.sess
 mov [edi+1],al
 xor eax,eax
 cmp ecx,vmm_focus
 sete al
 mov [edi+2],al
 xor eax,eax
 cmp ecx,vmm_current
 sete al
 mov [edi+3],al
 add edi,4
 inc ecx
 cmp ecx,VMM_MAX
 jb list_next
 xor eax,eax
list_done:
 ret
op_ivt:
 mov ecx,1024
 call guest_destination
 test eax,eax
 jnz ivt_done
 mov esi,offset ivt_snapshot
 mov ecx,256
 cld
 rep movsd
 xor eax,eax
ivt_done:
 ret
op_init:
 jmp vmm_init
op_create:
 jmp vmm_create
op_exit:
 jmp vmm_exit
op_state:
 jmp vmm_state
vmm_dispatch endp

.data
vmm_client_frame dd 0
vmm_borrowed dd -1                      ; VM whose session a call acts for
.code
