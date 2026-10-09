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
externdef c cvdev_set_pull:near
externdef c cvdev_set_hotkey:near
externdef c cvdev_instance_bytes:near
externdef c cvdev_select:near
externdef c cvdev_selected:near
externdef c cvdev_audio_poll:near
externdef c cvdev_audio_running:near
externdef c cvdev_audio_owner:near
externdef c cvdev_audio_stop:near
externdef c cvdev_wants_audio:near
externdef c cvdev_release_request:dword
externdef c cvdev_key:near
externdef c cvdev_mouse:near
externdef c cvdev_port_read:near
externdef c cvdev_port_write:near
externdef c cvdev_irq_filter:near
externdef c cvdev_poll:near
externdef c cvdev_report_state:near
externdef c cvdev_report_audio:near
externdef c cvdev_trap_ports:near
externdef c cvdev_note_bridge:near

externdef c cvdev_port_claimed:near
externdef c cvvid_note_device_irqs:near
externdef c cvvid_note_device_irqs_instance:near
externdef c cvvid_pm_attached_instance:near

.data
dev_block dd 0
dev_pages dd CVDEV_AUDIO_PAGES*2 dup (0)       ; cvdev_pages {linear, physical}
dev_ports dd 0                                  ; C trap list (uint16, FFFFh end)
dev_trapped dd 0
dev_hooked dd 0
dev_active dd 0
; Device IRQ lines held for the HDPMI adapter's protected-mode client. A
; DOS/4GW client runs its handlers on private stacks that are not reentrant;
; injecting these IRQs into V86 while HDPMI is inside such a handler (its
; I/O bridge, an IRQ reflected to real mode) nests them there.  They are
; delivered by the adapter at a client instruction boundary instead.
dev_pm_magic db 'CVPMHELD'
dev_pm_claimed dd 0
dev_pm_pending dd 0
dev_pm_delivered dd 0
dev_pm_refused dd 0
dev_granted dd 0
dev_audio dd 0
; The AC'97 stream and its pages are one for all sessions.
dev_stream dd 0                                 ; the stream runs
dev_sessions dd 0                               ; VMs with a device model
; Device ports are trapped while any session's model uses them.
DEV_REF_SLOTS equ 96
dev_ref_port dw DEV_REF_SLOTS dup (0FFFFh)
dev_ref_count db DEV_REF_SLOTS dup (0)
dev_audio_sessions dd 0                         ; sessions that play through it
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

; Publish the held lines in the shared video header, where the HDPMI adapter
; looks on every trapped I/O. Preserves every register.
dev_pm_publish proc
 pushad
 push dev_pm_pending
 call cvvid_note_device_irqs
 add esp,4
 popad
 ret
dev_pm_publish endp

; Raise an IRQ for VM in its own protected-mode pending set, even when another
; VM runs or a different device instance owns the AC'97 polling context.
; The current/borrowed VM has live dev_pm_* globals; other VMs have the same
; fields in VMREC.ctx. The target video instance is queried directly instead
; of using the caller's currently selected video instance.
cvdev_raise_irq proc c public vm:dword, irq:dword, audio:dword
 push esi
 push edi
 push ebx
 mov eax,vm
 cmp eax,VMM_MAX
 jae raise_failed
 call vmm_record
 mov ebx,[esi].VMREC.vid_inst
 mov edi,offset dev_pm_claimed
 mov eax,vmm_borrowed
 cmp eax,-1
 jne raise_selected
 mov eax,vmm_current
raise_selected:
 cmp eax,vm
 je raise_state_ready
 lea edi,[esi].VMREC.ctx+VMM_CTX_PM_OFFSET
raise_state_ready:
 cmp dword ptr [edi],0
 je raise_v86
 ; HDPMI maps the target VM's shared header until its PM client detaches.
 push ebx
 call cvvid_pm_attached_instance
 add esp,4
 test eax,eax
 jnz raise_held
 mov dword ptr [edi],0
 mov dword ptr [edi+4],0
 push 0
 push ebx
 call cvvid_note_device_irqs_instance
 add esp,8
 jmp raise_v86
raise_held:
 mov ecx,irq
 cmp ecx,16
 jae raise_v86
 bts dword ptr [edi+4],ecx
 push dword ptr [edi+4]
 push ebx
 call cvvid_note_device_irqs_instance
 add esp,8
 cmp audio,0
 je raise_success
 mov eax,vm
 cmp eax,vmm_current
 je raise_success
 mov vmm_audio_urgent,eax
