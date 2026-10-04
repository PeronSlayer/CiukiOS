; CiukiOS monitored-VGA glue for CVSESSION (video/presenter track).
; Included by session_jlm.asm after the core data. Assembled against the
; pinned Jemm/JLOAD ABI. C code in session_video.c runs on the private stack
; below with flat DS=ES=SS, DF=0 and interrupts disabled; it never calls
; DOS, BIOS or firmware and never toggles CR0. Retained raster allocation
; uses only the two bounded owner callbacks below, on this private stack.
;
; Memory trapping: BEGIN maps the 32 aperture PTEs to the private shadow with
; the USER bit clear. Every V86 access (and every access by a fresh HDPMI
; host that copied page table 0 through VCPI) therefore faults. The V86 #PF
; hook below executes exactly the faulting instruction against the model.

include session_video_abi.inc

VIDEO_BLOCK_PAGES equ 67                 ; CVVID_BLOCK_BYTES / 4096
VIDEO_STACK_BYTES equ 16384

externdef c cvvid_attach:near
externdef c cvvid_select:near
externdef c cvvid_instance_bytes:near
externdef c cvvid_detach:near
externdef c cvvid_v86_fault:near
externdef c cvvid_port_read:near
externdef c cvvid_port_write:near
externdef c cvvid_int10:near
externdef c cvvid_operation:near
externdef c cvvid_readback:near
externdef c cvvid_share_count:near
externdef c cvvid_timer:near
externdef c cvvid_port_string:near
externdef c cvvid_host_offset:near

.data
video_block dd 0
video_fault_hooked dd 0
video_timer_hooked dd 0
video_previous_int08 dd 0
video_fb_view dd 0,0                     ; cvvid_fb {linear, bytes}
video_saved_esp dd 0
video_busy db 0
align 4
video_stack db VIDEO_STACK_BYTES dup (0)
video_stack_end label byte

.code

; ---- Services for the C monitor (cdecl) ----

; Retained scanout RAM owned by one video instance. Actual window/source
; dimensions determine the allocation; no fixed multi-megabyte VM reserve.
cvvid_surface_alloc proc c public uses ebx esi edi bytes:dword
 mov ecx,bytes
 test ecx,ecx
 jz bad
 cmp ecx,00900000h                      ; VGA snapshot + 2048*1024*4
 ja bad
 call vmm_alloc_block
 ret
bad:
 xor eax,eax
 ret
cvvid_surface_alloc endp

cvvid_surface_free proc c public uses ebx esi edi storage:dword
 push 0
 push storage
 @VMMCall _PageFree
 add esp,8
 ret
cvvid_surface_free endp

cvvid_rdtsc proc c public low_ptr:dword, high_ptr:dword
 push edx
 push ecx
 db 0Fh,31h ; RDTSC (Pentium+); source is .386p
 mov ecx,low_ptr
 mov [ecx],eax
 mov ecx,high_ptr
 mov [ecx],edx
 pop ecx
 pop edx
 ret
cvvid_rdtsc endp

; Host-mode aperture PTEs changed (session_video.c host_map/host_unmap).
cvvid_flush_tlb proc c public
 push eax
 mov eax,cr3
 mov cr3,eax
 pop eax
 ret
cvvid_flush_tlb endp

; Physical port access, used only while the HOST (never the guest) draws.
cvvid_io_in proc c public port:dword, iosize:dword
 push edx
 mov edx,port
 xor eax,eax
 in al,dx
 pop edx
 ret
cvvid_io_in endp

cvvid_io_out proc c public port:dword, value:dword, iosize:dword
 push edx
 mov edx,port
 mov eax,value
 out dx,al
 pop edx
 ret
cvvid_io_out endp

; ---- Private-stack trampoline ----
; EAX = C function, ECX = argument count, arguments already in video_args.
; Returns the C result in EAX. Refuses reentry (returns -1).
.data
video_args dd 4 dup (0)
.code
video_call proc uses ebx esi edi
 cmp video_busy,0
 jne reentered
 mov video_busy,1
 push ds
 push es
 mov ebx,ss
 mov ds,ebx
 mov es,ebx
 mov video_saved_esp,esp
 mov esp,offset video_stack_end
 cld
 mov edx,ecx
