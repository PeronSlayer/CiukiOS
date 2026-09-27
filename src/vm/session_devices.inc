; CiukiOS guest-device glue for CVSESSION (keyboard, mouse, SB16, OPL).
; Included by session_jlm.asm. The C binding (session_devices.c) runs on the
; private stack below with flat DS=ES=SS, DF=0 and IF=0. Physical 8042 bytes
; arrive through the negotiated Jemm profile's IRQ1/IRQ12 filter; the
; profile's return-to-V86 poll advances the devices, streams AC'97 PCM and
; raises virtual IRQs into the profile's virtual PIC, which it then injects.

include session_devices_abi.inc

DEV_STACK_BYTES equ 16384

externdef c cvdev_begin:near
externdef c cvdev_end:near
externdef c cvdev_focus:near
externdef c cvdev_key:near
externdef c cvdev_port_read:near
externdef c cvdev_port_write:near
externdef c cvdev_irq_filter:near
externdef c cvdev_poll:near
externdef c cvdev_report_state:near
externdef c cvdev_trap_ports:near
externdef c cvdev_note_bridge:near
externdef c cvdev_port_claimed:near

.data
dev_block dd 0
dev_pages dd CVDEV_AUDIO_PAGES*2 dup (0)       ; cvdev_pages {linear, physical}
dev_ports dd 0                                  ; C trap list (uint16, FFFFh end)
dev_trapped dd 0
dev_hooked dd 0
dev_active dd 0
dev_granted dd 0
dev_audio dd 0
dev_saved_esp dd 0
dev_args dd 8 dup (0)
dev_busy db 0
align 4
dev_report db CVDEV_REPORT_BYTES dup (0)
dev_stack db DEV_STACK_BYTES dup (0)
dev_stack_end label byte

.code

; ---- Services for the C binding (cdecl) ----

cvdev_in proc c public port:dword, iosize:dword
 push edx
 mov edx,port
 xor eax,eax
 cmp iosize,4
 je in_dword
 cmp iosize,2
 je in_word
 in al,dx
 jmp in_done
in_word:
 in ax,dx
 jmp in_done
in_dword:
 in eax,dx
in_done:
 pop edx
 ret
cvdev_in endp

cvdev_out proc c public port:dword, value:dword, iosize:dword
 push edx
 mov edx,port
 mov eax,value
 cmp iosize,4
 je out_dword
 cmp iosize,2
 je out_word
 out dx,al
 jmp out_done
out_word:
 out dx,ax
 jmp out_done
out_dword:
 out dx,eax
out_done:
 pop edx
 ret
cvdev_out endp

; Raise a virtual IRQ in Jemm's profile PIC. Returns 1 on success.
cvdev_raise_irq proc c public irq:dword
 push ecx
 push edx
 mov ecx,irq
 mov eax,3
 @VMMCall Host_Scheduler_Profile
 pop edx
 pop ecx
 jc raise_failed
 mov eax,1
 ret
raise_failed:
 xor eax,eax
 ret
cvdev_raise_irq endp

; The guest's x87 state is not ours: DBOPL table setup runs between these.
cvdev_fpu_save proc c public area:dword
 mov eax,area
 db 0DDh,30h                                    ; FNSAVE [EAX]
 ret
cvdev_fpu_save endp

cvdev_fpu_restore proc c public area:dword
 mov eax,area
 db 0DDh,20h                                    ; FRSTOR [EAX]
 ret
cvdev_fpu_restore endp

; ---- Private-stack trampoline (EAX function, ECX argument count) ----

dev_call proc uses ebx esi edi
 cmp dev_busy,0
 jne reentered
 mov dev_busy,1
 push ds
 push es
 mov ebx,ss
 mov ds,ebx
 mov es,ebx
 mov dev_saved_esp,esp
 mov esp,offset dev_stack_end
 cld
 mov edx,ecx
next_arg:
 test edx,edx
 jz args_done
 dec edx
 push [dev_args+edx*4]
 jmp next_arg
args_done:
 call eax
 mov esp,dev_saved_esp
 pop es
 pop ds
 mov dev_busy,0
 ret
reentered:
 or eax,-1
 ret
dev_call endp

; ---- Jemm profile callbacks ----

; EAX = physical IRQ (1 or 12). CF=1 when the byte was consumed.
dev_filter proc
 cmp dev_active,0
 je pass
 cmp dev_busy,0
 jne pass
 mov dev_args,eax
 mov eax,offset cvdev_irq_filter
 mov ecx,1
 call dev_call
 cmp eax,1
 jne pass
 stc
 ret
pass:
 clc
 ret
dev_filter endp

; Called by CvLeave before virtual IRQ injection; preserves every register.
dev_poll proc
 pushad
 cmp dev_active,0
 je done
 cmp dev_busy,0
 jne done
 mov eax,offset cvdev_poll
 xor ecx,ecx
 call dev_call
done:
 popad
 ret
dev_poll endp

