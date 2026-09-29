; Physical-IRQ host scheduler for the monitored foreground session.
; Included by session_jlm.asm and assembled against the explicitly patched
; Jemm/JLOAD service table.  The callback never calls DOS, BIOS or firmware.
; It owns one private stack and one conventional descriptor supplied by the
; session parent; it does not create another runnable DOS context.

include session_scheduler_abi.inc

.data
scheduler_storage_start label byte
scheduler_descriptor dd 0
scheduler_installed dd 0
scheduler_if_allowed dd 1
scheduler_if_flags dd 0
scheduler_busy db 0
align 4
scheduler_saved_esp dd 0
scheduler_internal_ticks dd 0
scheduler_internal_reentries dd 0
scheduler_stack db 16384 dup (0)
scheduler_stack_end label byte
scheduler_storage_end label byte

.code

scheduler_validate proc uses esi
 mov esi,scheduler_descriptor
 test esi,esi
 jz invalid
 cmp dword ptr [esi+CVSCHED_MAGIC_OFS],CVSCHED_MAGIC
 jne invalid
 cmp word ptr [esi+CVSCHED_VERSION_OFS],CVSCHED_VERSION
 jne invalid
 cmp word ptr [esi+CVSCHED_BYTES_OFS],CVSCHED_BYTES
 jne invalid
 mov eax,[esi+CVSCHED_STATE]
 test eax,CVSCHED_STATE_BOUND
 jz invalid
 xor eax,eax
 ret
invalid:
 mov eax,VM_ERROR_ABI
 ret
scheduler_validate endp

; Return only capabilities whose ownership handshake is currently true.
; In particular, a resident host without a live 3.24 per-client callback does
; not advertise protected-mode VGA I/O virtualization.
vm_scheduler_capabilities proc uses esi
 mov eax,VM_CAPABILITIES
 mov esi,scheduler_descriptor
 test esi,esi
 jz done
 cmp dword ptr [esi+CVSCHED_MAGIC_OFS],CVSCHED_MAGIC
 jne done
 cmp word ptr [esi+CVSCHED_VERSION_OFS],CVSCHED_VERSION
 jne done
 cmp word ptr [esi+CVSCHED_BYTES_OFS],CVSCHED_BYTES
 jne done
 test dword ptr [esi+CVSCHED_STATE],CVSCHED_STATE_CALLBACK
 jz done
 or eax,VM_CAP_HDP324_ADAPTER
done:
 push eax
 xor eax,eax
 @VMMCall Host_Scheduler_Profile
 test eax,VM_IF_AVAILABLE
 pop eax
 jz no_virtual_if
 or eax,VM_CAP_V86_VIRTUAL_IF
no_virtual_if:
 ret
vm_scheduler_capabilities endp

; BX function (see VM_OP_IF_PROFILE). Returns EAX=0 and the Jemm flags in
; the client EBX; CX reports whether this session allows the profile.
vm_scheduler_if_profile proc
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 cmp eax,2
 ja invalid
 test eax,eax
 jz report
 cmp eax,1
 je allow
 mov scheduler_if_allowed,0
 cmp scheduler_installed,0
 je report
 mov eax,2
 @VMMCall Host_Scheduler_Profile
 jmp report
allow:
 mov scheduler_if_allowed,1
 cmp scheduler_installed,0
 je report
 mov eax,1
 @VMMCall Host_Scheduler_Profile
report:
 xor eax,eax
 @VMMCall Host_Scheduler_Profile
 mov scheduler_if_flags,eax
 mov [ebp].Client_Reg_Struc.Client_EBX,eax
 mov eax,scheduler_if_allowed
 mov word ptr [ebp].Client_Reg_Struc.Client_ECX,ax
 xor eax,eax
 ret
invalid:
 mov eax,VM_ERROR_OPERATION
 ret
vm_scheduler_if_profile endp

vm_scheduler_info proc
 mov eax,scheduler_descriptor
 test eax,eax
 jz unbound
 mov ecx,eax
 shr ecx,4
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,cx
 and eax,0Fh
 mov word ptr [ebp].Client_Reg_Struc.Client_EDX,ax
 mov eax,scheduler_descriptor
 mov eax,[eax+CVSCHED_STATE]
 mov [ebp].Client_Reg_Struc.Client_ECX,eax
 xor eax,eax
 ret
unbound:
 mov eax,VM_ERROR_SCHED_UNBOUND
 ret
vm_scheduler_info endp