next_arg:
 test edx,edx
 jz args_done
 dec edx
 push [video_args+edx*4]
 jmp next_arg
args_done:
 call eax
 mov esp,video_saved_esp
 pop es
 pop ds
 mov video_busy,0
 ret
reentered:
 or eax,-1
 ret
video_call endp

video_refresh_fb proc
 mov eax,fb_linear
 mov video_fb_view,eax
 mov eax,fb_bytes
 mov [video_fb_view+4],eax
 ret
video_refresh_fb endp

; ---- Lifecycle (called by begin_session / rollback / end_session) ----

; Allocates the shared block and hooks V86 #PF. EAX=0 or VM_ERROR_*.
video_begin proc uses esi edi ebx
 cmp video_block,0
 jne busy
 push 0
 push VIDEO_BLOCK_PAGES
 push PR_SYSTEM
 @VMMCall _PageReserve
 add esp,12
 cmp eax,-1
 je nomem
 mov video_block,eax
 shr eax,12
 push PC_FIXED or PC_WRITEABLE
 push 0
 push PD_FIXEDZERO
 push VIDEO_BLOCK_PAGES
 push eax
 @VMMCall _PageCommit
 add esp,20
 test eax,eax
 jz commit_failed
 mov eax,video_block
 mov video_args,eax
 mov eax,generation
 inc eax
 mov [video_args+4],eax
 mov eax,offset cvvid_attach
 mov ecx,2
 call video_call
 test eax,eax
 jnz attach_failed
 cmp video_fault_hooked,0               ; an earlier session's hooks serve
 jne hooks_ready
 mov eax,0Eh
 mov esi,offset video_fault_hook
 @VMMCall Hook_V86_Fault
 jc hook_failed
 mov video_fault_hooked,1
 mov video_previous_int08,0
 mov eax,8
 mov esi,offset video_timer_hook
 @VMMCall Hook_V86_Int_Chain
 jc timer_failed
 mov video_timer_hooked,1
hooks_ready:
 xor eax,eax
 ret
timer_failed:
 mov eax,0Eh
 mov esi,offset video_fault_hook
 @VMMCall Unhook_V86_Fault
 mov video_fault_hooked,0
hook_failed:
 mov eax,offset cvvid_detach
 xor ecx,ecx
 call video_call
attach_failed:
commit_failed:
 push 0
 push video_block
 @VMMCall _PageFree
 add esp,8
 mov video_block,0
nomem:
 mov eax,VM_ERROR_MEMORY
 ret
busy:
 mov eax,VM_ERROR_ACTIVE
 ret
video_begin endp

; END is refused while a protected-mode adapter still maps the model.
video_can_end proc
 cmp video_block,0
 je allowed
 mov eax,offset cvvid_share_count
 xor ecx,ecx
 call video_call
 test eax,eax
 jz allowed
 mov eax,VM_ERROR_VIDEO_SHARED
 ret
allowed:
 xor eax,eax
 ret
video_can_end endp

; Called after the core restored the aperture PTEs. Restores guest-visible
; BDA/IVT video state, removes the exact #PF hook and frees the block.
video_end proc uses esi edi ebx
 cmp video_block,0
 je done
 mov eax,offset cvvid_detach
 xor ecx,ecx
 call video_call
 test eax,eax
 jnz retained_failed
 cmp sessions,0                         ; another VM's session keeps them
 jne unhooked
 cmp video_timer_hooked,0
 je timer_unhooked
 mov eax,8
 mov esi,offset video_timer_hook
 @VMMCall Unhook_V86_Int_Chain
 jc failed
 mov video_timer_hooked,0