raise_success:
 mov eax,1
 jmp raise_done
raise_v86:
 mov eax,vm
 mov ecx,irq
 call vmm_pend
 jc raise_failed
 mov eax,1
 jmp raise_done
raise_failed:
 xor eax,eax
raise_done:
 pop ebx
 pop edi
 pop esi
 ret
cvdev_raise_irq endp

; Protected-mode delivery for the report: delivered count (low word) |
; held lines << 16, bit 31 while claimed.
cvdev_pm_state proc c public
 mov eax,dev_pm_pending
 shl eax,16
 mov ax,word ptr dev_pm_delivered
 cmp dev_pm_claimed,0
 je pm_state_done
 or eax,80000000h
pm_state_done:
 ret
cvdev_pm_state endp

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
 push eax
 call dev_hotkey_check
 pop eax
 cmp eax,1
 jne pass
 stc
 ret
pass:
 clc
 ret
dev_filter endp

; The AC'97 stream, from jlm_poll whichever VM runs. Preserves registers.
dev_audio_poll proc
 pushad
 cmp dev_busy,0
 jne done
 mov eax,offset cvdev_audio_poll
 xor ecx,ecx
 call dev_call
done:
 popad
 ret
dev_audio_poll endp

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
 call dev_hotkey_check
done:
 popad
 ret
dev_poll endp

; ---- V86 port trap (Install_IO_Handler ABI) ----
; EDX port, ECX Jemm I/O flags, EAX output value. Returns EAX.
dev_port_handler proc
 cmp dev_active,0                       ; the running VM's device model
 jne dev_port_model
 @VMMCall Simulate_IO
 ret
dev_port_handler endp

dev_port_model proc uses esi edi ebx
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
dev_port_model endp

; ---- Shared port traps ----

; EDX = port: trapped for one more session. CF=1 when Jemm refused.
dev_trap_port proc uses ebx
 call dev_ref_slot
 jc full
 cmp dev_ref_count[ebx],0
 jne counted
 push esi
 mov esi,offset dev_port_handler
 @VMMCall Install_IO_Handler
 pop esi
 jc refused
 mov dev_ref_port[ebx*2],dx
counted:
 inc dev_ref_count[ebx]
 clc
 ret
full:
refused:
 stc
 ret
dev_trap_port endp

; EDX = port: one session less. The last one removes the trap (and gives
; port 60h back to the hardware, see dev_end). CF=1 when Jemm refused.
dev_untrap_port proc uses ebx
 call dev_ref_slot
 jc done
 cmp dev_ref_count[ebx],0
 je done
 cmp dev_ref_count[ebx],1
 ja counted
 @VMMCall Remove_IO_Handler
 jc refused
 mov dev_ref_port[ebx*2],0FFFFh
 cmp edx,60h
 jne counted
 call iopb_release
counted:
 dec dev_ref_count[ebx]
done:
 clc
 ret
refused:
 stc
 ret
dev_untrap_port endp

; EDX = port: EBX = its slot (or a free one). CF=1 when none is left.
dev_ref_slot proc
 xor ebx,ebx
find:
 cmp dev_ref_port[ebx*2],dx
 je found
 inc ebx
 cmp ebx,DEV_REF_SLOTS
 jb find
 xor ebx,ebx
free:
 cmp dev_ref_port[ebx*2],0FFFFh
 je found
 inc ebx
 cmp ebx,DEV_REF_SLOTS
 jb free
 stc
 ret
found:
 clc
 ret
dev_ref_slot endp

; ---- Lifecycle ----

; EDX = port: clear its bit in the TSS I/O permission bitmap, so V86 accesses
; reach the hardware directly. Preserves every register.
iopb_release proc
 push eax
 push ebx
 sub esp,8
 sgdt fword ptr [esp+2]
 mov ebx,[esp+4]                        ; GDT base
 str ax
 movzx eax,ax
 and eax,0FFF8h
 add ebx,eax                            ; the TSS descriptor
 mov eax,[ebx+2]
 and eax,00FFFFFFh
 movzx ebx,byte ptr [ebx+7]
 shl ebx,24
 or eax,ebx                             ; TSS base
 movzx ebx,word ptr [eax+66h]           ; I/O map base
 add eax,ebx
 btr [eax],edx
 add esp,8
 pop ebx
 pop eax
 ret
