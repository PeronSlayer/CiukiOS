; CiukiOS adapter for the shipped upstream HDPMI 3.24 host.
;
; This module is deliberately built into that host.  It registers its range
; through HDPMI's existing is0006 TRAPPROCS implementation and releases the
; exact returned handle through is0007.  The ring-3 handlers below implement
; the 3.24 DPMI exception-frame ABI; no older fork ABI is used.
;
; There is one monitored foreground DOS context.  Nothing here creates or
; schedules another V86 context, and no IRQ/callback path calls DOS or BIOS.

	.386p

	include hdpmi.inc
	include external.inc
	include session_abi.inc
	include session_scheduler_abi.inc
	include session_video_abi.inc

	option proc:private

if ?32BIT

CV_FIRST_PORT equ 03B0h
CV_PORT_COUNT equ 30h
CV_LAST_VIDEO_PORT equ 03DFh
CV_DEV_RANGES equ 5
CV_OP_DEV_IO  equ 34h            ; VM_OP_DEV_IO (session_devices_abi.inc)
CV_OP_DEV_PM_IRQ equ 36h         ; VM_OP_DEV_PM_IRQ
CV_SHARED_DEVICE_IRQS equ 320    ; cvvid_shared.device_irqs (session_video.h)
CV_FIRST_PTE  equ 0A0h
CV_VIDEO_PAGES equ 67
CV_VIDEO_BYTES equ CV_VIDEO_PAGES*4096

_cvdpmi_video_execute proto near
_cvdpmi_video_port_read proto near
_cvdpmi_video_port_write proto near

; Four saved segment registers, PUSHAD, direction dword, then the official
; DPMI 0.9 exception frame supplied by HDPMI 3.24.
CVF_EDX       equ 36
CVF_ECX       equ 40
CVF_EAX       equ 44
CVF_OUT       equ 48
CVF_ERROR     equ 60
CVF_EIP       equ 64

CVERR_DESCRIPTOR equ 1
CVERR_OWNED      equ 2
CVERR_PTES       equ 3
CVERR_REGISTER   equ 4
CVERR_REMOVE     equ 5
CVERR_CALLBACK   equ 6

; Host-global configuration is copied into every client-specific data image.
_DATA16 segment
public cvdpmi_shared_linear,cvdpmi_client_active
public cvdpmi_entry_pending
public cvdpmi_rm_int10_pending,cvdpmi_jlm_entry,cvdpmi_int10_result
public cvdpmi_irq_proc
cvdpmi_shared_linear dd 0
cvdpmi_client_active dd 0
cvdpmi_entry_pending dd 0
cvdpmi_rm_int10_pending db 0
	align 4
cvdpmi_jlm_entry dd 0
; R3PROC of a device IRQ to enter once the current I/O instruction is done
; (HDPMI's I/O emulation reads and clears it, see cvdpmi_note_io).
cvdpmi_irq_proc dd 0
cvdpmi_int10_result dw 0
	align 4
cvdpmi_video_linear dd 0
_DATA16 ends

; Everything in this segment is client-specific in HDPMI, including -a
; address contexts.  Therefore a handle can never be mistaken for another
; client's ownership token.
_DATA32C segment
public cvdpmi_cli_stepping
cv_client_start label byte
cv_descriptor          dd 0
cv_generation          dd 0
cv_handle              dd 0
cv_dev_handles         dd CV_DEV_RANGES dup (0)
cv_jlm_entry           dd 0
; Protected-mode device bridge ranges (start, count): PIC 20h/21h with ISA
; DMA 00h-0Fh, DMA pages, slave PIC with 16-bit DMA, Sound Blaster, OPL.
cv_dev_ranges          dw 00h,22h, 80h,10h, 0A0h,40h, 220h,10h, 388h,4
; Device IRQ lines CVSESSION holds for this client (last DEV_IO / DEV_PM_IRQ)
; and how many were delivered.
cv_pm_magic            db 'CVPMIRQ!'
cv_pm_irqs             dd 0
cv_pm_delivered        dd 0
cv_faulted             dd 0
cv_saved_cr4           dd 0
cvdpmi_cli_stepping    dd 0
cv_if_enabled         dd 1
cv_if_trace           dd 0
cv_if_guest_tf        dd 0
cv_if_steps           dd 0
cv_if_shadow          dd 0
cv_if_pending         dd 0
cv_if_control_budget  dd 0
cv_if_operand         dd 0
cv_if_stack_offset    dd 0
cv_if_stack32         dd 0
cv_if_resume          dd 0
cv_if_resume_cs       dd 0
cv_if_resume_eip      dd 0
cv_if_ds              dd 0
cv_if_address32       dd 0
cv_if_segment         dd 0
cv_if_explicit_seg    dd 0
cv_if_gpr             dd 8 dup (0) ; PUSHAD order: DI SI BP SP BX DX CX AX
cv_if_sreg            dd 6 dup (0) ; ES CS SS DS FS GS
cv_if_modrm           dd 0
cv_if_default32       dd 0
cv_if_value           dd 0
cv_if_eax             dd 0
cv_saved_pic_master    db 0
cv_saved_pic_slave     db 0
cv_host_pic_master     db 0
cv_host_pic_slave      db 0
cv_tick_busy            db 0
cv_callback_busy        db 0
	cv_video_busy           db 0
	align 4
cv_video_shared        dd 0
cv_saved_stack          dd 0
                        dw 0
cv_fault_frame         dd 0
cv_saved_host_flags    dd 0
cv_gdt_base            dd 0
cv_ldt_base            dd 0
cv_stack_pointer        dd offset cv_callback_stack_end
                        dw _FLATSEL_
	align 4
cv_pending_services    dd 0
cv_pending_reentries   dd 0
cv_last_flags          dd 0
cv_last_eip            dd 0
cv_last_port           dd 0
cv_status1             dd 0
cv_status_phase        db 0
	align 4
cv_saved_ptes          dd CVSCHED_PTE_COUNT dup (0)
cv_ports               db CV_PORT_COUNT dup (0)
	align 4
cv_trap_procs          dd offset cv_input
	                     dw _CSR3SEL_
	                     dd offset cv_output
	                     dw _CSR3SEL_
	align 4
cv_callback_stack      db 4096 dup (0)
cv_callback_stack_end  label byte
cv_client_end label byte
_DATA32C ends

CV_ADAPTER_BYTES equ cv_client_end-cv_client_start

; Parse the cSSSS:OOOO option while INIT owns the PSP command tail.
; ES:SI points immediately after 'c', CL is the remaining byte count.  The
; descriptor itself must already be bound and armed by CVSESSION BEGIN.
_TEXT16 segment
	assume ds:GROUP16

; Real-mode leg of the protected-mode device bridge (callrmprocintern):
; EAX/EBX/ECX/EDX in, EBX/EDX/ESI out.  No V86 interrupt is delivered inside
; it: the caller enters with IF clear and it stays clear to the end, so Jemm
; cannot inject a pending IRQ onto this internal real-mode stack, nested
; inside the client's I/O instruction (and inside whatever DOS/4GW handler
; that instruction belongs to).
cvdpmi_devio_rm proc near
	cli
	call dword ptr cs:[cvdpmi_jlm_entry]
	ret
cvdpmi_devio_rm endp

cv_hex4 proc
	push bp
	push dx
	mov bp,4
	xor eax,eax
next_digit:
	test cl,cl
	jz bad
	mov dl,es:[si]
	inc si
	dec cl
	sub dl,'0'
	cmp dl,9
	jbe digit
	add dl,'0'
	or dl,20h
	sub dl,'a'
	cmp dl,5
	ja bad
	add dl,10
digit:
	movzx dx,dl
	shl ax,4
	add ax,dx
	dec bp
	jnz next_digit
	clc
	jmp done
bad:
	stc
done:
	pop dx
	pop bp
	ret
cv_hex4 endp

cvdpmi_parse_option proc near public
	push bx
	push dx
	push di
	push es
	call cv_hex4
	jc parse_bad
	mov bx,ax
	test cl,cl
	jz parse_bad
	cmp byte ptr es:[si],':'
	jne parse_bad
	inc si
	dec cl
	call cv_hex4
	jc parse_bad
	mov dx,ax
	movzx eax,bx
	shl eax,4
	movzx edx,dx
	add eax,edx
	jc parse_bad
	cmp eax,10000h
	jb parse_bad
	mov edx,eax
	add edx,CVSCHED_BYTES+VM_VSHARE_BYTES
	jc parse_bad
	cmp edx,0A0000h
	ja parse_bad
	mov cvdpmi_shared_linear,eax

	; Canonicalize the conventional linear pointer so the real-mode parser
	; can validate and claim host ownership before installation proceeds.
	mov edx,eax
	mov di,dx
	and di,000Fh
	shr edx,4
	mov es,dx
	cmp dword ptr es:[di+CVSCHED_MAGIC_OFS],CVSCHED_MAGIC
	jne parse_bad_clear
	cmp word ptr es:[di+CVSCHED_VERSION_OFS],CVSCHED_VERSION
	jne parse_bad_clear
	cmp word ptr es:[di+CVSCHED_BYTES_OFS],CVSCHED_BYTES
	jne parse_bad_clear
	mov eax,dword ptr es:[di+CVSCHED_STATE]
	mov edx,eax
	and edx,CVSCHED_STATE_BOUND or CVSCHED_STATE_SESSION or CVSCHED_STATE_JEMM
	cmp edx,CVSCHED_STATE_BOUND or CVSCHED_STATE_SESSION or CVSCHED_STATE_JEMM
	jne parse_bad_clear
	test eax,CVSCHED_STATE_DPMI_HOST or CVSCHED_STATE_DPMI_CLIENT or CVSCHED_STATE_CALLBACK
	jnz parse_bad_clear
	push ax
	push bx
	push di
	push es
	mov ax,1684h
	mov bx,VM_DEVICE_ID
	int 2Fh
	mov ax,es
	or ax,di
	jz parse_entry_bad
	mov word ptr cvdpmi_jlm_entry,di
	mov word ptr cvdpmi_jlm_entry+2,es
	pop es
	pop di
	pop bx
	pop ax
	or dword ptr es:[di+CVSCHED_STATE],CVSCHED_STATE_DPMI_HOST
	clc
	jmp parse_done
parse_entry_bad:
	pop es
	pop di
	pop bx
	pop ax
parse_bad_clear:
	mov dword ptr cvdpmi_shared_linear,0
parse_bad:
	stc
parse_done:
	pop es
	pop di
	pop dx
	pop bx
	ret
cvdpmi_parse_option endp

_TEXT16 ends

