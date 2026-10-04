; Per-VM PIT0 clock. Physical IRQ0 is exclusively a host service deadline.
; C uses the device trampoline's flat private stack; no DOS or BIOS calls.
externdef c cvclock_begin:near
externdef c cvclock_port:near
externdef c cvclock_pending:near
externdef c cvclock_consume:near
externdef c cvclock_restore:near

.data
clock_enabled dd 0
clock_trapped dd 0
clock_magic db 'CVPITIME'
clock_queued dd 0
clock_pm_ticks dd 0
clock_host_ticks dd 0
CLOCK_RELOAD equ 5966                  ; 1193182 / 200, mode 2

.code
cvclock_now proc c public lowptr:dword, highptr:dword
 db 0Fh,31h
 mov ecx,lowptr
 mov [ecx],eax
 mov ecx,highptr
 mov [ecx],edx
 ret
cvclock_now endp

cvclock_div proc c public highval:dword, lowval:dword, divisor:dword, remainder:dword
 mov edx,highval
 mov eax,lowval
 div divisor
 mov ecx,remainder
 mov [ecx],edx
 ret
cvclock_div endp

; EAX=VM index. A newly created VM starts with the BIOS PIT programming.
clock_begin_vm proc uses eax ecx edx
 mov dev_args,eax
 mov eax,vmm_tsc_measured
 mov [dev_args+4],eax
 mov eax,offset cvclock_begin
 mov ecx,2
 call dev_call
 ret
clock_begin_vm endp

clock_arm proc uses esi edx ecx
 cmp clock_enabled,0
 jne ready
 cmp vmm_tsc_measured,0
 je failed
 cmp clock_trapped,0
 jne ports_ready
 mov esi,offset clock_port
 mov edx,40h
 @VMMCall Install_IO_Handler
 jc failed
 mov edx,43h
 @VMMCall Install_IO_Handler
 jnc ports_installed
 mov edx,40h
 @VMMCall Remove_IO_Handler
 jmp failed
ports_installed:
 mov clock_trapped,1
ports_ready:
 xor eax,eax
 call clock_begin_vm
 mov ecx,1
 mov eax,12
 @VMMCall Host_Scheduler_Profile
 jc failed
 mov al,34h
 out 43h,al
 mov al,CLOCK_RELOAD and 0FFh
 out 40h,al
 mov al,CLOCK_RELOAD shr 8
 out 40h,al
 mov clock_enabled,1
ready:
 clc
 ret
failed:
 stc
 ret
clock_arm endp

; Called after sole-desktop pending/ISR has drained, with physical IF clear.
clock_stop proc uses eax ecx edx
 cmp clock_enabled,0
 je done
 mov dev_args,0
 mov eax,offset cvclock_restore
 mov ecx,1
 call dev_call
 mov edx,eax
 shr eax,16
 out 43h,al
 mov eax,edx
 out 40h,al
 mov al,ah
 out 40h,al
 mov clock_enabled,0
done:
 ret
clock_stop endp

clock_untrap proc uses edx
 cmp clock_trapped,0
 je done
 mov edx,40h
 @VMMCall Remove_IO_Handler
 mov edx,43h
 @VMMCall Remove_IO_Handler
 mov clock_trapped,0
done:
 ret
clock_untrap endp

; EDX=40/43, ECX=Jemm I/O flags, EAX=value. Channels 1/2 remain physical.
clock_port proc uses ebx esi edi
 cmp clock_enabled,0
 je physical
 test ecx,STRING_IO
 jnz unsupported
 cmp dx,43h
 jne model
 test ecx,OUTPUT
 jz model
 mov bl,al
 and bl,0C0h
 jz model
 cmp bl,0C0h
 jne physical
 ; Read-back may select several counters. Route channel 0 to the model;
 ; physical channels 1/2 receive the same command with channel 0 excluded.
 push eax
 and al,0FDh
 out 43h,al
 pop eax
model:
 mov ebx,eax
 mov eax,vmm_live
 mov dev_args,eax
 movzx edx,dx
 mov [dev_args+4],edx
 and ecx,OUTPUT
 mov [dev_args+8],ecx
 mov [dev_args+12],ebx
 mov eax,offset cvclock_port
 mov ecx,4
 call dev_call
 and ebx,0FFFFFF00h
 and eax,0FFh
 or eax,ebx
 ret
physical:
 @VMMCall Simulate_IO
 ret
unsupported:
 or eax,-1
 ret
clock_port endp

; Current VM only: elapsed time accrues while other VMs run. One request
; leaves the expiry queue only after the authoritative virtual PIC accepts
; it as pending. Pending held PM IRQ0 is included in that check.
clock_poll proc
 pushad
 cmp clock_enabled,0
 je done
 cmp dev_busy,0
 jne done
 mov eax,vmm_live
 call vmm_record
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne done
 mov dev_args,eax
 mov eax,offset cvclock_pending
 mov ecx,1
 call dev_call
 test eax,eax
 jz done
 test dev_pm_pending,1
 jnz done
 mov eax,5
 @VMMCall Host_Scheduler_Profile
 jc done
 or ecx,edx
 test ecx,1
 jnz done
 push 0
 push 0
 push vmm_live
 call cvdev_raise_irq
 add esp,12
 test eax,eax
 jz done
 mov eax,vmm_live
 mov dev_args,eax
 mov eax,offset cvclock_consume
 mov ecx,1
 call dev_call
 inc clock_queued
done:
 popad
 ret
clock_poll endp
