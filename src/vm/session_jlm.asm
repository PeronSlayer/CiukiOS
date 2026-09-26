; CiukiOS original-DOS virtualization foundation, not a complete DOS VM.
; Uses the exact pinned Jemm/JLOAD ABI, no DOS/BIOS calls from ring zero.
; Only the owning Jemm CR3 may call it. VGA memory is a raw private aperture;
; planar latches, protected-mode guests, rendering and audio are NOT supplied.
.386p
.model flat, stdcall
.nolist
include jlm.inc
include session_abi.inc
.list

PAGE_MAP equ 0FF800000h
VGA_FIRST_PAGE equ 0A0h
VGA_PAGES equ 32
VGA_BYTES equ 20000h
VGA_PTES equ PAGE_MAP + VGA_FIRST_PAGE*4
FIRST_PORT equ 03B0h
PORT_COUNT equ 30h

.data
public ddb
ddb VxD_Desc_Block <0,0,VM_DEVICE_ID,1,0,0,"CIUKIVM",0,0,v86_dispatch>
owner_cr3 dd 0
shadow dd 0
active dd 0
mapped dd 0
trapped dd 0
hooked dd 0
generation dd 0
io_count dd 0
reject_count dd 0
fatal_status dd 0 ; query packet +52: 1 = unsupported string port I/O
fatal_port dd 0   ; query packet +56: rejected port
fatal_io_type dd 0 ; query packet +60: original Jemm I/O type flags
video_mode dd 3
previous_int10 dd 0
fb_linear dd 0
fb_physical dd 0
fb_bytes dd 0
fb_pages dd 0
saved_ptes dd VGA_PAGES dup (0)
port_bytes db PORT_COUNT dup (0)
seq_regs db 256 dup (0)
gc_regs db 256 dup (0)
crtc_regs db 256 dup (0)
attr_regs db 32 dup (0)
dac db 768 dup (0)
seq_index db 0
gc_index db 0
crtc_index db 0
attr_index db 0
attr_phase db 0
dac_read_index dw 0
dac_write_index dw 0
info_packet dd VM_INFO_MAGIC
 dw VM_ABI_VERSION,VM_INFO_SIZE
 dd VM_CAPABILITIES
 dd 13 dup (0)

.code

check_context proc
 mov eax,cr0
 and eax,80000001h
 cmp eax,80000001h
 jne bad
 mov eax,cr3
 cmp eax,owner_cr3
 jne bad
 xor eax,eax
 ret
bad:
 mov eax,VM_ERROR_CONTEXT
 ret
check_context endp

; Validate a guest copy destination without touching any guest memory.
; ECX=count <=4096, ES:DI from the client frame. EDI=linear destination.
; Only conventional user pages at 10000h..9FFFFh, no 16-bit offset wrap.
guest_destination proc uses esi ebx edx
 test ecx,ecx
 jz bad
 cmp ecx,4096
 ja bad
 movzx edi,word ptr [ebp].Client_Reg_Struc.Client_EDI
 mov eax,edi
 add eax,ecx
 cmp eax,10000h
 ja bad
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_ES
 shl eax,4
 add edi,eax
 cmp edi,10000h
 jb bad
 mov edx,edi
 add edx,ecx
 jc bad
 cmp edx,0A0000h
 ja bad
 dec edx
 shr edx,12
 mov esi,edi
 shr esi,12
pages:
 mov eax,[PAGE_MAP+esi*4]
 and eax,7
 cmp eax,7
 jne bad
 inc esi
 cmp esi,edx
 jbe pages
 xor eax,eax
 ret
bad:
 mov eax,VM_ERROR_ADDRESS
 ret
guest_destination endp