; Ring-3 callbacks use HDPMI's shipped code/data selectors and client-specific
; storage. Conventional memory is intentionally not touched at ring 3; the next
; physical host tick flushes callback accounting into the shared descriptor.
; Match HDPMI 3.24's declared ring-3 code segment exactly.  A private/default
; declaration creates an extra PE section and moves the resident GROUP16
; section away from the fixed section number used by the upstream post-link
; extraction sequence.
_TEXT32R3 segment dword ?USE32 public ?CODER3
	assume ds:nothing,es:nothing
	; The host's ring-3 selector can begin 138 bytes before a linear page
	; boundary.  Keep the callback wholly in the following mapped page.
	db 0FCh dup (90h)
cv_callback_start label byte

cv_input:
	pushd 0
	pushad
	push ds
	push es
	push fs
	push gs
	mov ebp,esp
	cld
	mov ecx,ss:[ebp+CVF_ERROR]
	mov edx,ecx
	and edx,7
	add ss:[ebp+CVF_EIP],edx
	pop gs
	pop fs
	pop es
	pop ds
	popad
	add esp,4
	retf

cv_output:
	pushd 1
	pushad
	push ds
	push es
	push fs
	push gs
	mov ebp,esp
	cld
	mov ecx,ss:[ebp+CVF_ERROR]
	mov edx,ecx
	and edx,7
	add ss:[ebp+CVF_EIP],edx
	pop gs
	pop fs
	pop es
	pop ds
	popad
	add esp,4
	retf

cv_callback_end label byte
_TEXT32R3 ends

CV_CALLBACK_BYTES equ cv_callback_end-cv_callback_start

_TEXT32 segment
	assume ds:GROUP16

; ES must be flat, EDI is the shared descriptor.  This validation is repeated
; at each lifecycle boundary before conventional memory is modified.
cv_validate proc
	test edi,edi
	jz invalid
	cmp dword ptr es:[edi+CVSCHED_MAGIC_OFS],CVSCHED_MAGIC
	jne invalid
	cmp word ptr es:[edi+CVSCHED_VERSION_OFS],CVSCHED_VERSION
	jne invalid
	cmp word ptr es:[edi+CVSCHED_BYTES_OFS],CVSCHED_BYTES
	jne invalid
	mov eax,es:[edi+CVSCHED_GENERATION]
	test eax,eax
	jz invalid
	test fs:cv_generation,-1
	jz valid
	cmp eax,fs:cv_generation
	jne invalid
valid:
	clc
	ret
invalid:
	stc
	ret
cv_validate endp

; Map the VIDEO_SHARE packet immediately following the scheduler descriptor
; into HDPMI's ring-0 system area.  The pages stay supervisor-only: ring-3 VGA
; memory accesses must continue to fault into cvdpmi_video_fault.
cv_map_video proc
	lea esi,[edi+CVSCHED_BYTES]
	cmp dword ptr es:[esi],VM_VSHARE_MAGIC
	jne map_video_bad
	cmp dword ptr es:[esi+4],CV_VIDEO_PAGES
	jne map_video_bad
	mov eax,ss:cvdpmi_video_linear
	test eax,eax
	jnz map_video_address_ready
	mov ecx,CV_VIDEO_PAGES
	call pm_AllocSysAddrSpace
	jc map_video_bad
	mov ss:cvdpmi_video_linear,eax
map_video_address_ready:
	mov fs:cv_video_shared,eax
	push edi
	call pm_Linear2PT
	mov ebx,edi
	pop edi
	add esi,8
	mov ecx,CV_VIDEO_PAGES
map_video_page:
	mov edx,es:[esi]
	test edx,0FFFh
	jnz map_video_unwind
	test edx,edx
	jz map_video_unwind
	or edx,3
	mov es:[ebx],edx
	add esi,4
	add ebx,4
	loop map_video_page
	mov eax,cr3
	mov cr3,eax
	mov ebx,fs:cv_video_shared
	cmp dword ptr es:[ebx],053565643h
	jne map_video_unwind
	cmp dword ptr es:[ebx+4],0100h
	jne map_video_unwind
	cmp dword ptr es:[ebx+8],CV_VIDEO_BYTES
	jne map_video_unwind
	mov eax,es:[edi+CVSCHED_GENERATION]
	cmp es:[ebx+12],eax
	jne map_video_unwind
	mov dword ptr es:[ebx+56],1
	clc
	ret
map_video_unwind:
	call cv_unmap_video
map_video_bad:
	stc
	ret
cv_map_video endp

cv_unmap_video proc
	mov eax,fs:cv_video_shared
	test eax,eax
	jz unmap_video_done
	mov dword ptr es:[eax+56],0
	push edi
	call pm_Linear2PT
	mov eax,edi
	pop edi
	mov ecx,CV_VIDEO_PAGES
unmap_video_page:
	mov dword ptr es:[eax],0
	add eax,4
	loop unmap_video_page
	mov eax,cr3
	mov cr3,eax
	mov dword ptr fs:cv_video_shared,0
unmap_video_done:
	ret
cv_unmap_video endp

; HDPMI owns a page table distinct from Jemm's.  Save every original HDPMI
; entry, install the exact session shadow, flush, and verify the complete
; aperture before the I/O callback becomes reachable. RESERVED1 retains an
; audit hash of the saved table across detach.
cv_install_ptes proc
	mov esi,ss:pPageTab0
	add esi,CV_FIRST_PTE*4
	lea ebx,[edi+CVSCHED_PTES]
	xor edx,edx
	xor ebp,ebp
	mov ecx,CVSCHED_PTE_COUNT
install_next:
	mov eax,es:[esi]
	mov fs:[cv_saved_ptes+edx],eax
	rol ebp,5
	xor ebp,eax
	mov eax,es:[ebx]
	mov es:[esi],eax
	add esi,4
	add ebx,4
	add edx,4
	loop install_next
	mov es:[edi+CVSCHED_RESERVED1],ebp
	mov eax,cr3
	mov es:[edi+CVSCHED_RESERVED0],eax
	mov cr3,eax

	mov esi,ss:pPageTab0
	add esi,CV_FIRST_PTE*4
	lea ebx,[edi+CVSCHED_PTES]
	mov ecx,CVSCHED_PTE_COUNT
install_verify:
	mov eax,es:[ebx]
	cmp es:[esi],eax
	jne install_bad
	add esi,4
	add ebx,4
	loop install_verify
	clc
	ret
install_bad:
	stc
	ret
cv_install_ptes endp

; Restore the exact HDPMI table captured by cv_install_ptes, flush it, and
; verify every entry.  This occurs only after the exact 3.24 callback handle
; has been removed, and before HDPMI frees client-specific state.
cv_restore_ptes proc
	mov esi,ss:pPageTab0
	add esi,CV_FIRST_PTE*4
	xor edx,edx
	mov ecx,CVSCHED_PTE_COUNT
restore_next:
	mov eax,fs:[cv_saved_ptes+edx]
	mov es:[esi],eax
	add esi,4
	add edx,4
	loop restore_next
	mov eax,cr3
	mov cr3,eax
	mov esi,ss:pPageTab0
	add esi,CV_FIRST_PTE*4
	xor edx,edx
	mov ecx,CVSCHED_PTE_COUNT
restore_verify:
	mov eax,fs:[cv_saved_ptes+edx]
	cmp es:[esi],eax
	jne restore_bad
	add esi,4
	add edx,4
	loop restore_verify
	clc
	ret
restore_bad:
	stc
	ret
cv_restore_ptes endp

; Guard all 32 HDPMI entries against the descriptor's authoritative session
; shadow.  ES=flat, EDI=descriptor.
cv_guard_ptes proc
	inc dword ptr es:[edi+CVSCHED_PTE_CHECKS]
	mov esi,ss:pPageTab0
	add esi,CV_FIRST_PTE*4
	lea ebp,[edi+CVSCHED_PTES]
	xor ebx,ebx
	xor edx,edx
	mov ecx,CVSCHED_PTE_COUNT
guard_next:
	mov eax,es:[ebp+edx]
	cmp es:[esi],eax
	je guard_ok
	mov es:[esi],eax
	inc ebx
guard_ok:
	add esi,4
	add edx,4
	loop guard_next
	test ebx,ebx
	jz guarded
	add es:[edi+CVSCHED_PTE_REPAIRS],ebx
	mov dword ptr es:[edi+CVSCHED_LAST_ERROR],CVSCHED_ERROR_PTES
	inc dword ptr es:[edi+CVSCHED_FAILURES]
	mov eax,cr3
	mov cr3,eax
guarded:
	mov eax,cr3
	; LAST_CR3 remains the Jemm owner context.  RESERVED0 records HDPMI's
	; distinct client CR3 so validation can walk both complete low tables.
	mov es:[edi+CVSCHED_RESERVED0],eax
	ret
cv_guard_ptes endp

; Register only after HDPMI has completed the first ring-3 return.  Calling
; is0006 from _initclient_pm is too early for original DOS/4G clients.  The
; host's existing first-entry INT3 trampoline invokes this helper before the
; original first instruction; the timer path remains a defensive fallback.
; ES=flat, FS=GROUP32 writable alias, EDI=descriptor.
cv_register_callback proc
	cmp dword ptr fs:cv_handle,0
	jne register_ready
	push ds
	push byte ptr _DSR3SEL_
	pop ds
	mov esi,offset cv_trap_procs
	mov dx,CV_FIRST_PORT
	mov cx,CV_PORT_COUNT
	call is0006
	pop ds
	jc register_bad
	test eax,eax
	jz register_bad
	mov fs:cv_handle,eax
	mov es:[edi+CVSCHED_DPMI_HANDLE],eax
	or dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_CALLBACK
	inc dword ptr es:[edi+CVSCHED_INSTALLS]
	mov dword ptr es:[edi+CVSCHED_CALLBACK_BYTES],CV_CALLBACK_BYTES
	call cv_register_devices
register_ready:
	clc
	ret
register_bad:
	stc
	ret
cv_register_callback endp

; Device bridge ranges.  A range another owner already traps stays theirs
; (physical for this client); none is required for the video path.
cv_register_devices proc
	pushad
	mov eax,ss:cvdpmi_jlm_entry
	mov fs:cv_jlm_entry,eax
	push ds
	push byte ptr _DSR3SEL_
	pop ds
	xor ebx,ebx
register_device_next:
	cmp dword ptr fs:[cv_dev_handles+ebx*4],0
	jne register_device_skip
	mov esi,offset cv_trap_procs
	mov dx,word ptr fs:[cv_dev_ranges+ebx*4]
	mov cx,word ptr fs:[cv_dev_ranges+ebx*4+2]
	test cx,cx
	jz register_device_skip
	push ebx
	call is0006
	pop ebx
	jc register_device_skip
	mov fs:[cv_dev_handles+ebx*4],eax
register_device_skip:
	inc ebx
	cmp ebx,CV_DEV_RANGES
	jb register_device_next
	pop ds
	popad
	ret
cv_register_devices endp