; ---- V86 port trap (Install_IO_Handler ABI) ----
; EDX port, ECX Jemm I/O flags, EAX output value. Returns EAX.
dev_port_handler proc uses esi edi ebx
 test ecx,STRING_IO
 jnz reject_string
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
 mov ebx,eax
 movzx edx,dx
 test ecx,OUTPUT
 jz port_input
 mov dev_args,edx
 mov [dev_args+4],esi
 mov [dev_args+8],ebx
 mov eax,offset cvdev_port_write
 mov ecx,3
 call dev_call
 mov eax,ebx
 ret
port_input:
 mov dev_args,edx
 mov [dev_args+4],esi
 mov eax,offset cvdev_port_read
 mov ecx,2
 call dev_call
 cmp esi,4
 je input_done
 cmp esi,2
 je input_word
 and ebx,0FFFFFF00h
 and eax,0FFh
 or eax,ebx
 ret
input_word:
 and ebx,0FFFF0000h
 and eax,0FFFFh
 or eax,ebx
input_done:
 ret
reject_string:
 ; REP INS/OUTS to a sound or keyboard port is not part of this device
 ; profile: counted by the model as unsupported, never passed through.
 or eax,-1
 ret
dev_port_handler endp

; ---- Lifecycle ----

; Free the audio block (if any). Returns EAX=0 or VM_ERROR_TRAP.
dev_free_pages proc
 mov eax,dev_block
 test eax,eax
 jz freed
 push 0
 push eax
 @VMMCall _PageFree
 add esp,8
 test eax,eax
 jz free_failed
 mov dev_block,0
freed:
 xor eax,eax
 ret
free_failed:
 mov eax,VM_ERROR_TRAP
 ret
dev_free_pages endp

; Remove hooks, traps and the model, in reverse order of dev_begin.
; Returns EAX=0 or VM_ERROR_TRAP (ownership retained for a retry).
dev_end proc uses esi edi ebx
 cmp dev_active,0
 je done_ok
 ; Traps first: from here on the host BIOS reads the real 8042 again, so a
 ; physical byte can never be left unread behind a model-served IN.
 mov esi,dev_ports
remove_next:
 cmp dev_trapped,0
 je untrapped
 dec dev_trapped
 mov eax,dev_trapped
 movzx edx,word ptr [esi+eax*2]
 @VMMCall Remove_IO_Handler
 jnc remove_next
 inc dev_trapped
 mov eax,VM_ERROR_TRAP
 ret
untrapped:
 cmp dev_hooked,0
 je unhooked
 xor ecx,ecx
 mov eax,6
 @VMMCall Host_Scheduler_Profile
 xor ecx,ecx
 mov eax,4
 @VMMCall Host_Scheduler_Profile
 mov dev_hooked,0
unhooked:
 mov eax,offset cvdev_end
 xor ecx,ecx
 call dev_call
 mov dev_active,0
 ; A byte the filter's IRQ already claimed but nobody read would keep the
 ; controller's output buffer full and silence IRQ1/IRQ12 for good.
 in al,64h
 test al,1
 jz controller_clear
 mov ecx,1
 test al,20h
 jz raise_pending
 mov ecx,12
raise_pending:
 mov eax,3
 @VMMCall Host_Scheduler_Profile
controller_clear:
 call dev_free_pages
 ret
done_ok:
 xor eax,eax
 ret
dev_end endp

; Client EBX caps, ECX flags, EDX TSC kHz. EAX=0 or VM_ERROR_*.
dev_begin proc uses esi edi ebx
 cmp active,0
 je not_session
 cmp dev_active,0
 jne already
 xor eax,eax
 @VMMCall Host_Scheduler_Profile
 test eax,VM_IF_ACTIVE
 jz no_profile
 test dword ptr [ebp].Client_Reg_Struc.Client_ECX,CVDEV_FLAG_AUDIO_OUT
 jz pages_ready
 push 0
 push CVDEV_AUDIO_PAGES
 push PR_SYSTEM
 @VMMCall _PageReserve
 add esp,12
 cmp eax,-1
 je nomem
 mov dev_block,eax
 shr eax,12
 push PC_FIXED or PC_WRITEABLE
 push 0
 push PD_FIXEDZERO
 push CVDEV_AUDIO_PAGES
 push eax
 @VMMCall _PageCommit
 add esp,20
 test eax,eax
 jz commit_failed
 mov esi,dev_block
 shr esi,12
 lea esi,[PAGE_MAP+esi*4]
 mov edi,offset dev_pages
 mov ebx,dev_block
 xor ecx,ecx
page_entry:
 mov [edi+ecx*4],ebx
 mov eax,[esi+ecx*4]
 and eax,0FFFFF000h
 mov [edi+CVDEV_AUDIO_PAGES*4+ecx*4],eax
 add ebx,4096
 inc ecx
 cmp ecx,CVDEV_AUDIO_PAGES
 jb page_entry