iopb_release endp

; Free the audio block (if any). Returns EAX=0 or VM_ERROR_TRAP.
dev_free_pages proc
 cmp dev_stream,0                       ; another session still plays
 jne freed
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
 ; physical byte can never be left unread behind a model-served IN. Jemm
 ; traps port 60h only for one read or write after a D0h/D1h command to 64h
 ; (A20 through the controller); JLOAD's Remove_IO_Handler leaves the IOPB
 ; bit of a port Jemm also handles, so dev_untrap_port gives 60h back (else
 ; the next keyboard read would go to Jemm's output-port emulation, A20 bit
 ; forced into the scan code).
 mov esi,dev_ports
remove_next:
 cmp dev_trapped,0
 je untrapped
 dec dev_trapped
 mov eax,dev_trapped
 movzx edx,word ptr [esi+eax*2]
 call dev_untrap_port
 jnc remove_next
 inc dev_trapped
 mov eax,VM_ERROR_TRAP
 ret
untrapped:
 dec dev_sessions
 mov eax,vmm_current
 call vmm_record
 and [esi].VMREC.sess,not (SESS_DEVICES or SESS_KBD or SESS_MOUSE or SESS_AUDIO)
 cmp dev_sessions,0                     ; another session keeps the hooks
 jne unhooked
 cmp dev_hooked,0
 je unhooked
 xor ecx,ecx                            ; the VM manager or another
 cmp vmm_installed,0                    ; session's stream keeps the poll
 jne poll_kept
 cmp dev_audio_sessions,1
 ja poll_kept
 jmp poll_off
poll_kept:
 mov ecx,offset jlm_poll
poll_off:
 mov eax,6
 @VMMCall Host_Scheduler_Profile
 xor ecx,ecx                            ; the VM manager keeps routing input
 cmp vmm_installed,0
 je filter_off
 mov ecx,offset jlm_filter
filter_off:
 mov eax,4
 @VMMCall Host_Scheduler_Profile
 mov dev_hooked,0
unhooked:
 mov eax,offset cvdev_end
 xor ecx,ecx
 call dev_call
 mov dev_active,0
 mov dev_pm_claimed,0
 mov dev_pm_pending,0
 call dev_pm_publish
 ; A byte the filter's IRQ already claimed but nobody read would keep the
 ; controller's output buffer full and silence IRQ1/IRQ12 for good.
 in al,64h
 test al,1
 jz controller_clear
 mov ecx,12                             ; mouse byte: the desktop's VM
 xor edx,edx
 test al,20h
 jnz raise_pending
 mov ecx,1                              ; keyboard byte: the focus VM
 mov edx,vmm_focus
raise_pending:
 mov eax,edx
 call vmm_pend
controller_clear:
 ; The stream stops with its last session.
 cmp dev_audio,1
 jne stream_kept
 mov dev_audio,0
 dec dev_audio_sessions
 jnz stream_kept
 mov eax,offset cvdev_audio_stop
 xor ecx,ecx
 call dev_call
 mov dev_stream,0
 call dev_free_pages
 ret
stream_kept:
 call vmm_pick_audio                    ; another session may play now
 xor eax,eax
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
 call vmm_device_instance
 test eax,eax
 jnz instance_failed
 xor eax,eax
 @VMMCall Host_Scheduler_Profile
 test eax,VM_IF_ACTIVE
 jz no_profile
 test dword ptr [ebp].Client_Reg_Struc.Client_ECX,CVDEV_FLAG_AUDIO_OUT
 jz pages_ready
 cmp dev_block,0                        ; the stream's pages exist already
 jne pages_ready
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
 test eax,eax
 jz tsc_unknown
 mov vmm_tsc_khz,eax                    ; for the VM manager's short waits
tsc_unknown:
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
 inc dev_sessions
install_next:
 mov eax,dev_trapped
 movzx edx,word ptr [esi+eax*2]
 cmp edx,0FFFFh
 je installed
 call dev_trap_port
 jc trap_failed
 inc dev_trapped
 jmp install_next