; Remove every device range this client registered.  CF=1 if one refused.
cv_remove_devices proc
	pushad
	xor ebx,ebx
	clc
	pushfd
remove_device_next:
	mov edx,fs:[cv_dev_handles+ebx*4]
	test edx,edx
	jz remove_device_skip
	push ebx
	call is0007
	pop ebx
	jc remove_device_failed
	mov dword ptr fs:[cv_dev_handles+ebx*4],0
	jmp remove_device_skip
remove_device_failed:
	or dword ptr [esp],1
remove_device_skip:
	inc ebx
	cmp ebx,CV_DEV_RANGES
	jb remove_device_next
	popfd
	popad
	ret
cv_remove_devices endp

; The host redirects the first ring-3 entry through its existing INT3/RETF
; trampoline.  This handler runs after the client is live but before its first
; original instruction, which is the safe boundary for official is0006.
cvdpmi_entry_break proc near public
	pushfd
	pushad
	push ds
	push es
	push fs
	push byte ptr _CSALIAS_
	pop fs
	push byte ptr _FLATSEL_
	pop es
	mov edi,fs:cv_descriptor
	call cv_validate
	jc entry_bad
	call cv_register_callback
	jnc entry_done
entry_bad:
	test edi,edi
	jz entry_done
	mov dword ptr es:[edi+CVSCHED_LAST_ERROR],CVERR_REGISTER
	or dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_FATAL
	inc dword ptr es:[edi+CVSCHED_FAILURES]
entry_done:
	mov dword ptr ss:cvdpmi_entry_pending,0
	pop fs
	pop es
	pop ds
	popad
	popfd
	ret
cvdpmi_entry_break endp

; Guest IF belongs to the DPMI client. Physical IF stays available to the
; bounded host scheduler. TF is used only inside a CLI-started compatibility
; region (and one instruction after STI), never as a timeout to enable IRQs.
; ECX -> IRET32, EDX -> saved original guest EAX, both addressed through SS.
cvdpmi_cli_enter proc near public
 pushad
 push ds
 push es
 push fs
 push gs
 push byte ptr _CSALIAS_
 pop fs
 cmp dword ptr ss:cvdpmi_client_active,0
 je if_cli_done
 mov ebp,ecx
 mov fs:cv_if_eax,edx
 mov eax,ss:[ebp-12]
 mov fs:cv_if_ds,eax
 lea esi,[esp+16]
 call cv_if_capture
 call cv_if_capture_cli
 call cv_if_disable
 cmp dword ptr fs:cv_if_trace,0
 jne if_cli_already_tracing
 mov eax,ss:[ebp+8]
 and eax,100h
 mov fs:cv_if_guest_tf,eax
if_cli_already_tracing:
 mov dword ptr fs:cv_if_steps,0
 mov dword ptr fs:cv_if_trace,1
 mov dword ptr fs:cv_if_shadow,0
 call cv_if_prepare
if_cli_done:
 pop gs
 pop fs
 pop es
 pop ds
 popad
 ret
cvdpmi_cli_enter endp

; ECX -> IRET32; STI has an interrupt shadow through the next instruction.
cvdpmi_cli_exit proc near public
 pushad
 push ds
 push es
 push fs
 push gs
 push byte ptr _CSALIAS_
 pop fs
 cmp dword ptr ss:cvdpmi_client_active,0
 je if_sti_done
 mov ebp,ecx
 mov fs:cv_if_eax,edx
 mov eax,ss:[ebp-12]
 mov fs:cv_if_ds,eax
 lea esi,[esp+16]
 call cv_if_capture
 call cv_if_capture_cli
 cmp dword ptr fs:cv_if_trace,0
 jne if_sti_tracing
 mov eax,ss:[ebp+8]
 and eax,100h
 mov fs:cv_if_guest_tf,eax
 mov dword ptr fs:cv_if_steps,0
 call cv_if_disable
if_sti_tracing:
 mov dword ptr fs:cv_if_enabled,1
 mov dword ptr fs:cv_if_trace,1
 mov dword ptr fs:cv_if_shadow,1
 call cv_if_prepare
if_sti_done:
 pop gs
 pop fs
 pop es
 pop ds
 popad
 ret
cvdpmi_cli_exit endp

; Internal, FS=client data alias, guest frame EBP in SS.
cv_if_disable proc
 cmp dword ptr fs:cvdpmi_cli_stepping,0
 jne if_disable_saved
 in al,21h
 mov fs:cv_saved_pic_master,al
 in al,0A1h
 mov fs:cv_saved_pic_slave,al
 mov dword ptr fs:cvdpmi_cli_stepping,1
if_disable_saved:
 mov al,0FEh
 out 21h,al
 mov al,0FFh
 out 0A1h,al
 mov dword ptr fs:cv_if_enabled,0
 or dword ptr ss:[ebp+8],200h
 ret
cv_if_disable endp

cv_if_enable proc
 mov dword ptr fs:cv_if_enabled,1
 cmp dword ptr fs:cvdpmi_cli_stepping,0
 je if_enable_saved
 mov al,fs:cv_saved_pic_slave
 out 0A1h,al
 mov al,fs:cv_saved_pic_master
 out 21h,al
 mov dword ptr fs:cvdpmi_cli_stepping,0
if_enable_saved:
 mov dword ptr fs:cv_if_trace,0
 mov dword ptr fs:cv_if_shadow,0
 mov dword ptr fs:cv_if_resume,0
 and dword ptr ss:[ebp+8],not 100h
 mov eax,fs:cv_if_guest_tf
 or ss:[ebp+8],eax
 or dword ptr ss:[ebp+8],200h
 ret
cv_if_enable endp

; CF=0: this physical IRQ0 has been acknowledged and deferred. No guest IRQ
; handler is entered. CF=1: use the ordinary upstream IRQ0 delivery path.
cvdpmi_if_irq0 proc near public
 push eax
 push fs
 push byte ptr _CSALIAS_
 pop fs
 cmp dword ptr ss:cvdpmi_client_active,0
 je if_irq_pass
 cmp dword ptr fs:cvdpmi_cli_stepping,0
 je if_irq_pass
 mov dword ptr fs:cv_if_pending,1
 mov al,20h
 out 20h,al
 pop fs
 pop eax
 clc
 ret
if_irq_pass:
 mov dword ptr fs:cv_if_pending,0
 pop fs
 pop eax
 stc
 ret
cvdpmi_if_irq0 endp

; The DPMI 0900/0901/0902 path passes its return IRET32 in ECX and AL=op.
; Returns previous state in AL, clears client CF, leaves physical IF enabled.
cvdpmi_if_api proc near public
 pushad
 push ds
 push es
 push fs
 push gs
 push byte ptr _CSALIAS_
 pop fs
 mov ebp,ecx
 mov esi,eax
 mov eax,fs:cv_if_enabled
 mov ss:[esp+44],al
 and dword ptr ss:[ebp+8],not 1
 cmp si,0902h
 je if_api_done
 cmp si,0901h
 je if_api_enable
 call cv_if_disable
 jmp if_api_done
if_api_enable:
 call cv_if_enable
if_api_done:
 pop gs
 pop fs
 pop es
 pop ds
 popad
 ret
cvdpmi_if_api endp

; Called BEFORE upstream intr01 handles debug causes. The monitor consumes
; only its own single-step. Guest TF and DR0..3/BT causes remain observable.
cvdpmi_if_debug proc near public
 pushad
 push ds
 push es
 push fs
 push gs
 mov ebp,esp
 add ebp,52
 push byte ptr _CSALIAS_
 pop fs
 cmp dword ptr ss:cvdpmi_client_active,0
 je if_debug_chain
 cmp dword ptr fs:cv_if_trace,0
 je if_debug_chain
 mov eax,dr6
 test eax,4000h
 jz if_debug_chain
 test eax,800Fh
 jnz if_debug_chain
 cmp dword ptr fs:cv_if_guest_tf,0
 jne if_debug_guest_tf
 and eax,not 4000h
 mov dr6,eax
 cmp dword ptr fs:cv_if_shadow,0
 jne if_debug_shadow_done
 ; No instruction budget: a long virtual-CLI region is legal and must not
 ; kill the client. Physical IF stays 1, so host ticks keep running; the
 ; count is diagnostic only.
 inc dword ptr fs:cv_if_steps
 lea eax,[ebp-8]
 mov fs:cv_if_eax,eax
 mov eax,ss:[ebp-40]
 mov fs:cv_if_ds,eax
 lea esi,[ebp-36]
 call cv_if_capture
 call cv_if_prepare
 jmp if_debug_handled
if_debug_shadow_done:
 call cv_if_enable
if_debug_handled:
 pop gs
 pop fs
 pop es
 pop ds
 popad
 clc
 ret
if_debug_guest_tf:
 cmp dword ptr fs:cv_if_shadow,0
 je if_debug_chain
 call cv_if_enable
if_debug_chain:
 pop gs
 pop fs
 pop es
 pop ds
 popad
 stc
 ret
cvdpmi_if_debug endp

; Tail entry with the original IRET32 at SS:ESP. A deferred timer interrupt
; is coalesced once, just like one PIC IRR bit, and enters HDPMI's existing
; locked-stack interrupt path. The original physical IRQ was already EOIed.
cvdpmi_if_return proc near public
 pushad
 push ds
 push es
 push fs
 push gs
 push byte ptr _CSALIAS_
 pop fs
 cmp dword ptr ss:cvdpmi_client_active,0
 je if_resume_done
 cmp dword ptr fs:cv_if_resume,0
 je if_resume_done
 lea ebp,[esp+48]
 test byte ptr ss:[ebp+4],3
 jz if_resume_done
 cmp dword ptr fs:cv_if_trace,0
 je if_resume_done
 mov eax,ss:[ebp+4]
 cmp ax,word ptr fs:cv_if_resume_cs
 jne if_resume_done
 mov eax,ss:[ebp]
 cmp eax,fs:cv_if_resume_eip
 jne if_resume_done
 mov dword ptr fs:cv_if_resume,0
 lea eax,[ebp-4]
 mov fs:cv_if_eax,eax
 mov eax,ss:[ebp-36]
 mov fs:cv_if_ds,eax
 lea esi,[ebp-32]
 call cv_if_capture
 cmp dword ptr fs:cv_if_shadow,0
 jne if_resume_shadow
 call cv_if_prepare
 jmp if_resume_done
if_resume_shadow:
 call cv_if_enable
if_resume_done:
 pop gs
 pop fs
 pop es
 pop ds
 popad
 push eax
 push fs
 push byte ptr _CSALIAS_
 pop fs
 cmp dword ptr ss:cvdpmi_client_active,0
 je if_return_plain
 cmp dword ptr fs:cv_if_enabled,0
 je if_return_plain
 cmp dword ptr fs:cv_if_shadow,0
 jne if_return_plain
 cmp dword ptr fs:cv_if_pending,0
 je if_return_plain
 test byte ptr fs:cv_saved_pic_master,1
 jnz if_return_plain
 mov dword ptr fs:cv_if_pending,0
 pop fs
 pop eax
 pushd offset r3vect08
 jmp lpms_call_int