timer_unhooked:
 cmp video_fault_hooked,0
 je unhooked
 mov eax,0Eh
 mov esi,offset video_fault_hook
 @VMMCall Unhook_V86_Fault
 jc failed
 mov video_fault_hooked,0
unhooked:
 push 0
 push video_block
 @VMMCall _PageFree
 add esp,8
 test eax,eax
 jz failed
 mov video_block,0
done:
 xor eax,eax
 ret
failed:
 mov eax,VM_ERROR_TRAP
 ret
retained_failed:
 ; The C instance still owns its raster. Do not release its shared block.
 ret
video_end endp

video_owned proc
 mov eax,video_block
 or eax,video_fault_hooked
 or eax,video_timer_hooked
 ret
video_owned endp

; V86 INT 08h hook (IRQ0 reflection). Bounded ARMed presentation, then CF=1
; so later hooks and the guest's own IVT handler still receive the tick.
align 4
 dd offset video_previous_int08
video_timer_hook proc
 cmp active,0                           ; the running VM's session: its
 je timer_other_vm                      ; packet is in its memory
 pushad
 call video_refresh_fb
 mov video_args,offset video_fb_view
 mov eax,offset cvvid_timer
 mov ecx,1
 call video_call
 popad
timer_other_vm:
 stc
 ret
video_timer_hook endp

; ---- Jemm entry points ----

; V86 #PF hook (Hook_V86_Fault). EBP = client frame. Either RET (handled; all
; GPRs except EBP/ESP may change) or chain with every register preserved.
align 4
 dd offset video_previous_fault
video_fault_hook proc
 cmp active,0                           ; the running VM's session
 je video_fault_chain
 pushad
 mov eax,cr2
 cmp eax,0A0000h
 jb chain
 cmp eax,0C0000h
 jae chain
 mov [video_args+4],eax
 mov video_args,ebp
 call video_refresh_fb
 mov [video_args+8],offset video_fb_view
 mov eax,offset cvvid_v86_fault
 mov ecx,3
 call video_call
 test eax,eax
 jnz chain
 popad
 ret
chain:
 popad
video_fault_chain:
 jmp [video_previous_fault]
video_fault_hook endp
.data
video_previous_fault dd 0
.code

; Byte port access from the core port handler. DX = port, AL = value.
video_port_write proc uses ecx edx
 movzx edx,dx
 mov video_args,edx
 movzx eax,al
 mov [video_args+4],eax
 mov eax,offset cvvid_port_write
 mov ecx,2
 call video_call
 ret
video_port_write endp

; String port I/O from the core port handler. EDX = port, ECX = Jemm I/O
; type (segment in the high word), EBP = client. EAX = 0 when handled.
video_port_string proc uses ecx edx
 movzx edx,dx
 mov video_args,edx
 mov [video_args+4],ecx
 mov [video_args+8],ebp
 mov eax,offset cvvid_port_string
 mov ecx,3
 call video_call
 ret
video_port_string endp

video_port_read proc uses ecx edx
 movzx edx,dx
 mov video_args,edx
 mov eax,offset cvvid_port_read
 mov ecx,1
 call video_call
 ret
video_port_read endp

; INT 10h from the V86 hook. EBP = client frame.
video_int10 proc uses ecx
 mov video_args,ebp
 mov eax,offset cvvid_int10
 mov ecx,1
 call video_call
 ret
video_int10 endp

; VM_OP_VIDEO_* from v86_dispatch. EAX = operation, EBP = client frame.
video_dispatch proc uses ecx
 mov video_args,eax
 mov [video_args+4],ebp
 call video_refresh_fb
 mov [video_args+8],offset video_fb_view
 mov eax,offset cvvid_operation
 mov ecx,3
 call video_call
 ret
video_dispatch endp

; Legacy READBACK: ESI = aperture offset, ECX = count, EDI = validated buffer.
video_readback proc uses ecx
 mov video_args,esi
 mov [video_args+4],ecx
 mov [video_args+8],edi
 mov eax,offset cvvid_readback
 mov ecx,3
 call video_call
 ret
video_readback endp