installed:
 cmp dev_hooked,0                       ; an earlier session's hooks serve
 jne hooks_ready
 mov ecx,offset jlm_filter
 mov eax,4
 @VMMCall Host_Scheduler_Profile
 mov ecx,offset jlm_poll
 mov eax,6
 @VMMCall Host_Scheduler_Profile
 mov dev_hooked,1
hooks_ready:
 ; What this VM's session has, for the routing between the VMs.
 mov eax,vmm_current
 call vmm_record
 or [esi].VMREC.sess,SESS_DEVICES
 test dev_granted,CVDEV_CAP_KEYBOARD
 jz no_keyboard
 or [esi].VMREC.sess,SESS_KBD
no_keyboard:
 test dev_granted,CVDEV_CAP_MOUSE
 jz no_mouse
 or [esi].VMREC.sess,SESS_MOUSE
no_mouse:
 cmp dev_audio,1                        ; CVDEV_AUDIO_RUNNING: it plays
 jne no_stream
 or [esi].VMREC.sess,SESS_AUDIO
 mov dev_stream,1
 inc dev_audio_sessions
no_stream:
 call vmm_input_route
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
instance_failed:
 ret
already:
 mov eax,VM_ERROR_DEV_ACTIVE
 ret
not_session:
 mov eax,VM_ERROR_INACTIVE
 ret
dev_begin endp

; The protected-mode client is gone (DPMIRUN after its exit): device IRQs
; go back to V86 injection. Lines still held were requests to that client
; and are dropped with it.
dev_pm_release proc
 mov dev_pm_claimed,0
 mov dev_pm_pending,0
 call dev_pm_publish
 ret
dev_pm_release endp

; Advance the devices now (their IRQs are raised or held), unless the model
; is already running below us.
dev_poll_now proc
 cmp dev_active,0
 je poll_now_done
 cmp dev_busy,0
 jne poll_now_done
 mov eax,offset cvdev_poll
 xor ecx,ecx
 call dev_call
poll_now_done:
 ret
dev_poll_now endp

; DEV_PM_IRQ: accept one held line through Jemm's virtual PIC (profile
; function 8), in 8259 priority order with the slave at master level 2.
dev_pm_irq proc
 cmp byte ptr [ebp].Client_Reg_Struc.Client_ECX,2
 je pm_accept
 call dev_pm_release
 xor eax,eax
 ret
pm_accept:
 call clock_poll
 call dev_poll_now
 mov dword ptr [ebp].Client_Reg_Struc.Client_EBX,-1
 xor esi,esi
pm_next_line:
 movzx ecx,byte ptr pm_priority[esi]
 inc esi
 cmp ecx,0FFh
 je pm_done
 bt dev_pm_pending,ecx
 jnc pm_next_line
 push ecx
 mov eax,8
 @VMMCall Host_Scheduler_Profile
 pop ecx
 jnc pm_accepted
 inc dev_pm_refused
 jmp pm_next_line
pm_accepted:
 btr dev_pm_pending,ecx
 call dev_pm_publish
 inc dev_pm_delivered
 mov [ebp].Client_Reg_Struc.Client_EBX,ecx
 mov [ebp].Client_Reg_Struc.Client_EDX,edx
pm_done:
 mov eax,dev_pm_pending
 mov [ebp].Client_Reg_Struc.Client_ESI,eax
 xor eax,eax
 ret
pm_priority db 0,1,8,9,10,11,12,13,14,15,3,4,5,6,7,0FFh
dev_pm_irq endp

; ---- V86 operations 30h-36h (EBP = client frame). EAX=0 or VM_ERROR_* ----
dev_dispatch proc uses esi edi ebx
 cmp eax,VM_OP_DEV_BEGIN
 je op_begin
 cmp eax,VM_OP_DEV_IO
 je op_io_any
 cmp eax,VM_OP_DEV_PM_IRQ
 je op_pm_irq
 cmp dev_active,0
 je inactive
 cmp eax,VM_OP_DEV_END
 je op_end
 cmp eax,VM_OP_DEV_FOCUS
 je op_focus
 cmp eax,VM_OP_DEV_STATE
 je op_state
 cmp eax,VM_OP_DEV_AUDIO_STATE
 je op_audio_state
 cmp eax,VM_OP_DEV_IO
 je op_io
 cmp eax,VM_OP_DEV_KEY
 je op_key
 cmp eax,VM_OP_DEV_MOUSE
 je op_mouse
 mov eax,VM_ERROR_OPERATION
 ret