if_return_plain:
 pop fs
 pop eax
 iretd
cvdpmi_if_return endp

; Validate a flat linear guest range before reading/writing from ring0.
; EAX=linear, ECX=bytes, BL=required PTE bits (5 read,7 write).
cv_if_memory proc
 pushad
 push es
 push ds
 push byte ptr _FLATSEL_
 pop es
 push ss
 pop ds
 mov edx,eax
 add edx,ecx
 jc if_memory_bad
 dec edx
 and eax,0FFFFF000h
 and edx,0FFFFF000h
if_memory_page:
 push eax
 call pm_Linear2PT
 pop eax
 jc if_memory_bad
 mov ecx,es:[edi]
 and cl,bl
 cmp cl,bl
 jne if_memory_bad
 cmp eax,edx
 je if_memory_ok
 add eax,1000h
 jmp if_memory_page
if_memory_ok:
 pop ds
 pop es
 popad
 clc
 ret
if_memory_bad:
 pop ds
 pop es
 popad
 stc
 ret
cv_if_memory endp

; Before allowing one native instruction, emulate flag-stack operations that
; would expose our TF or silently discard a legacy POPF restoration. Only
; Segmented stacks use their actual SS.B address size.
cv_if_prepare proc
 cmp dword ptr fs:cv_if_guest_tf,0
 jne if_prepare_native_guest_tf
 mov dword ptr fs:cv_if_control_budget,16
if_prepare_again:
 dec dword ptr fs:cv_if_control_budget
 jz cv_if_abort
 mov eax,ss:[ebp+4]
 verr ax
 jnz cv_if_abort
 lsl ecx,eax
 jnz cv_if_abort
 mov es,ax
 lar eax,eax
 test eax,400000h
 setnz dl
 movzx edx,dl
 mov fs:cv_if_address32,edx
 mov fs:cv_if_default32,edx
 mov dword ptr fs:cv_if_explicit_seg,0
 mov eax,fs:cv_if_ds
 mov fs:cv_if_segment,eax
 shl edx,1
 add edx,2                         ; default operand width2/4
 mov fs:cv_if_operand,edx
 mov esi,ss:[ebp]
 xor edi,edi
if_prepare_prefix:
 cmp edi,15
 jae cv_if_abort
 cmp esi,ecx
 ja cv_if_abort
 mov al,es:[esi]
 inc esi
 inc edi
 cmp al,66h
 je if_prepare_opsize
 cmp al,67h
 je if_prepare_addrsize
 cmp al,26h
 je if_prepare_es
 cmp al,2Eh
 je if_prepare_cs
 cmp al,36h
 je if_prepare_ss
 cmp al,3Eh
 je if_prepare_ds
 cmp al,64h
 je if_prepare_fs
 cmp al,65h
 je if_prepare_gs
 cmp al,9Ch
 je if_prepare_push
 cmp al,9Dh
 je if_prepare_pop
 cmp al,0CFh
 je if_prepare_iret
 cmp al,0CDh
 je if_prepare_int31
 cmp al,17h
 je if_prepare_pop_ss
 cmp al,8Eh
 jne if_prepare_not_movseg
 mov ah,es:[esi]
 and ah,38h
 cmp ah,10h
 je if_prepare_mov_ss
if_prepare_not_movseg:
 cmp al,0Fh
 jne if_prepare_other
 cmp byte ptr es:[esi],0B2h
 je if_prepare_lss                 ; LSS suppresses debug delivery
if_prepare_other:
 cmp al,0F1h
 je cv_if_abort
 ; Guest/debugger TF is not borrowed. Standard CLI/STI remains supported;
 ; legacy flag-stack restoration is not claimed while a guest debugger owns TF.
 cmp dword ptr fs:cv_if_guest_tf,0
 jne if_prepare_native_guest_tf
 or dword ptr ss:[ebp+8],100h
if_prepare_native_guest_tf:
 or dword ptr ss:[ebp+8],200h
 ret
if_prepare_es:
 mov eax,fs:cv_if_sreg[0]
 jmp if_prepare_segment
if_prepare_cs:
 mov eax,ss:[ebp+4]
 jmp if_prepare_segment
if_prepare_ss:
 mov eax,ss:[ebp+16]
 jmp if_prepare_segment
if_prepare_ds:
 mov eax,fs:cv_if_ds
 jmp if_prepare_segment
if_prepare_fs:
 mov eax,fs:cv_if_sreg[16]
 jmp if_prepare_segment
if_prepare_gs:
 mov eax,fs:cv_if_sreg[20]
if_prepare_segment:
 mov fs:cv_if_segment,eax
 mov dword ptr fs:cv_if_explicit_seg,1
 jmp if_prepare_prefix
if_prepare_addrsize:
 mov eax,fs:cv_if_default32
 xor eax,1
 mov fs:cv_if_address32,eax
 jmp if_prepare_prefix
if_prepare_opsize:
 mov eax,6
 sub eax,edx
 mov fs:cv_if_operand,eax
 jmp if_prepare_prefix
if_prepare_int31:
 cmp esi,ecx
 ja cv_if_abort
 cmp byte ptr es:[esi],21h
 je if_prepare_int21
 ; HDPMI's own ring-3 stubs (default vectors, IRQ return) enter the host
 ; through INT 30h; INT clears TF and the host's IRET restores it.
 cmp word ptr ss:[ebp+4],_INTSEL_
 je if_prepare_other
 cmp byte ptr es:[esi],31h
 jne cv_if_abort
 inc esi
 mov ebx,fs:cv_if_eax
 mov eax,ss:[ebx]
 cmp ax,0900h
 jb if_prepare_service
 cmp ax,0902h
 ja if_prepare_service
 mov edx,fs:cv_if_enabled
 mov ss:[ebx],dl
 and dword ptr ss:[ebp+8],not 1
 cmp ax,0902h
 je if_control_done
 cmp ax,0901h
 je if_control_api_enable
 call cv_if_disable
 jmp if_control_done
if_control_api_enable:
 call cv_if_enable
 jmp if_control_done
if_prepare_int21:
 inc esi
if_prepare_service:
 ; The ordinary host dispatcher owns the service. Resume this bounded
 ; compatibility region at its common return, before another client opcode.
 ; Only the exact return CS:EIP re-arms it: a client handler may take the
 ; INT and return with a native IRET, and an unrelated later host return
 ; must never receive the monitor's TF.
 mov dword ptr fs:cv_if_resume,1
 mov eax,ss:[ebp+4]
 mov fs:cv_if_resume_cs,eax
 mov fs:cv_if_resume_eip,esi
 and dword ptr ss:[ebp+8],not 100h
 or dword ptr ss:[ebp+8],200h
 ret
if_prepare_push:
 mov eax,ss:[ebp+8]
 and eax,not (100h or 30000h)
 or eax,fs:cv_if_guest_tf
 mov fs:cv_if_value,eax
 mov eax,ss:[ebp+12]
 sub eax,fs:cv_if_operand
 mov fs:cv_if_stack_offset,eax
 mov bl,7
 call cv_if_stack
 mov eax,fs:cv_if_value
 cmp dword ptr fs:cv_if_operand,2
 jne if_push32
 mov ds:[edi],ax
 jmp if_push_done
if_push32:
 mov ds:[edi],eax
if_push_done:
 mov eax,fs:cv_if_stack_offset
 cmp dword ptr fs:cv_if_stack32,0
 jne if_push_sp32
 mov word ptr ss:[ebp+12],ax
 jmp if_control_done
if_push_sp32:
 mov ss:[ebp+12],eax
 jmp if_control_done
if_prepare_mov_ss:
 call cv_if_effective_address
 jc if_movss_selector_ready
if_movss_address:
 mov ecx,2
 call cv_if_far_read
 movzx eax,word ptr ds:[edi]
if_movss_selector_ready:
 call cv_if_validate_ss
 mov word ptr ss:[ebp+16],ax
 mov ss:[ebp],esi
 jmp if_prepare_again