; EBP is the Jemm client frame. guest_destination validates ES:DI and returns
; the conventional linear address in EDI without touching it on failure.
vm_scheduler_bind proc uses esi edi ebx
 cmp scheduler_descriptor,0
 jne already
 cmp active,0
 jne active_error
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_ECX
 cmp ecx,CVSCHED_BYTES
 jb address_error
 mov ecx,CVSCHED_BYTES
 call guest_destination
 test eax,eax
 jnz done
 cmp dword ptr [edi+CVSCHED_MAGIC_OFS],CVSCHED_MAGIC
 jne abi_error
 cmp word ptr [edi+CVSCHED_VERSION_OFS],CVSCHED_VERSION
 jne abi_error
 cmp word ptr [edi+CVSCHED_BYTES_OFS],CVSCHED_BYTES
 jne abi_error
 mov scheduler_descriptor,edi
 lea esi,[edi+CVSCHED_GENERATION]
 push edi
 mov edi,esi
 xor eax,eax
 mov ecx,(CVSCHED_BYTES-CVSCHED_GENERATION)/4
 cld
 rep stosd
 pop edi
 mov dword ptr [edi+CVSCHED_STATE],CVSCHED_STATE_BOUND
 mov dword ptr [edi+CVSCHED_FLAGS],CVSCHED_FLAG_V86_IRQ or CVSCHED_FLAG_DPMI_IRQ or CVSCHED_FLAG_HDP324_ABI or CVSCHED_FLAG_PTE_GUARD
 mov dword ptr [edi+CVSCHED_SHADOW_BYTES],VGA_BYTES
 mov dword ptr [edi+CVSCHED_SCHEDULER_BYTES],(scheduler_storage_end-scheduler_storage_start)+CVSCHED_BYTES
 xor eax,eax
 ret
already:
 mov eax,VM_ERROR_SCHED_BOUND
 ret
active_error:
 mov eax,VM_ERROR_ACTIVE
 ret
address_error:
 mov eax,VM_ERROR_ADDRESS
 ret
abi_error:
 mov eax,VM_ERROR_ABI
done:
 ret
vm_scheduler_bind endp

vm_scheduler_arm proc uses esi edi ebx
 cmp scheduler_descriptor,0
 je optional
 call scheduler_validate
 test eax,eax
 jnz done
 mov edi,scheduler_descriptor
 mov eax,generation
 mov [edi+CVSCHED_GENERATION],eax
 mov eax,CVSCHED_STATE_BOUND or CVSCHED_STATE_SESSION
 mov [edi+CVSCHED_STATE],eax
 mov esi,VGA_PTES
 add edi,CVSCHED_PTES
 mov ecx,CVSCHED_PTE_COUNT
 cld
 rep movsd
 mov edi,scheduler_descriptor
 mov eax,cr3
 mov [edi+CVSCHED_LAST_CR3],eax
 mov eax,TICK_SCHEDULER
 call tick_acquire
 jc install_error
 mov scheduler_installed,1
 or dword ptr [edi+CVSCHED_STATE],CVSCHED_STATE_JEMM
 ; Negotiate Jemm's virtual-IF profile: request it only when this session
 ; allows it; a Jemm without the profile answers 0 and nothing changes.
 cmp scheduler_if_allowed,0
 je profile_done
 mov eax,1
 @VMMCall Host_Scheduler_Profile
 mov scheduler_if_flags,eax
profile_done:
 xor eax,eax
 ret
install_error:
 mov dword ptr [edi+CVSCHED_LAST_ERROR],CVSCHED_ERROR_INSTALL
 inc dword ptr [edi+CVSCHED_FAILURES]
 and dword ptr [edi+CVSCHED_STATE],not CVSCHED_STATE_SESSION
 mov eax,VM_ERROR_TRAP
 ret
optional:
 xor eax,eax
done:
 ret
vm_scheduler_arm endp

vm_scheduler_can_end proc uses esi
 cmp scheduler_descriptor,0
 je allowed
 call scheduler_validate
 test eax,eax
 jnz done
 mov esi,scheduler_descriptor
 mov eax,[esi+CVSCHED_STATE]
 test eax,CVSCHED_STATE_DPMI_HOST or CVSCHED_STATE_DPMI_CLIENT or CVSCHED_STATE_CALLBACK
 jz allowed
 mov dword ptr [esi+CVSCHED_LAST_ERROR],CVSCHED_ERROR_HOST_OWNED
 mov eax,VM_ERROR_DPMI_OWNED
 ret
allowed:
 xor eax,eax
done:
 ret
vm_scheduler_can_end endp

vm_scheduler_disarm proc uses esi
 cmp scheduler_installed,0
 je mark_inactive
 cmp vmm_installed,0                    ; the VM manager keeps the profile
 jne profile_kept
 mov eax,2
 @VMMCall Host_Scheduler_Profile
profile_kept:
 mov eax,TICK_SCHEDULER
 call tick_release
 jc remove_error
 mov scheduler_installed,0
mark_inactive:
 mov esi,scheduler_descriptor
 test esi,esi
 jz success
 and dword ptr [esi+CVSCHED_STATE],not (CVSCHED_STATE_SESSION or CVSCHED_STATE_JEMM)
success:
 xor eax,eax
 ret
remove_error:
 mov esi,scheduler_descriptor
 test esi,esi
 jz no_record
 mov dword ptr [esi+CVSCHED_LAST_ERROR],CVSCHED_ERROR_REMOVE
 or dword ptr [esi+CVSCHED_STATE],CVSCHED_STATE_FATAL
 inc dword ptr [esi+CVSCHED_FAILURES]
no_record:
 mov eax,VM_ERROR_TRAP
 ret
vm_scheduler_disarm endp