; Trusted-host linear framebuffer mapping. The 80000000h floor is an
; intentionally restricted MMIO profile: it excludes low/native XMS memory.
; A validated VBE PhysBasePtr/extent must be supplied by the caller; this
; service does not identify a PCI BAR or turn shared DOS into a sandbox.
bind_framebuffer proc uses esi edi ebx
 cmp active,0
 jne session_busy
 cmp fb_linear,0
 jne already_bound
 cmp word ptr [ebp].Client_Reg_Struc.Client_ECX,VM_FB_PACKET_SIZE
 jb bad_address
 mov ecx,VM_FB_PACKET_SIZE
 call guest_destination
 test eax,eax
 jnz done
 cmp dword ptr [edi],VM_FB_PACKET_MAGIC
 jne bad_abi
 cmp word ptr [edi+VM_FB_PACKET_VERSION],VM_ABI_VERSION
 jne bad_abi
 cmp word ptr [edi+VM_FB_PACKET_BYTES],VM_FB_PACKET_SIZE
 jne bad_abi
 mov ebx,[edi+VM_FB_PACKET_PHYSICAL]
 cmp ebx,VM_FB_MIN_PHYSICAL
 jb bad_address
 test ebx,0FFFh
 jnz bad_address
 mov esi,[edi+VM_FB_PACKET_LENGTH]
 test esi,esi
 jz bad_address
 cmp esi,VM_FB_MAX_BYTES
 ja bad_address
 mov eax,esi
 add eax,0FFFh
 jc bad_address
 and eax,0FFFFF000h
 mov edx,ebx
 add edx,eax
 jc bad_address
 shr eax,12
 cmp eax,4000h
 ja bad_address
 mov fb_pages,eax
 push 0
 push eax
 push PR_SYSTEM
 @VMMCall _PageReserve
 add esp,12
 cmp eax,-1
 je no_memory
 mov fb_linear,eax
 mov fb_physical,ebx
 mov fb_bytes,esi
 shr eax,12
 shr ebx,12
 push PC_INCR or PC_WRITEABLE or PC_CACHEDIS
 push ebx
 push fb_pages
 push eax
 @VMMCall _PageCommitPhys
 add esp,16
 test eax,eax
 jz map_failed
 mov eax,cr3
 mov cr3,eax
 xor eax,eax
 ret
map_failed:
 call unbind_framebuffer
 test eax,eax
 jnz done
no_memory:
 mov fb_pages,0
 mov eax,VM_ERROR_MEMORY
 ret
session_busy:
 mov eax,VM_ERROR_ACTIVE
 ret
already_bound:
 mov eax,VM_ERROR_FB_BOUND
 ret
bad_address:
 mov eax,VM_ERROR_ADDRESS
 ret
bad_abi:
 mov eax,VM_ERROR_ABI
done:
 ret
bind_framebuffer endp

; END deliberately retains the framebuffer binding; UNBIND is explicit.
; _PageFree recognizes _PageCommitPhys mappings and never frees MMIO pages.
unbind_framebuffer proc
 mov eax,fb_linear
 test eax,eax
 jz not_bound
 push 0
 push eax
 @VMMCall _PageFree
 add esp,8
 test eax,eax
 jz failed
 mov fb_linear,0
 mov fb_physical,0
 mov fb_bytes,0
 mov fb_pages,0
 xor eax,eax
 ret
not_bound:
 mov eax,VM_ERROR_FB_UNBOUND
 ret
failed:
 ; Keep ownership recorded and refuse DLL unload; caller can retry UNBIND.
 mov eax,VM_ERROR_MEMORY
 ret
unbind_framebuffer endp

framebuffer_copy proc uses esi edi edx
 cmp fb_linear,0
 je not_bound
 cmp word ptr [ebp].Client_Reg_Struc.Client_EBX,1
 ja bad_operation
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_ECX
 call guest_destination
 test eax,eax
 jnz done
 mov eax,[ebp].Client_Reg_Struc.Client_EDX
 mov edx,eax
 add edx,ecx
 jc bad_address
 cmp edx,fb_bytes
 ja bad_address
 add eax,fb_linear
 cmp word ptr [ebp].Client_Reg_Struc.Client_EBX,0
 je get_pixels
 mov esi,edi
 mov edi,eax
 jmp copy_pixels
get_pixels:
 mov esi,eax
copy_pixels:
 ; Native protected-mode stores; no CR0 toggles or firmware calls. Max 4 KiB.
 mov edx,ecx
 shr ecx,2
 cld
 rep movsd
 mov ecx,edx
 and ecx,3
 rep movsb
 xor eax,eax
 ret
bad_operation:
 mov eax,VM_ERROR_OPERATION
 ret
not_bound:
 mov eax,VM_ERROR_FB_UNBOUND
 ret
bad_address:
 mov eax,VM_ERROR_ADDRESS
done:
 ret
framebuffer_copy endp