if_prepare_lss:
 ; LSS (E)SP,m16:16/32 (a DOS extender's stack switch back): emulated like
 ; MOV SS, since the CPU would hold the trap past the next instruction.
 ; Other destination registers are not needed by the known clients.
 inc esi
 mov al,es:[esi]
 cmp al,0C0h
 jae cv_if_abort
 and al,38h
 cmp al,20h
 jne cv_if_abort
 call cv_if_effective_address
 jc cv_if_abort
 mov ecx,fs:cv_if_operand
 add ecx,2
 call cv_if_far_read
 mov ecx,fs:cv_if_operand
 movzx eax,word ptr ds:[edi+ecx]
 call cv_if_validate_ss
 mov word ptr ss:[ebp+16],ax
 cmp dword ptr fs:cv_if_operand,2
 jne if_lss_offset32
 mov ax,ds:[edi]
 mov word ptr ss:[ebp+12],ax
 jmp if_lss_done
if_lss_offset32:
 mov eax,ds:[edi]
 mov ss:[ebp+12],eax
if_lss_done:
 mov ss:[ebp],esi
 jmp if_prepare_again
if_prepare_pop_ss:
 mov eax,ss:[ebp+12]
 mov fs:cv_if_stack_offset,eax
 mov bl,5
 call cv_if_stack
 movzx eax,word ptr ds:[edi]
 call cv_if_validate_ss
 mov word ptr ss:[ebp+16],ax
 mov ecx,fs:cv_if_operand
 cmp dword ptr fs:cv_if_stack32,0
 jne if_popss_sp32
 add word ptr ss:[ebp+12],cx
 jmp if_popss_done
if_popss_sp32:
 add ss:[ebp+12],ecx
if_popss_done:
 mov ss:[ebp],esi
 ; Emulation avoids the CPU's debug inhibition, while keeping any pending
 ; STI interrupt shadow through the instruction following the SS load.
 jmp if_prepare_again
if_prepare_pop:
 mov eax,ss:[ebp+12]
 mov fs:cv_if_stack_offset,eax
 mov bl,5
 call cv_if_stack
 xor eax,eax
 cmp dword ptr fs:cv_if_operand,2
 jne if_pop32
 mov ax,ds:[edi]
 mov edx,ss:[ebp+8]
 and edx,0FFFF0000h
 or eax,edx
 jmp if_pop_read
if_pop32:
 mov eax,ds:[edi]
if_pop_read:
 test eax,4000h                   ; NT cannot be approximated by a flags copy
 jnz cv_if_abort
 mov edx,eax
 and edx,100h
 mov fs:cv_if_guest_tf,edx
 mov edx,ss:[ebp+8]
 and edx,not 240DD5h
 and eax,240FD5h                  ; arithmetic/TF/DF/AC/ID plus logical IF
 or edx,eax
 or edx,202h                      ; physical IF and architectural bit1
 mov ss:[ebp+8],edx
 mov ecx,fs:cv_if_operand
 cmp dword ptr fs:cv_if_stack32,0
 jne if_pop_sp32
 add word ptr ss:[ebp+12],cx
 jmp if_pop_sp_done
if_pop_sp32:
 add ss:[ebp+12],ecx
if_pop_sp_done:
 test eax,200h
 jz if_control_done
 call cv_if_enable
if_control_done:
 mov ss:[ebp],esi
if_control_resume:
 cmp dword ptr fs:cv_if_shadow,0
 je if_control_continue
 call cv_if_enable                ; emulated following instruction ends shadow
if_control_continue:
 cmp dword ptr fs:cv_if_trace,0
 jne if_prepare_again
 ret
; Same-privilege IRET/IRETD only: a ring-3 client can return only to ring 3.
; NT (task return) and VM (V86 return) images are refused, never approximated.
; The popped image supplies logical IF exactly as POPF does.
if_prepare_iret:
 mov eax,fs:cv_if_operand
 push eax
 lea eax,[eax+eax*2]
 mov fs:cv_if_operand,eax           ; validate EIP, CS and FLAGS together
 mov eax,ss:[ebp+12]
 mov bl,5
 call cv_if_stack
 pop eax
 mov fs:cv_if_operand,eax
 cmp eax,2
 jne if_iret32
 movzx ecx,word ptr ds:[edi]
 movzx edx,word ptr ds:[edi+2]
 movzx eax,word ptr ds:[edi+4]
 mov ebx,ss:[ebp+8]
 and ebx,0FFFF0000h
 or eax,ebx
 jmp if_iret_loaded
if_iret32:
 mov ecx,ds:[edi]
 movzx edx,word ptr ds:[edi+4]
 mov eax,ds:[edi+8]
if_iret_loaded:
 test eax,24000h                   ; NT or VM
 jnz cv_if_abort
 mov ebx,edx
 and ebx,3
 cmp ebx,3
 jne cv_if_abort
 lar ebx,edx
 jnz cv_if_abort
 test ebx,8000h                    ; present
 jz cv_if_abort
 mov esi,ebx
 and esi,1800h
 cmp esi,1800h                     ; code segment descriptor
 jne cv_if_abort
 test ebx,400h                     ; conforming code accepts any DPL <= 3
 jnz if_iret_privilege
 and ebx,6000h
 cmp ebx,6000h
 jne cv_if_abort
if_iret_privilege:
 lsl ebx,edx
 jnz cv_if_abort
 cmp ecx,ebx
 ja cv_if_abort
 mov ss:[ebp],ecx
 mov ss:[ebp+4],edx
 mov edx,eax
 and edx,100h
 mov fs:cv_if_guest_tf,edx
 mov edx,ss:[ebp+8]
 and edx,not 240DD5h
 mov ebx,eax
 and ebx,240FD5h
 or edx,ebx
 or edx,202h
 mov ss:[ebp+8],edx
 mov ecx,fs:cv_if_operand
 lea ecx,[ecx+ecx*2]
 cmp dword ptr fs:cv_if_stack32,0
 jne if_iret_sp32
 add word ptr ss:[ebp+12],cx
 jmp if_iret_flags
if_iret_sp32:
 add ss:[ebp+12],ecx
if_iret_flags:
 test eax,200h
 jz if_control_resume
 call cv_if_enable
 jmp if_control_resume
cv_if_prepare endp

; EAX=guest stack offset, BL=5read/7write; returns DS:EDI validated pointer.
cv_if_stack proc
 mov edi,eax
 mov ecx,ss:[ebp+16]
 verr cx
 jnz cv_if_abort
 cmp bl,7
 jne if_stack_rights
 verw cx
 jnz cv_if_abort
if_stack_rights:
 lar edx,ecx
 mov dword ptr fs:cv_if_stack32,1
 test edx,400000h
 jnz if_stack_size_done
 mov dword ptr fs:cv_if_stack32,0
 movzx edi,di
if_stack_size_done:
 mov fs:cv_if_stack_offset,edi
 test edx,400h                     ; reject expand-down segments in this path
 jnz cv_if_abort
 lsl edx,ecx
 mov eax,edi
 add eax,fs:cv_if_operand
 jc cv_if_abort
 dec eax
 cmp eax,edx
 ja cv_if_abort
 mov ds,cx
 movzx eax,cx
 push eax
 call _cvdpmi_selector_base
 add esp,4
 add eax,edi
 jc cv_if_abort
 mov ecx,fs:cv_if_operand
 call cv_if_memory
 jc cv_if_abort
 ret
cv_if_stack endp

; ESI points to the saved PUSHAD image immediately after the segment saves.
cv_if_capture proc
 xor edi,edi
if_capture_gprs:
 mov eax,ss:[esi+edi]
 mov fs:cv_if_gpr[edi],eax
 add edi,4
 cmp edi,32
 jb if_capture_gprs
 mov eax,ss:[ebp+12]
 mov fs:cv_if_gpr[12],eax
 mov eax,ss:[esi-8]
 mov fs:cv_if_sreg[0],eax
 mov eax,ss:[ebp+4]
 mov fs:cv_if_sreg[4],eax
 mov eax,ss:[ebp+16]
 mov fs:cv_if_sreg[8],eax
 mov eax,fs:cv_if_ds
 mov fs:cv_if_sreg[12],eax
 mov eax,ss:[esi-12]
 mov fs:cv_if_sreg[16],eax
 mov eax,ss:[esi-16]
 mov fs:cv_if_sreg[20],eax
 ret
cv_if_capture endp
cv_if_capture_cli proc
 ; The official CLI decoder has already borrowed EAX/ESI/ECX/EDX.
 mov eax,ss:[ebp-20]
 mov fs:cv_if_gpr[28],eax
 mov eax,ss:[ebp-16]
 mov fs:cv_if_gpr[4],eax
 mov eax,ss:[ebp-24]
 mov fs:cv_if_gpr[24],eax
 mov eax,ss:[ebp-28]
 mov fs:cv_if_gpr[20],eax
 ret
cv_if_capture_cli endp
; EAX register number -> EAX saved client register.
cv_if_reg proc
 and eax,7
 xor eax,7
 mov eax,fs:cv_if_gpr[eax*4]
 ret
cv_if_reg endp

; Decode only the MOV SS r/m16 address; no guest instruction is executed.
; CF1 returns a register selector in AX. CF0 returns segment:EDI.
cv_if_effective_address proc
 movzx eax,byte ptr es:[esi]
 inc esi
 mov fs:cv_if_modrm,eax
 cmp eax,0C0h
 jb if_ea_memory
 call cv_if_reg
 stc
 ret
if_ea_memory:
 cmp dword ptr fs:cv_if_address32,0
 jne if_ea32
 and eax,7
 cmp eax,6
 jne if_ea16_regs
 cmp dword ptr fs:cv_if_modrm,40h
 jae if_ea16_regs
 movzx edi,word ptr es:[esi]
 add esi,2
 jmp if_ea_ready
if_ea16_regs:
 xor edi,edi
 cmp eax,4
 jae if_ea16_one
 cmp eax,2
 jae if_ea16_bp
 mov edi,fs:cv_if_gpr[16]           ; BX
 jmp if_ea16_index
if_ea16_bp:
 mov edi,fs:cv_if_gpr[8]            ; BP
 call cv_if_ea_ss
if_ea16_index:
 test al,1
 jnz if_ea16_di
 add edi,fs:cv_if_gpr[4]            ; SI
 jmp if_ea16_disp
if_ea16_di:
 add edi,fs:cv_if_gpr[0]            ; DI
 jmp if_ea16_disp
if_ea16_one:
 cmp eax,4
 jne if_ea16_one_di
 mov edi,fs:cv_if_gpr[4]
 jmp if_ea16_disp
if_ea16_one_di:
 cmp eax,5
 jne if_ea16_one_bp
 mov edi,fs:cv_if_gpr[0]
 jmp if_ea16_disp
if_ea16_one_bp:
 cmp eax,6
 jne if_ea16_one_bx
 mov edi,fs:cv_if_gpr[8]
 call cv_if_ea_ss
 jmp if_ea16_disp
if_ea16_one_bx:
 mov edi,fs:cv_if_gpr[16]
if_ea16_disp:
 mov eax,fs:cv_if_modrm
 and eax,0C0h
 cmp eax,40h
 je if_ea_disp8
 cmp eax,80h
 jne if_ea_ready
 movzx eax,word ptr es:[esi]
 add esi,2
 add edi,eax
 jmp if_ea_ready
if_ea32:
 xor edi,edi
 and eax,7
 cmp eax,4
 je if_ea_sib
 cmp eax,5
 jne if_ea32_base
 cmp dword ptr fs:cv_if_modrm,40h
 jb if_ea_disp32
if_ea32_base:
 cmp eax,5
 jne if_ea32_base_ready
 call cv_if_ea_ss
if_ea32_base_ready:
 call cv_if_reg
 mov edi,eax
 jmp if_ea32_disp
if_ea_sib:
 movzx edx,byte ptr es:[esi]
 inc esi
 mov eax,edx
 shr eax,3
 and eax,7
 cmp eax,4
 je if_ea_sib_base
 call cv_if_reg
 mov ecx,edx
 shr ecx,6
 shl eax,cl
 mov edi,eax
if_ea_sib_base:
 mov eax,edx
 and eax,7
 cmp eax,5
 jne if_ea_sib_base_reg
 cmp dword ptr fs:cv_if_modrm,40h
 jb if_ea_disp32
if_ea_sib_base_reg:
 cmp eax,4
 je if_ea_sib_ss
 cmp eax,5
 jne if_ea_sib_reg_ready
if_ea_sib_ss:
 call cv_if_ea_ss
if_ea_sib_reg_ready:
 call cv_if_reg
 add edi,eax
if_ea32_disp:
 mov eax,fs:cv_if_modrm
 and eax,0C0h
 cmp eax,40h
 je if_ea_disp8
 cmp eax,80h
 jne if_ea_ready
if_ea_disp32:
 add edi,dword ptr es:[esi]
 add esi,4
 jmp if_ea_ready
if_ea_disp8:
 movsx eax,byte ptr es:[esi]
 inc esi
 add edi,eax
if_ea_ready:
 cmp dword ptr fs:cv_if_address32,0
 jne if_ea_address_done
 movzx edi,di
if_ea_address_done:
 clc
 ret
cv_if_effective_address endp
cv_if_ea_ss proc
 cmp dword ptr fs:cv_if_explicit_seg,0
 jne if_ea_ss_done
 push eax
 mov eax,ss:[ebp+16]
 mov fs:cv_if_segment,eax
 pop eax
if_ea_ss_done:
 ret
cv_if_ea_ss endp

; cv_if_segment:EDI, ECX bytes -> DS:EDI, validated readable guest memory
; (selector limit, present user pages); anything else aborts the client.
cv_if_far_read proc
 mov edx,fs:cv_if_segment
 verr dx
 jnz cv_if_abort
 lsl eax,edx
 lea ecx,[ecx+edi-1]               ; last byte
 cmp ecx,edi
 jb cv_if_abort
 cmp ecx,eax
 ja cv_if_abort
 sub ecx,edi
 inc ecx
 mov ds,dx
 movzx eax,dx
 push ecx
 push eax
 call _cvdpmi_selector_base
 add esp,4
 pop ecx
 add eax,edi
 jc cv_if_abort
 mov bl,5
 call cv_if_memory
 jc cv_if_abort
 ret
cv_if_far_read endp

cv_if_validate_ss proc
 mov ecx,eax
 and ecx,3
 cmp ecx,3
 jne cv_if_abort
 lar edx,eax
 jnz cv_if_abort
 and edx,0FF00h
 and edx,not 500h                 ; ignore accessed and expand-down bits
 cmp edx,0F200h                   ; present/DPL3/writable data, not code
 jne cv_if_abort
 ret
cv_if_validate_ss endp

cv_if_diaghex proc
 mov esi,eax
 mov bp,8
if_diag_next:
 rol esi,4
 mov ecx,10000h
 mov dx,3FDh
if_diag_poll:
 in al,dx
 test al,20h
 jnz if_diag_write
 loop if_diag_poll
if_diag_write:
 mov eax,esi
 and al,15
 add al,'0'
 cmp al,'9'
 jbe if_diag_digit
 add al,7
if_diag_digit:
 mov dx,3F8h
 out dx,al
 dec bp
 jnz if_diag_next
 ret
cv_if_diaghex endp
cv_if_abort:
 mov ax,word ptr ss:[ebp+4]
 mov es,ax
 mov esi,ss:[ebp]
 mov eax,es:[esi]
 push ebp
 call cv_if_diaghex
 pop ebp
 push ebp
 mov eax,ss:[ebp]
 call cv_if_diaghex
 pop ebp
 push ebp
 mov eax,ss:[ebp+4]
 call cv_if_diaghex
 pop ebp
 push byte ptr _FLATSEL_
 pop es
 mov edi,fs:cv_descriptor
 call cv_validate
 jc if_abort_exit
 mov eax,ss:[ebp]
 mov es:[edi+CVSCHED_LAST_EIP],eax
 mov dword ptr es:[edi+CVSCHED_LAST_ERROR],15
 or dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_FATAL
 inc dword ptr es:[edi+CVSCHED_FAILURES]
if_abort_exit:
 mov ax,0C10Fh
 jmp _exitclientEx

; C-callable selector helpers used by the freestanding instruction emulator.
; Client selectors may name either the LDT or GDT; the returned base is linear.
_cvdpmi_selector_base proc near public
	push ebx
	push edx
	push es
	movzx ebx,word ptr [esp+16]
	lar eax,ebx
	jnz selector_base_bad
	test bl,4
	jnz selector_base_ldt
	and ebx,0FFF8h
	mov edx,fs:cv_gdt_base
	jmp selector_base_got_table
selector_base_ldt:
	and ebx,0FFF8h
	mov edx,fs:cv_ldt_base
selector_base_got_table:
	add ebx,edx
	push byte ptr _FLATSEL_
	pop es
	mov ah,es:[ebx+7]
	mov al,es:[ebx+4]
	shl eax,16
	mov ax,es:[ebx+2]
	jmp selector_base_done
selector_base_bad:
	xor eax,eax
selector_base_done:
	pop es
	pop edx
	pop ebx
	ret
_cvdpmi_selector_base endp

_cvdpmi_selector_dbit proc near public
	movzx eax,word ptr [esp+4]
	lar eax,eax
	jnz selector_dbit_bad
	shr eax,22
	and eax,1
	ret
selector_dbit_bad:
	xor eax,eax
	ret
_cvdpmi_selector_dbit endp

; Called at HDPMI's ring-0 #PF entry while the original ring-3 frame is still
; on the host stack.  The push order is the contract in
; hdpmi_video_adapter.h.  CF clear means the faulting instruction was executed.
cvdpmi_video_fault proc near public
	; The client's EAX is still live here: the frame below is its register
	; image, and a chained fault must reach HDPMI unchanged.
	push eax
	.586p
	mov eax,cr2
	.386p
	cmp eax,0A0000h
	jb video_fault_not_aperture
	cmp eax,0C0000h
	jae video_fault_not_aperture
	cmp dword ptr ss:cvdpmi_client_active,0
	je video_fault_not_aperture
	pop eax
	pushad
	push ds
	push es
	push fs
	push gs
	mov ebp,esp
	push byte ptr _CSALIAS_
	pop fs
	cmp dword ptr fs:cv_video_shared,0
	je video_fault_pop_chain
	mov al,1
	xchg al,fs:cv_video_busy
	test al,al
	jnz video_fault_pop_chain
	push byte ptr _FLATSEL_
	pop ds
	push byte ptr _FLATSEL_
	pop es
	mov eax,ss:pdGDT.dwBase
	mov fs:cv_gdt_base,eax
	mov eax,ss:dwLDTAddr
	mov fs:cv_ldt_base,eax
	; The HDPMI ring-0 SS has a relocated base.  OpenWatcom's flat-model C
	; passes pointers to locals as absolute offsets, so run it on an owned
	; stack addressed by the base-zero flat selector.  Keep EBP pointing at
	; the original exception image and pass its translated linear address.
	xor eax,eax
	mov ax,ss
	push eax
	call _cvdpmi_selector_base
	add esp,4
	add eax,ebp
	mov fs:cv_fault_frame,eax
	push _CSALIAS_
	call _cvdpmi_selector_base
	add esp,4
	add eax,offset cv_callback_stack_end
	mov fs:cv_stack_pointer,eax
	mov fs:cv_saved_stack,esp
	mov ax,ss
	mov word ptr fs:[cv_saved_stack+4],ax
	; HDPMI's interrupt/exception paths address resident globals through SS.
	; The freestanding C call needs a base-zero SS for flat-model local
	; pointers, so defer physical IRQs for this one bounded emulation slice.
	; Restore the exact host flags immediately after restoring HDPMI's SS.
	pushfd
	pop eax
	mov fs:cv_saved_host_flags,eax
	cli
	lss esp,fword ptr fs:cv_stack_pointer
	mov edx,fs:cv_video_shared
	inc dword ptr es:[edx+32]
	.586p
	mov eax,cr2
	.386p
	push fs:cv_video_shared
	push fs:cv_fault_frame
	call _cvdpmi_video_execute
	add esp,8
	lss esp,fword ptr fs:cv_saved_stack
	push byte ptr _CSALIAS_
	pop fs
	mov byte ptr fs:cv_video_busy,0
	push dword ptr fs:cv_saved_host_flags
	popfd
	test eax,eax
	jnz video_fault_pop_chain
	pop gs
	pop fs
	pop es
	pop ds
	popad
	clc
	ret
video_fault_pop_chain:
	pop gs
	pop fs
	pop es
	pop ds
	popad
video_fault_chain:
	stc
	ret
video_fault_not_aperture:
	pop eax
	stc
	ret
cvdpmi_video_fault endp

cvdpmi_attach proc near public
	pushad
	push ds
	push es
	push fs
	push byte ptr _CSALIAS_
	pop fs
	push byte ptr _FLATSEL_
	pop es
	mov edi,ss:cvdpmi_shared_linear
	test edi,edi
	jz attach_optional
	call cv_validate
	jc attach_descriptor
	mov eax,es:[edi+CVSCHED_STATE]
	mov edx,eax
	and edx,CVSCHED_STATE_BOUND or CVSCHED_STATE_SESSION or CVSCHED_STATE_JEMM or CVSCHED_STATE_DPMI_HOST
	cmp edx,CVSCHED_STATE_BOUND or CVSCHED_STATE_SESSION or CVSCHED_STATE_JEMM or CVSCHED_STATE_DPMI_HOST
	jne attach_descriptor
	test eax,CVSCHED_STATE_DPMI_CLIENT or CVSCHED_STATE_CALLBACK
	jnz attach_owned
	cmp fs:cv_handle,0
	jne attach_owned
	call cv_install_ptes
	jc attach_ptes
	mov fs:cv_descriptor,edi
	mov eax,ss:pdGDT.dwBase
	mov fs:cv_gdt_base,eax
	mov eax,ss:dwLDTAddr
	mov fs:cv_ldt_base,eax
	mov eax,es:[edi+CVSCHED_GENERATION]
	mov fs:cv_generation,eax
	call cv_map_video
	jc attach_video
	or dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_DPMI_CLIENT
	inc dword ptr es:[edi+CVSCHED_CLIENT_ENTRIES]
	mov dword ptr es:[edi+CVSCHED_ADAPTER_BYTES],CV_ADAPTER_BYTES
	.586p
	mov eax,cr4
	.386p
	mov fs:cv_saved_cr4,eax
	and eax,not 2
	.586p
	mov cr4,eax
	.386p
	mov dword ptr ss:cvdpmi_entry_pending,1
	mov dword ptr fs:cvdpmi_cli_stepping,0
	mov dword ptr fs:cv_if_enabled,1
	mov dword ptr fs:cv_if_trace,0
	mov dword ptr fs:cv_if_guest_tf,0
	mov dword ptr fs:cv_if_shadow,0
	mov dword ptr fs:cv_if_pending,0
	mov dword ptr fs:cv_if_resume,0
	mov dword ptr fs:cv_pm_irqs,0
	mov dword ptr ss:cvdpmi_irq_proc,0
	in al,21h
	mov fs:cv_host_pic_master,al
	and al,0FEh
	out 21h,al
	in al,0A1h
	mov fs:cv_host_pic_slave,al
	mov dword ptr ss:cvdpmi_client_active,1
	clc
	jmp attach_done
attach_optional:
	clc
	jmp attach_done
attach_descriptor:
	mov eax,CVERR_DESCRIPTOR
	jmp attach_error
attach_owned:
	mov eax,CVERR_OWNED
	jmp attach_error
attach_ptes:
	call cv_restore_ptes
	mov eax,CVERR_PTES
	jmp attach_error
attach_video:
	call cv_restore_ptes
	mov dword ptr fs:cv_descriptor,0
	mov dword ptr fs:cv_generation,0
	mov eax,CVERR_PTES
	jmp attach_error
attach_error:
	test edi,edi
	jz attach_cf
	mov es:[edi+CVSCHED_LAST_ERROR],eax
	inc dword ptr es:[edi+CVSCHED_FAILURES]
attach_cf:
	stc
attach_done:
	pop fs
	pop es
	pop ds
	popad
	ret
cvdpmi_attach endp

cvdpmi_detach proc near public
	pushad
	push ds
	push es
	push fs
	push byte ptr _CSALIAS_
	pop fs
	push byte ptr _FLATSEL_
	pop es
	mov edi,fs:cv_descriptor
	test edi,edi
	jz detach_optional
	call cv_validate
	jc detach_failed_no_record
	mov edx,fs:cv_handle
	test edx,edx
	jz detach_removed
	; Keep the installed shadow authoritative until the exact callback is gone.
	call cv_guard_ptes
	; Flush callback accounting even if the client exits before the next
	; physical timer tick.
	xor eax,eax
	xchg eax,fs:cv_pending_services
	add es:[edi+CVSCHED_SERVICES],eax
	xor eax,eax
	xchg eax,fs:cv_pending_reentries
	add es:[edi+CVSCHED_REENTRIES],eax
	mov eax,fs:cv_last_eip
	mov es:[edi+CVSCHED_LAST_EIP],eax
	mov eax,fs:cv_last_flags
	mov es:[edi+CVSCHED_LAST_FLAGS],eax
	mov eax,fs:cv_last_port
	mov es:[edi+CVSCHED_LAST_PORT],eax
	call cv_remove_devices
	jc detach_remove_failed
	mov edx,fs:cv_handle
	call is0007
	jc detach_remove_failed
	mov dword ptr ss:cvdpmi_client_active,0
	mov dword ptr fs:cv_handle,0
	mov dword ptr es:[edi+CVSCHED_DPMI_HANDLE],0
	and dword ptr es:[edi+CVSCHED_STATE],not CVSCHED_STATE_CALLBACK
	inc dword ptr es:[edi+CVSCHED_REMOVES]
detach_removed:
	cmp dword ptr fs:cvdpmi_cli_stepping,0
	je detach_cli_restored
	mov al,fs:cv_saved_pic_slave
	out 0A1h,al
	mov al,fs:cv_saved_pic_master
	out 21h,al
	mov dword ptr fs:cvdpmi_cli_stepping,0
detach_cli_restored:
	mov al,fs:cv_host_pic_slave
	out 0A1h,al
	mov al,fs:cv_host_pic_master
	out 21h,al
	call cv_unmap_video
	call cv_restore_ptes
	jc detach_restore_failed
	.586p
	mov eax,cr4
	.386p
	and eax,not 2
	mov edx,fs:cv_saved_cr4
	and edx,2
	or eax,edx
	.586p
	mov cr4,eax
	.386p
	mov dword ptr ss:cvdpmi_entry_pending,0
	mov dword ptr fs:cvdpmi_cli_stepping,0
	and dword ptr es:[edi+CVSCHED_STATE],not (CVSCHED_STATE_DPMI_CLIENT or CVSCHED_STATE_CALLBACK)
	inc dword ptr es:[edi+CVSCHED_CLIENT_EXITS]
	mov dword ptr fs:cv_descriptor,0
	mov dword ptr fs:cv_generation,0
	mov dword ptr fs:cv_faulted,0
	clc
	jmp detach_done
detach_optional:
	clc
	jmp detach_done
detach_remove_failed:
	call cv_guard_ptes
	mov dword ptr es:[edi+CVSCHED_LAST_ERROR],CVERR_REMOVE
	or dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_FATAL
	inc dword ptr es:[edi+CVSCHED_FAILURES]
	jmp detach_failed_no_record
detach_restore_failed:
	mov dword ptr es:[edi+CVSCHED_LAST_ERROR],CVERR_PTES
	or dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_FATAL
	inc dword ptr es:[edi+CVSCHED_FAILURES]
detach_failed_no_record:
	stc
detach_done:
	pop fs
	pop es
	pop ds
	popad
	ret
cvdpmi_detach endp

cvdpmi_mark_fault proc near public
	pushad
	push es
	push fs
	push byte ptr _CSALIAS_
	pop fs
	cmp dword ptr fs:cv_faulted,0
	jne marked
	mov dword ptr fs:cv_faulted,1
	push byte ptr _FLATSEL_
	pop es
	mov edi,fs:cv_descriptor
	call cv_validate
	jc marked
	inc dword ptr es:[edi+CVSCHED_FAULT_EXITS]
marked:
	pop fs
	pop es
	popad
	ret
cvdpmi_mark_fault endp

; Execute one scalar 8/16/32-bit port access (as byte cycles) while HDPMI is
; still in ring 0.  CF=1 on return: executed here (the host then skips the
; instruction without the ring-3 callback); CF=0: not handled.  ECX is the official EMUINSFR supplied by the shipped 3.24
; path, EDX is the decoded port, and AL bit 1 selects OUT.  The ring-3 callback only consumes the saved
; input byte and advances that same official frame.
cvdpmi_note_io proc near public
	pushfd
	pushad
	push ds
	push es
	push fs
	mov ebp,ecx
	mov ebx,eax
	movzx edi,dx
	push byte ptr _CSALIAS_
	pop fs
	cmp dword ptr ss:cvdpmi_client_active,0
	je note_io_done
	cmp edi,CV_FIRST_PORT
	jb note_io_device
	cmp edi,CV_LAST_VIDEO_PORT
	ja note_io_device
	mov fs:cv_last_port,edi
	mov eax,ss:[ebp+20]	;EMUINSFR + R3FAULT32.rIP
	mov fs:cv_last_eip,eax
	inc dword ptr fs:cv_pending_services
	mov edx,fs:cv_video_shared
	test edx,edx
	jz note_io_done
	mov al,1
	xchg al,fs:cv_callback_busy
	test al,al
	jnz note_io_done
	push byte ptr _FLATSEL_
	pop ds
	push byte ptr _FLATSEL_
	pop es
	mov eax,ss:pdGDT.dwBase
	mov fs:cv_gdt_base,eax
	mov eax,ss:dwLDTAddr
	mov fs:cv_ldt_base,eax
	; EMUINSFR begins with the saved client EAX in shipped HDPMI 3.24.
	mov esi,ss:[ebp]
	; IRQ0 is the physical host scheduling source.  A foreground client may
	; virtualize its own timer state, but it cannot mask that source in the
	; host PIC while ownership is active.  The exact pre-client IMR is saved
	; at attach and restored at detach.
	cmp edi,21h
	jne note_io_pic_ready
	test bl,2
	jz note_io_pic_ready
	mov eax,ss:[ebp]
	and eax,0FFFFFF00h
	mov edx,esi
	and edx,0FEh
	or eax,edx
	mov ss:[ebp],eax
note_io_pic_ready:
	; Cycle through all display-disable/retrace combinations.  They are
	; independent timing signals on hardware; exposing only 00/09 leaves old
	; Mode-X polling loops waiting forever for a mixed state.
	test bl,2
	jnz note_io_status_ready
	cmp edi,03DAh
	jne note_io_status_ready
	inc byte ptr fs:cv_status_phase
	and byte ptr fs:cv_status_phase,3
	movzx eax,byte ptr fs:cv_status_phase
	cmp eax,1
	je note_io_status_one
	cmp eax,2
	je note_io_status_nine
	cmp eax,3
	je note_io_status_eight
	xor eax,eax
	jmp note_io_status_store
note_io_status_one:
	mov eax,1
	jmp note_io_status_store
note_io_status_nine:
	mov eax,9
	jmp note_io_status_store
note_io_status_eight:
	mov eax,8
note_io_status_store:
	mov fs:cv_status1,eax
note_io_status_ready:
	push _CSALIAS_
	call _cvdpmi_selector_base
	add esp,4
	add eax,offset cv_callback_stack_end
	mov fs:cv_stack_pointer,eax
	mov fs:cv_saved_stack,esp
	mov ax,ss
	mov word ptr fs:[cv_saved_stack+4],ax
	pushfd
	pop eax
	mov fs:cv_saved_host_flags,eax
	; Access width from HDPMI's decoded flags in EMUINSFR.rErr: SI_WORD
	; (10h) alone is 16-bit, with SI_DWORD (20h) 32-bit. Wider cycles are
	; consecutive byte ports, as on the ISA bus (OUT DX,AX to 3C4h/3D4h).
	mov bh,1
	test byte ptr ss:[ebp+16],10h
	jz note_io_width
	mov bh,2
	test byte ptr ss:[ebp+16],20h
	jz note_io_width
	mov bh,4
note_io_width:
	cli
	lss esp,fword ptr fs:cv_stack_pointer
	test bl,2
	jnz note_io_write
	xor esi,esi
note_io_read_next:
	dec bh
	push byte ptr _CSALIAS_
	pop fs
	movzx eax,byte ptr fs:cv_status1
	push eax
	movzx eax,bh
	add eax,edi
	push eax
	push fs:cv_video_shared
	call _cvdpmi_video_port_read
	add esp,12
	shl esi,8
	movzx eax,al
	or esi,eax
	test bh,bh
	jnz note_io_read_next
	jmp note_io_called
note_io_write:
	push byte ptr _CSALIAS_
	pop fs
	mov eax,esi
	and eax,0FFh
	push eax
	push edi
	push fs:cv_video_shared
	call _cvdpmi_video_port_write
	add esp,12
	shr esi,8
	inc edi
	dec bh
	jnz note_io_write
note_io_called:
	lss esp,fword ptr fs:cv_saved_stack
	push byte ptr _CSALIAS_
	pop fs
	mov byte ptr fs:cv_callback_busy,0
	push dword ptr fs:cv_saved_host_flags
	popfd
	test bl,2
	jnz note_io_handled
	; Supply AL/AX/EAX in HDPMI's exact saved EMUINSFR.  The official ring-3
	; callback remains installed and advances the official exception frame;
	; it does not need a private data selector or a non-upstream ABI.
	mov ecx,0FFh
	test byte ptr ss:[ebp+16],10h
	jz note_io_mask
	mov ecx,0FFFFh
	test byte ptr ss:[ebp+16],20h
	jz note_io_mask
	or ecx,-1
note_io_mask:
	and esi,ecx
	not ecx
	mov eax,ss:[ebp]
	and eax,ecx
	or eax,esi
	mov ss:[ebp],eax
	jmp note_io_handled

; Device and PIC ports: CVSESSION DEV_IO through HDPMI's internal real-mode
; call (IF clear there, so no V86 interrupt is delivered inside the I/O).
note_io_device:
	cmp dword ptr ss:cvdpmi_jlm_entry,0
	je note_io_done
	test byte ptr ss:[ebp+16],8	; INS/OUTS are not bridged
	jnz note_io_done
	mov ecx,1
	test byte ptr ss:[ebp+16],10h
	jz note_io_device_width
	mov cl,2
	test byte ptr ss:[ebp+16],20h
	jz note_io_device_width
	mov cl,4
note_io_device_width:
	test bl,2			; AL bit 1 of the entry: OUT
	jz note_io_device_call
	or ch,1
note_io_device_call:
	push edi
	push ecx
	mov edx,ss:[ebp]
	mov ebx,edi
	mov eax,CV_OP_DEV_IO
	call cv_bridge_call
	mov fs:cv_pm_irqs,esi
	pop ecx
	pop edi
	test ch,1
	jnz note_io_handled
	; IN: merge the returned value into the client's saved EAX.
	mov eax,0FFh
	cmp cl,1
	je note_io_device_mask
	mov eax,0FFFFh
	cmp cl,2
	je note_io_device_mask
	or eax,-1
note_io_device_mask:
	and edx,eax
	not eax
	and ss:[ebp],eax
	or ss:[ebp],edx
note_io_handled:
	push ebp
	add ebp,20			; EMUINSFR.rIP: the client's IRET32
	call cv_pm_irq_check
	pop ebp
	; CF=1: the cycle was executed here; HDPMI advances the client past the
	; instruction itself and does not enter the ring-3 TRAPPROCS callback.
	or byte ptr [esp+4*3+4*8],1	; saved EFLAGS (below FS/ES/DS, PUSHAD)
	jmp note_io_exit
note_io_done:
	and byte ptr [esp+4*3+4*8],not 1
note_io_exit:
	pop fs
	pop es
	pop ds
	popad
	popfd
	ret
cvdpmi_note_io endp

; One CVSESSION operation through HDPMI's internal real-mode call: EAX op,
; EBX/ECX/EDX in; EBX/EDX/ESI out.  An I/O can arrive while HDPMI is inside
; a real-mode to protected-mode dispatch (a client IRQ handler, a real-mode
; callback): that interrupted real-mode context and the client's PM segment
; state are kept exactly as the official 0300h path does around its own
; real-mode call.  IF is clear throughout (see cvdpmi_devio_rm).
cv_bridge_call proc
	push ebp
	push edi
	pushfd
	cli
	; v86iret ESP, SS, ES, DS, FS, GS and the whole PMSTATE.
	.errnz V86IRET.rGSd - V86IRET.rESP - 20
	.errnz sizeof PMSTATE - 16
	mov ebp,6
bridge_save_v86:
	push dword ptr ss:v86iret.rESP[ebp*4-4]
	dec ebp
	jnz bridge_save_v86
	mov ebp,4
bridge_save_pm:
	push dword ptr ss:pmstate[ebp*4-4]
	dec ebp
	jnz bridge_save_pm
	pushd offset cvdpmi_devio_rm
	call callrmprocintern
	xor ebp,ebp
bridge_restore_pm:
	pop dword ptr ss:pmstate[ebp*4]
	inc ebp
	cmp ebp,4
	jb bridge_restore_pm
	xor ebp,ebp
bridge_restore_v86:
	pop dword ptr ss:v86iret.rESP[ebp*4]
	inc ebp
	cmp ebp,6
	jb bridge_restore_v86
	popfd
	pop edi
	pop ebp
	ret
cv_bridge_call endp

; Device IRQs for the protected-mode client (CVSESSION holds them instead of
; injecting them into V86, where they would nest inside a DOS/4GW handler).
; Clobbers EAX.
; At the end of an emulated I/O instruction the client is at an instruction
; boundary; if it accepts interrupts there (IF set in its frame, no virtual
; CLI or STI shadow of the IF profile), CVSESSION arbitrates the highest held
; line through Jemm's virtual PIC and HDPMI enters the client's handler for
; it through its ordinary locked-stack IRQ path, exactly as a physical IRQ
; arriving right after the instruction.  EBP = the client's IRET32 frame (SS
; relative), FS = client data.
cv_pm_irq_check proc
	; CVSESSION publishes the held lines in the shared video header.
	mov eax,fs:cv_video_shared
	test eax,eax
	jz pm_irq_done
	push es
	push byte ptr _FLATSEL_
	pop es
	mov eax,es:[eax+CV_SHARED_DEVICE_IRQS]
	pop es
	mov fs:cv_pm_irqs,eax
	test eax,eax
	jz pm_irq_done
	cmp dword ptr ss:cvdpmi_jlm_entry,0
	je pm_irq_done
	cmp dword ptr ss:cvdpmi_irq_proc,0
	jne pm_irq_done
	call cv_client_interruptible
	jc pm_irq_done
	pushad
	mov eax,CV_OP_DEV_PM_IRQ
	mov ecx,2
	call cv_bridge_call
	mov fs:cv_pm_irqs,esi
	cmp ebx,16
	jae pm_irq_refused
	; HDPMI's client vectors: IRQ 0-7 at INT 08h-0Fh, IRQ 8-15 at 70h-77h.
	.errnz sizeof R3PROC - 8
	lea eax,[ebx*8 + offset r3vect08]
	cmp ebx,8
	jb pm_irq_vector
	lea eax,[ebx*8 + offset r3vect70 - 8*8]