op_mouse:
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 mov dev_args,eax
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_ECX
 mov [dev_args+4],eax
 movzx eax,byte ptr [ebp].Client_Reg_Struc.Client_EDX
 mov [dev_args+8],eax
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_ESI
 mov [dev_args+12],eax
 mov eax,offset cvdev_mouse
 mov ecx,4
 call dev_call
 test eax,eax
 jz mouse_done
 mov eax,VM_ERROR_OPERATION
mouse_done:
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
op_pm_irq:
 call dev_pm_irq
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
 mov eax,offset cvdev_report_state
 jmp state_packet
op_audio_state:
 mov eax,offset cvdev_report_audio
state_packet:
 push eax
 mov ecx,CVDEV_REPORT_BYTES
 call guest_destination
 pop edx
 test eax,eax
 jnz state_done
 push edi
 mov dev_args,offset dev_report
 mov eax,edx
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
op_io_any:
 ; The protected-mode bridge traps these ports for its client; the client
 ; cannot reach them itself. PIC ports are the V86 guest's virtual PIC
 ; (Jemm profile function 7); without devices everything else is physical.
 ; Its client now owns delivery of the device IRQs (DEV_PM_IRQ).
 mov dev_pm_claimed,1
 movzx ebx,word ptr [ebp].Client_Reg_Struc.Client_EBX
 cmp clock_enabled,0
 je io_no_pit
 cmp ebx,40h
 je io_pit
 cmp ebx,43h
 je io_pit
io_no_pit:
 cmp ebx,20h
 je io_pic
 cmp ebx,21h
 je io_pic
 cmp ebx,0A0h
 je io_pic
 cmp ebx,0A1h
 je io_pic
 cmp dev_active,0
 jne op_io
 jmp io_physical
io_pit:
 mov edx,ebx
 xor ecx,ecx
 test byte ptr [ebp].Client_Reg_Struc.Client_ECX+1,1
 jz io_pit_call
 mov ecx,OUTPUT
io_pit_call:
 mov eax,[ebp].Client_Reg_Struc.Client_EDX
 call clock_port
 mov [ebp].Client_Reg_Struc.Client_EDX,eax
 jmp io_done
io_pic:
 cmp byte ptr [ebp].Client_Reg_Struc.Client_ECX,1
 jne io_physical
 mov ecx,ebx
 test byte ptr [ebp].Client_Reg_Struc.Client_ECX+1,1
 jz io_pic_call
 or ecx,10000h
io_pic_call:
 mov edx,[ebp].Client_Reg_Struc.Client_EDX
 mov eax,7
 @VMMCall Host_Scheduler_Profile
 jc io_physical
 test byte ptr [ebp].Client_Reg_Struc.Client_ECX+1,1
 jnz io_done
 mov [ebp].Client_Reg_Struc.Client_EDX,edx
io_done:
 call clock_poll
 ; Report the held lines so the adapter knows when to ask for delivery.
 call dev_poll_now
 mov eax,dev_pm_pending
 mov [ebp].Client_Reg_Struc.Client_ESI,eax
 xor eax,eax
 ret
io_physical:
 mov edx,ebx
 mov cl,byte ptr [ebp].Client_Reg_Struc.Client_ECX
 test byte ptr [ebp].Client_Reg_Struc.Client_ECX+1,1
 jz io_physical_in
 mov eax,[ebp].Client_Reg_Struc.Client_EDX
 cmp cl,4
 je io_out32
 cmp cl,2
 je io_out16
 out dx,al
 jmp io_done
io_out16:
 out dx,ax
 jmp io_done
io_out32:
 out dx,eax
 jmp io_done
io_physical_in:
 xor eax,eax
 cmp cl,4
 je io_in32
 cmp cl,2
 je io_in16
 in al,dx
 jmp io_in_done
io_in16:
 in ax,dx
 jmp io_in_done
io_in32:
 in eax,dx
io_in_done:
 mov [ebp].Client_Reg_Struc.Client_EDX,eax
 jmp io_done
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
 jmp io_done
io_read:
 mov eax,offset cvdev_port_read
 mov ecx,2
 call dev_call
 mov [ebp].Client_Reg_Struc.Client_EDX,eax
 jmp io_done
inactive:
 mov eax,VM_ERROR_DEV_INACTIVE
 ret
dev_dispatch endp