clear_shadow proc uses edi ecx
 mov edi,shadow
 xor eax,eax
 mov ecx,VGA_BYTES/4
 cld
 rep stosd
 cmp video_mode,3
 jne done
 mov edi,shadow
 add edi,18000h
 mov eax,07200720h
 mov ecx,1000
 rep stosd
done:
 ret
clear_shadow endp

; Unwind in reverse order. Only called while owner CR3 is active.
; Restores the original 32 PTE DWORDs verbatim before freeing their aliases.
rollback proc uses esi edi ebx
 cmp mapped,0
 je unmapped
 mov esi,offset saved_ptes
 mov edi,VGA_PTES
 mov ecx,VGA_PAGES
 cld
 rep movsd
 mov eax,cr3
 mov cr3,eax
 mov mapped,0
unmapped:
 cmp hooked,0
 je unhooked
 mov eax,10h
 mov esi,offset video_interrupt
 @VMMCall Unhook_V86_Int_Chain
 jc cleanup_failed
 ; The host unhooks only our exact callback, preserving a later chain owner.
 mov hooked,0
unhooked:
 mov ebx,trapped
 test ebx,ebx
 jz untrapped
ports:
 dec ebx
 lea edx,[ebx+FIRST_PORT]
 @VMMCall Remove_IO_Handler
 jc port_cleanup_failed
 mov trapped,ebx
 test ebx,ebx
 jnz ports
 mov trapped,0
untrapped:
 mov eax,shadow
 test eax,eax
 jz freed
 push 0
 push eax
 @VMMCall _PageFree
 add esp,8
 test eax,eax
 jz cleanup_failed
 mov shadow,0
freed:
 mov active,0
 xor eax,eax
 ret
port_cleanup_failed:
 inc ebx
 mov trapped,ebx
cleanup_failed:
 ; Retain the module and remaining ownership for an explicit END retry.
 mov active,1
 mov eax,VM_ERROR_TRAP
 ret
rollback endp

begin_session proc uses esi edi ebx
 cmp active,0
 jne already
 push 0
 push VGA_PAGES
 push PR_SYSTEM
 @VMMCall _PageReserve
 add esp,12
 cmp eax,-1
 je nomem
 mov shadow,eax
 shr eax,12
 push PC_FIXED or PC_WRITEABLE or PC_USER
 push 0
 push PD_FIXEDZERO
 push VGA_PAGES
 push eax
 @VMMCall _PageCommit
 add esp,20
 test eax,eax
 jz alloc_failed

 ; JLOAD has established PAGE_MAP. Validate all original and source PTEs
 ; before any change; physical pages must reside outside the video aperture.
 mov esi,VGA_PTES
 mov edi,offset saved_ptes
 mov ebx,shadow
 shr ebx,12
 lea ebx,[PAGE_MAP+ebx*4]
 mov ecx,VGA_PAGES
validate_pages:
 mov eax,[esi]
 test al,1
 jz invalid_mapping
 mov [edi],eax
 mov eax,[ebx]
 and eax,7
 cmp eax,7
 jne invalid_mapping
 mov eax,[ebx]
 and eax,0FFFFF000h
 cmp eax,100000h
 jb invalid_mapping
 add esi,4
 add edi,4
 add ebx,4
 loop validate_pages

 mov ebx,FIRST_PORT
install_ports:
 mov edx,ebx
 mov esi,offset port_handler
 @VMMCall Install_IO_Handler
 jc trap_failed
 inc trapped
 inc ebx
 cmp ebx,FIRST_PORT+PORT_COUNT
 jb install_ports
 mov previous_int10,0
 mov eax,10h
 mov esi,offset video_interrupt
 @VMMCall Hook_V86_Int_Chain
 jc trap_failed
 mov hooked,1

 mov video_mode,3
 mov fatal_status,0
 mov fatal_port,0
 mov fatal_io_type,0
 mov edi,offset port_bytes
 mov ecx,(offset info_packet)-(offset port_bytes)
 xor eax,eax
 rep stosb
 call clear_shadow
 mov esi,shadow
 shr esi,12
 lea esi,[PAGE_MAP+esi*4]
 mov edi,VGA_PTES
 mov ecx,VGA_PAGES