pm_irq_vector:
	mov ss:cvdpmi_irq_proc,eax
	inc fs:cv_pm_delivered
	popad
	ret
pm_irq_refused:
	popad
pm_irq_done:
	ret
cv_pm_irq_check endp

; CF=0 when the client frame at SS:EBP (IRET32) is ring-3 code that accepts
; an interrupt now: IF set, no virtual CLI or STI shadow of the IF profile.
cv_client_interruptible proc
	test byte ptr ss:[ebp+4],3
	jz not_interruptible
	test byte ptr ss:[ebp+9],2
	jz not_interruptible
	cmp dword ptr fs:cv_if_enabled,0
	je not_interruptible
	cmp dword ptr fs:cv_if_shadow,0
	jne not_interruptible
	cmp dword ptr fs:cvdpmi_cli_stepping,0
	jne not_interruptible
	clc
	ret
not_interruptible:
	stc
	ret
cv_client_interruptible endp

; Tail of HDPMI's returns to the client from a client IRQ handler (rpmstacki)
; and from an emulated VGA memory access (#PF), IRET32 at SS:ESP.  A program
; waiting for a device IRQ may run without any trapped port I/O (Apogee's
; MV_TestPlayback spins on a variable), and while a frame is drawn nearly all
; of the time is spent in the VGA emulation, where the timer lands in ring 0;
; both boundaries deliver held device IRQs (and a tick deferred for one).
cvdpmi_irq_return proc near public
	pushad
	push fs
	push byte ptr _CSALIAS_
	pop fs
	cmp dword ptr ss:cvdpmi_client_active,0
	je irq_return_plain
	lea ebp,[esp+4+32]
	; A timer tick deferred for a device IRQ (cvdpmi_irq_tick) comes first.
	cmp dword ptr fs:cv_if_pending,0
	je irq_return_device
	call cv_client_interruptible
	jc irq_return_plain
	mov dword ptr fs:cv_if_pending,0
	mov dword ptr ss:cvdpmi_irq_proc,offset r3vect08
	jmp irq_return_plain
irq_return_device:
	call cv_pm_irq_check
irq_return_plain:
	pop fs
	popad
	cmp dword ptr ss:cvdpmi_irq_proc,0
	jne irq_return_deliver
	iretd
irq_return_deliver:
	push dword ptr ss:cvdpmi_irq_proc
	mov dword ptr ss:cvdpmi_irq_proc,0
	jmp lpms_call_int
cvdpmi_irq_return endp

; Physical IRQ0 with the client in interruptible ring-3 code and a device IRQ
; held: that device IRQ is entered now, and this tick is acknowledged and
; deferred exactly like a virtual-CLI tick, to be entered when the device
; handler returns (cvdpmi_irq_return).  The handler-return boundary alone is
; too rare: the client does most of its I/O inside IF=0 handlers.  IRET32 at
; [ESP+4].  CF=0: cvdpmi_irq_proc holds the device IRQ to enter.
cvdpmi_irq_tick proc near public
	pushad
	push fs
	push byte ptr _CSALIAS_
	pop fs
	lea ebp,[esp+4+32+4]
	call cv_pm_irq_check
	cmp dword ptr ss:cvdpmi_irq_proc,0
	stc
	je irq_tick_done
	mov dword ptr fs:cv_if_pending,1
	mov al,20h
	out 20h,al
	clc
irq_tick_done:
	pop fs
	popad
	ret
cvdpmi_irq_tick endp

; Called from physical IRQ0 and the first-entry fallback.  It never invokes
; DOS, BIOS, or a source-port callback.
cvdpmi_scheduler_tick proc near public
	pushfd
	pushad
	push ds
	push es
	push fs
	cmp dword ptr ss:cvdpmi_client_active,0
	je tick_done
	push byte ptr _CSALIAS_
	pop fs
	mov al,1
	xchg al,fs:cv_tick_busy
	test al,al
	jnz tick_reentered
	push byte ptr _FLATSEL_
	pop es
	mov edi,fs:cv_descriptor
	call cv_validate
	jc tick_done_busy
	test dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_DPMI_CLIENT
	jz tick_done_busy
	test dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_FATAL
	jnz tick_done_busy
	test dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_CALLBACK
	jnz tick_registered
	call cv_register_callback
	jc tick_register_failed
tick_registered:
	inc dword ptr es:[edi+CVSCHED_DPMI_TICKS]
	inc dword ptr es:[edi+CVSCHED_SERVICES]
	mov eax,es:[edi+CVSCHED_REQUEST_GENERATION]
	mov es:[edi+CVSCHED_SERVICE_GENERATION],eax
	xor eax,eax
	xchg eax,fs:cv_pending_services
	add es:[edi+CVSCHED_SERVICES],eax
	xor eax,eax
	xchg eax,fs:cv_pending_reentries
	add es:[edi+CVSCHED_REENTRIES],eax
	mov eax,fs:cv_last_flags
	mov es:[edi+CVSCHED_LAST_FLAGS],eax
	mov eax,fs:cv_last_eip
	mov es:[edi+CVSCHED_LAST_EIP],eax
	mov eax,fs:cv_last_port
	mov es:[edi+CVSCHED_LAST_PORT],eax
	call cv_guard_ptes
	jmp tick_done_busy
tick_register_failed:
	mov dword ptr es:[edi+CVSCHED_LAST_ERROR],CVERR_REGISTER
	or dword ptr es:[edi+CVSCHED_STATE],CVSCHED_STATE_FATAL
	inc dword ptr es:[edi+CVSCHED_FAILURES]
tick_done_busy:
	mov byte ptr fs:cv_tick_busy,0
	jmp tick_done
tick_reentered:
	inc dword ptr fs:cv_pending_reentries
tick_done:
	pop fs
	pop es
	pop ds
	popad
	popfd
	ret
cvdpmi_scheduler_tick endp

_TEXT32 ends

endif

	end