; Called by the real-mode owner only after the resident HDPMI discovery call
; reports absent. A live per-client callback/handle can never be cleared here.
vm_scheduler_dpmi_release proc uses esi
 call scheduler_validate
 test eax,eax
 jnz done
 mov esi,scheduler_descriptor
 mov eax,[esi+CVSCHED_STATE]
 test eax,CVSCHED_STATE_DPMI_CLIENT or CVSCHED_STATE_CALLBACK
 jnz owned
 cmp dword ptr [esi+CVSCHED_DPMI_HANDLE],0
 jne owned
 and dword ptr [esi+CVSCHED_STATE],not CVSCHED_STATE_DPMI_HOST
 xor eax,eax
 ret
owned:
 mov dword ptr [esi+CVSCHED_LAST_ERROR],CVSCHED_ERROR_HOST_OWNED
 mov eax,VM_ERROR_DPMI_OWNED
done:
 ret
vm_scheduler_dpmi_release endp

vm_scheduler_unbind proc uses esi
 cmp active,0
 jne active_error
 cmp scheduler_installed,0
 jne owned
 cmp scheduler_descriptor,0
 je unbound
 call scheduler_validate
 test eax,eax
 jnz done
 mov esi,scheduler_descriptor
 mov eax,[esi+CVSCHED_STATE]
 test eax,CVSCHED_STATE_DPMI_HOST or CVSCHED_STATE_DPMI_CLIENT or CVSCHED_STATE_CALLBACK
 jnz owned_record
 mov dword ptr [esi+CVSCHED_STATE],0
 mov scheduler_descriptor,0
 xor eax,eax
 ret
owned_record:
 mov dword ptr [esi+CVSCHED_LAST_ERROR],CVSCHED_ERROR_HOST_OWNED
owned:
 mov eax,VM_ERROR_DPMI_OWNED
 ret
active_error:
 mov eax,VM_ERROR_ACTIVE
 ret
unbound:
 mov eax,VM_ERROR_SCHED_UNBOUND
done:
 ret
vm_scheduler_unbind endp

; Called by Jemm before it decides whether IRQ0 is deliverable to the V86
; guest. Consequently guest virtual CLI cannot suppress this bounded service.
; Jemm supplies flat DS/ES and moves off its reserved client frame.  This
; function preserves caller state on its owned private stack and has its own
; non-reentrancy gate.
vm_scheduler_irq proc
 ; Acquire ownership on the caller's stack. A nested entry must never reset
 ; the active private stack or replace the outer saved ESP before it sees
 ; the busy flag. Keep original IF on the caller's stack until final return.
 pushfd
 cli
 push eax
 mov al,1
 xchg al,scheduler_busy
 test al,al
 jnz reentered
 pop eax
 mov scheduler_saved_esp,esp
 mov esp,offset scheduler_stack_end
 pushad
 push ds
 push es
 inc scheduler_internal_ticks
 call scheduler_validate
 test eax,eax
 jnz descriptor_bad
 mov edi,scheduler_descriptor
 cmp dword ptr [edi+CVSCHED_GENERATION],0
 je descriptor_bad_record
 mov eax,scheduler_internal_reentries
 mov scheduler_internal_reentries,0
 add [edi+CVSCHED_REENTRIES],eax
 inc dword ptr [edi+CVSCHED_V86_TICKS]
 inc dword ptr [edi+CVSCHED_SERVICES]
 mov eax,[edi+CVSCHED_REQUEST_GENERATION]
 mov [edi+CVSCHED_SERVICE_GENERATION],eax
 inc dword ptr [edi+CVSCHED_PTE_CHECKS]
 mov esi,VGA_PTES
 lea ebx,[edi+CVSCHED_PTES]
 mov ecx,CVSCHED_PTE_COUNT
 xor edx,edx
check_pte:
 mov eax,[ebx]
 cmp [esi],eax
 je pte_ok
 mov [esi],eax
 inc edx
pte_ok:
 add esi,4
 add ebx,4
 loop check_pte
 test edx,edx
 jz service_done
 add [edi+CVSCHED_PTE_REPAIRS],edx
 mov dword ptr [edi+CVSCHED_LAST_ERROR],CVSCHED_ERROR_PTES
 inc dword ptr [edi+CVSCHED_FAILURES]
 mov eax,cr3
 mov cr3,eax
 mov [edi+CVSCHED_LAST_CR3],eax
 jmp service_done
descriptor_bad_record:
 mov dword ptr [edi+CVSCHED_LAST_ERROR],CVSCHED_ERROR_DESCRIPTOR
 or dword ptr [edi+CVSCHED_STATE],CVSCHED_STATE_FATAL
 inc dword ptr [edi+CVSCHED_FAILURES]
 jmp service_done
descriptor_bad:
 ; The pointer cannot be trusted enough for another write. Keep only the
 ; module-local gate alive so END can still remove the exact callback.
service_done:
 pop es
 pop ds
 popad
 mov esp,scheduler_saved_esp
 mov byte ptr scheduler_busy,0
 popfd
 ret
reentered:
 inc scheduler_internal_reentries
 pop eax
 popfd
 ret
vm_scheduler_irq endp