map_pages:
 lodsd
 ; Don't copy JLOAD's allocator metadata bits into a second page-table slot.
 and eax,0FFFFF000h
 or eax,7
 stosd
 loop map_pages
 mov eax,cr3
 mov cr3,eax
 mov mapped,1
 mov active,1
 inc generation
 xor eax,eax
 ret
already:
 mov eax,VM_ERROR_ACTIVE
 ret
nomem:
 mov eax,VM_ERROR_MEMORY
 ret
alloc_failed:
 mov ebx,VM_ERROR_MEMORY
 jmp failed
invalid_mapping:
 mov ebx,VM_ERROR_MAPPING
 jmp failed
trap_failed:
 mov ebx,VM_ERROR_TRAP
failed:
 push ebx
 call rollback
 test eax,eax
 jnz failed_cleanup
 pop eax
 ret
failed_cleanup:
 add esp,4
 ret
begin_session endp

v86_dispatch proc
 @VMMCall Simulate_Far_Ret
 pushad
 cld
 call check_context
 test eax,eax
 jnz error
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EAX
 cmp eax,VM_OP_QUERY
 je query
 cmp eax,VM_OP_BEGIN
 je start_session
 cmp eax,VM_OP_END
 je end_session
 cmp eax,VM_OP_READBACK
 je readback
 cmp eax,VM_OP_BIND_FB
 je bind_fb
 cmp eax,VM_OP_UNBIND_FB
 je unbind_fb
 cmp eax,VM_OP_FB_COPY
 je copy_fb
 mov eax,VM_ERROR_OPERATION
 jmp error
bind_fb:
 call bind_framebuffer
 jmp checked_result
unbind_fb:
 call unbind_framebuffer
 jmp checked_result
copy_fb:
 call framebuffer_copy
checked_result:
 test eax,eax
 jnz error
 jmp success
start_session:
 call begin_session
 test eax,eax
 jnz error
 jmp success
end_session:
 cmp active,0
 je inactive
 call rollback
 test eax,eax
 jnz error
 xor eax,eax
 jmp success
readback:
 cmp active,0
 je inactive
 cmp fatal_status,0
 je readback_live
 mov eax,VM_ERROR_OPERATION
 jmp error
readback_live:
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_ECX
 call guest_destination
 test eax,eax
 jnz error
 mov esi,[ebp].Client_Reg_Struc.Client_EDX
 mov eax,esi
 add eax,ecx
 jc address_bad
 cmp eax,VGA_BYTES
 ja address_bad
 add esi,shadow
 rep movsb
 xor eax,eax
 jmp success
query:
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_ECX
 test ecx,ecx
 jz query_regs
 cmp ecx,VM_INFO_SIZE
 jb address_bad
 mov ecx,VM_INFO_SIZE
 call guest_destination
 test eax,eax
 jnz error
 mov eax,active
 mov [info_packet+VM_INFO_ACTIVE],eax
 mov eax,video_mode
 mov [info_packet+VM_INFO_MODE],eax
 mov eax,generation
 mov [info_packet+VM_INFO_GENERATION],eax
 mov eax,io_count
 mov [info_packet+VM_INFO_IO_COUNT],eax
 mov eax,reject_count
 mov [info_packet+VM_INFO_REJECT_COUNT],eax
 mov eax,owner_cr3
 mov [info_packet+VM_INFO_OWNER_CR3],eax
 mov dword ptr [info_packet+VM_INFO_SHADOW_BYTES],VGA_BYTES
 xor eax,eax
 cmp fb_linear,0
 setne al
 mov [info_packet+VM_INFO_FB_BOUND],eax
 mov eax,fb_bytes
 mov [info_packet+VM_INFO_FB_BYTES],eax
 mov eax,fb_physical
 mov [info_packet+VM_INFO_FB_PHYSICAL],eax
 mov eax,fatal_status
 mov [info_packet+52],eax
 mov eax,fatal_port
 mov [info_packet+56],eax
 mov eax,fatal_io_type
 mov [info_packet+60],eax
 mov esi,offset info_packet
 rep movsb
query_regs:
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,VM_CAPABILITIES
 mov eax,active
 mov [ebp].Client_Reg_Struc.Client_EDX,eax
 mov eax,video_mode
 mov [ebp].Client_Reg_Struc.Client_ESI,eax
 mov eax,VM_ABI_VERSION
 jmp success
inactive:
 mov eax,VM_ERROR_INACTIVE
 jmp error