pages_ready:
 mov eax,generation
 mov dev_args,eax
 mov eax,[ebp].Client_Reg_Struc.Client_EBX
 mov [dev_args+4],eax
 mov eax,[ebp].Client_Reg_Struc.Client_ECX
 mov [dev_args+8],eax
 mov eax,[ebp].Client_Reg_Struc.Client_EDX
 mov [dev_args+12],eax
 xor eax,eax
 cmp dev_block,0
 je pass_pages
 mov eax,offset dev_pages
pass_pages:
 mov [dev_args+16],eax
 mov [dev_args+20],offset dev_granted
 mov [dev_args+24],offset dev_audio
 mov eax,offset cvdev_begin
 mov ecx,7
 call dev_call
 test eax,eax
 jnz model_failed
 mov dev_active,1
 mov eax,offset cvdev_trap_ports
 xor ecx,ecx
 call dev_call
 mov dev_ports,eax
 mov esi,eax
 mov dev_trapped,0
install_next:
 mov eax,dev_trapped
 movzx edx,word ptr [esi+eax*2]
 cmp edx,0FFFFh
 je installed
 push esi
 mov esi,offset dev_port_handler
 @VMMCall Install_IO_Handler
 pop esi
 jc trap_failed
 inc dev_trapped
 jmp install_next
installed:
 mov ecx,offset dev_filter
 mov eax,4
 @VMMCall Host_Scheduler_Profile
 mov ecx,offset dev_poll
 mov eax,6
 @VMMCall Host_Scheduler_Profile
 mov dev_hooked,1
 mov eax,dev_granted
 mov [ebp].Client_Reg_Struc.Client_EBX,eax
 mov eax,dev_audio
 mov [ebp].Client_Reg_Struc.Client_ECX,eax
 xor eax,eax
 ret
trap_failed:
 call dev_end
 mov eax,VM_ERROR_TRAP
 ret
model_failed:
 call dev_free_pages
 mov eax,VM_ERROR_DEV_MODEL
 ret
commit_failed:
 call dev_free_pages
nomem:
 mov eax,VM_ERROR_MEMORY
 ret
no_profile:
 mov eax,VM_ERROR_DEV_PROFILE
 ret
already:
 mov eax,VM_ERROR_DEV_ACTIVE
 ret
not_session:
 mov eax,VM_ERROR_INACTIVE
 ret
dev_begin endp

; ---- V86 operations 30h-34h (EBP = client frame). EAX=0 or VM_ERROR_* ----
dev_dispatch proc uses esi edi ebx
 cmp eax,VM_OP_DEV_BEGIN
 je op_begin
 cmp dev_active,0
 je inactive
 cmp eax,VM_OP_DEV_END
 je op_end
 cmp eax,VM_OP_DEV_FOCUS
 je op_focus
 cmp eax,VM_OP_DEV_STATE
 je op_state
 cmp eax,VM_OP_DEV_IO
 je op_io
 cmp eax,VM_OP_DEV_KEY
 je op_key
 mov eax,VM_ERROR_OPERATION
 ret
op_key:
 movzx eax,byte ptr [ebp].Client_Reg_Struc.Client_EBX
 mov dev_args,eax
 movzx eax,byte ptr [ebp].Client_Reg_Struc.Client_ECX
 mov [dev_args+4],eax
 mov eax,offset cvdev_key
 mov ecx,2
 call dev_call
 test eax,eax
 jz key_done
 mov eax,VM_ERROR_OPERATION
key_done:
 ret
op_begin:
 call dev_begin
 ret
op_end:
 call dev_end
 ret
op_focus:
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 mov dev_args,eax
 mov eax,offset cvdev_focus
 mov ecx,1
 call dev_call
 xor eax,eax
 ret
op_state:
 mov ecx,CVDEV_REPORT_BYTES
 call guest_destination
 test eax,eax
 jnz state_done
 push edi
 mov dev_args,offset dev_report
 mov eax,offset cvdev_report_state
 mov ecx,1
 call dev_call
 pop edi
 mov esi,offset dev_report
 mov ecx,CVDEV_REPORT_BYTES
 cld
 rep movsb
 xor eax,eax
state_done:
 ret
op_io:
 mov eax,offset cvdev_note_bridge
 xor ecx,ecx
 call dev_call
 movzx ebx,word ptr [ebp].Client_Reg_Struc.Client_EBX
 movzx esi,byte ptr [ebp].Client_Reg_Struc.Client_ECX
 mov dev_args,ebx
 mov [dev_args+4],esi
 test byte ptr [ebp].Client_Reg_Struc.Client_ECX+1,1
 jz io_read
 mov eax,[ebp].Client_Reg_Struc.Client_EDX
 mov [dev_args+8],eax
 mov eax,offset cvdev_port_write
 mov ecx,3
 call dev_call
 xor eax,eax
 ret
io_read:
 mov eax,offset cvdev_port_read
 mov ecx,2
 call dev_call
 mov [ebp].Client_Reg_Struc.Client_EDX,eax
 xor eax,eax
 ret
inactive:
 mov eax,VM_ERROR_DEV_INACTIVE
 ret
dev_dispatch endp