address_bad:
 mov eax,VM_ERROR_ADDRESS
error:
 inc reject_count
 or [ebp].Client_Reg_Struc.Client_EFlags,1
 mov word ptr [ebp].Client_Reg_Struc.Client_EAX,ax
 popad
 ret
success:
 and [ebp].Client_Reg_Struc.Client_EFlags,not 1
 mov word ptr [ebp].Client_Reg_Struc.Client_EAX,ax
 popad
 ret
v86_dispatch endp

; Hooks require a pointer to their saved-chain variable at entry-4.
align 4
 dd offset previous_int10
video_interrupt proc
 cmp active,0
 je chain
 pushad
 call check_context
 test eax,eax
 jnz video_reject
 mov ax,word ptr [ebp].Client_Reg_Struc.Client_EAX
 cmp ah,0Fh
 je get_mode
 test ah,ah
 jnz video_reject
 mov dl,al
 and al,7Fh
 cmp al,3
 je set_mode
 cmp al,13h
 jne video_reject
set_mode:
 movzx eax,al
 mov video_mode,eax
 test dl,80h
 jnz video_done
 call clear_shadow
 jmp video_done
get_mode:
 mov eax,video_mode
 mov ah,80
 cmp al,13h
 jne mode_result
 mov ah,40
mode_result:
 mov word ptr [ebp].Client_Reg_Struc.Client_EAX,ax
 mov byte ptr [ebp].Client_Reg_Struc.Client_EBX+1,0
video_done:
 and [ebp].Client_Reg_Struc.Client_EFlags,not 1
 popad
 clc
 ret
video_reject:
 inc reject_count
 or [ebp].Client_Reg_Struc.Client_EFlags,1
 mov word ptr [ebp].Client_Reg_Struc.Client_EAX,014Fh
 popad
 clc ; handled: never let an unsupported guest call change physical video
 ret
chain:
 stc
 ret
video_interrupt endp

; Primitive byte register isolation, not a complete VGA model. In particular
; status is supplied as 0; no fake clock progresses when a guest polls a port.
; Wide accesses are decomposed into adjacent byte registers, never physical IO.
port_handler proc uses esi edi ebx
 inc io_count
 test ecx,STRING_IO
 jnz reject_string
 cmp fatal_status,0
 jne reject_after_fatal
 mov ebx,eax
 mov edi,ecx
 mov esi,1
 test ecx,DWORD_IO
 jz not_dword
 mov esi,4
 jmp width_ready
not_dword:
 test ecx,WORD_IO
 jz width_ready
 mov esi,2
width_ready:
 push eax ; IN AL/AX must retain the unaffected bits of the guest EAX
 push ebp
 xor ebp,ebp ; input accumulator
 xor ecx,ecx ; byte shift
next_byte:
 push ecx
 push edx
 mov eax,ebx
 test edi,OUTPUT
 jz handle_input_byte
 call write_port
 jmp byte_done
handle_input_byte:
 call read_port
 pop edx
 pop ecx
 movzx eax,al
 shl eax,cl
 or ebp,eax
 jmp advance
byte_done:
 pop edx
 pop ecx
advance:
 inc edx
 shr ebx,8
 add ecx,8
 dec esi
 jnz next_byte
 mov eax,ebp
 pop ebp
 pop edx ; original guest EAX; EDX is volatile in the Jemm callback ABI
 test edi,OUTPUT
 jnz preserve_output
 test edi,DWORD_IO
 jnz port_done
 test edi,WORD_IO
 jnz merge_word
 and edx,0FFFFFF00h
 or eax,edx
 ret
merge_word:
 and edx,0FFFF0000h
 or eax,edx
port_done:
 ret
preserve_output:
 mov eax,edx
 ret
reject_string:
 ; Jemm's Crash_Cur_VM faults the entire shared V86 machine; it cannot
 ; terminate only this DOS child. Record a fatal session diagnostic instead.
 ; Do not invent string/REP transfer semantics, touch memory, or pass through
 ; to physical IO. READBACK refuses this session, QUERY exposes the reason,
 ; and END still performs normal ownership cleanup. No support is advertised.
 cmp fatal_status,0
 jne reject_after_fatal
 mov fatal_status,1
 movzx ebx,dx
 mov fatal_port,ebx
 mov fatal_io_type,ecx
reject_after_fatal:
 inc reject_count
 ret
port_handler endp

write_port proc uses ebx
 cmp dx,FIRST_PORT
 jb done
 cmp dx,FIRST_PORT+PORT_COUNT
 jae done
 movzx ebx,dx
 mov [port_bytes+ebx-FIRST_PORT],al
 cmp dx,3C4h
 je seq_idx
 cmp dx,3C5h
 je seq_data
 cmp dx,3CEh
 je gc_idx
 cmp dx,3CFh
 je gc_data
 cmp dx,3D4h
 je crtc_idx
 cmp dx,3B4h
 je crtc_idx
 cmp dx,3D5h
 je crtc_data
 cmp dx,3B5h
 je crtc_data
 cmp dx,3C0h
 je attr_data
 cmp dx,3C7h
 je dac_read_idx
 cmp dx,3C8h
 je dac_write_idx
 cmp dx,3C9h
 je dac_data
done:
 ret
seq_idx: mov seq_index,al
 ret
seq_data: movzx ebx,seq_index
 mov seq_regs[ebx],al
 ret
gc_idx: mov gc_index,al
 ret
gc_data: movzx ebx,gc_index
 mov gc_regs[ebx],al
 ret
crtc_idx: mov crtc_index,al
 ret
crtc_data: movzx ebx,crtc_index
 mov crtc_regs[ebx],al
 ret
attr_data:
 cmp attr_phase,0
 jne attr_value
 and al,1Fh
 mov attr_index,al
 mov attr_phase,1
 ret
attr_value:
 movzx ebx,attr_index
 mov attr_regs[ebx],al
 mov attr_phase,0
 ret
dac_read_idx:
 movzx eax,al
 lea eax,[eax+eax*2]
 mov dac_read_index,ax
 ret
dac_write_idx:
 movzx eax,al
 lea eax,[eax+eax*2]
 mov dac_write_index,ax
 ret
dac_data:
 movzx ebx,dac_write_index
 and al,3Fh
 mov dac[ebx],al
 inc ebx
 cmp ebx,768
 jb dac_write_next
 xor ebx,ebx
dac_write_next:
 mov dac_write_index,bx
 ret
write_port endp

read_port proc uses ebx
 mov al,0FFh
 cmp dx,FIRST_PORT
 jb done
 cmp dx,FIRST_PORT+PORT_COUNT
 jae done
 movzx ebx,dx
 mov al,[port_bytes+ebx-FIRST_PORT]
 cmp dx,3BAh
 je status
 cmp dx,3DAh
 je status
 cmp dx,3C5h
 je seq_data
 cmp dx,3CFh
 je gc_data
 cmp dx,3D5h
 je crtc_data
 cmp dx,3B5h
 je crtc_data
 cmp dx,3C1h
 je attr_data
 cmp dx,3C9h
 je dac_data
done:
 ret
status:
 mov attr_phase,0
 xor eax,eax
 ret
seq_data: movzx ebx,seq_index
 mov al,seq_regs[ebx]
 ret
gc_data: movzx ebx,gc_index
 mov al,gc_regs[ebx]
 ret
crtc_data: movzx ebx,crtc_index
 mov al,crtc_regs[ebx]
 ret
attr_data: movzx ebx,attr_index
 mov al,attr_regs[ebx]
 ret
dac_data:
 movzx ebx,dac_read_index
 mov al,dac[ebx]
 inc ebx
 cmp ebx,768
 jb dac_read_next
 xor ebx,ebx
dac_read_next:
 mov dac_read_index,bx
 ret
read_port endp

DllMain proc stdcall public hModule:dword, dwReason:dword, dwRes:dword
 cmp dwReason,1
 jne detach
 mov eax,cr0
 and eax,80000001h
 cmp eax,80000001h
 jne refuse
 mov eax,cr3
 mov owner_cr3,eax
 mov eax,1
 ret
detach:
 cmp dwReason,0
 jne allow
 cmp active,0
 jne refuse
 cmp mapped,0
 jne refuse
 cmp hooked,0
 jne refuse
 cmp trapped,0
 jne refuse
 cmp fb_linear,0
 jne refuse
allow:
 mov eax,1
 ret
refuse:
 xor eax,eax
 ret
DllMain endp
end DllMain
