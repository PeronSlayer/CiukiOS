; Bounded CPL3 execution boundary, separate from Jemm's V86 frame router.
; Called only by trusted CVSESSION code on the Jemm owner CR3. The caller
; owns and validates all mappings in the supplied private page directory.
; Private code/data/stack are the ONLY user mappings. Kernel/module/GDT/
; page tables and this IDT/TSS/stack remain mapped supervisor at their normal
; linear addresses. This routine neither allocates nor releases those pages.
;
; IF=0, TF=1, IOPL=0 and a mandatory <=256 instruction budget make this an
; executable diagnostic boundary, not a native scheduler. Every normal exit,
; user fault and exhausted budget returns to the exact owner state. Hardware
; IRQs are deferred only for this bounded quantum. Native TSS has no I/O map,
; so all user port I/O faults independently of the legacy DOS bitmap.
; Sources and integration constraints: docs/validation/2026-10-02-native-entry-boundary.md
.586p
.model flat, stdcall
.nolist
include jlm.inc
include native_entry_abi.inc
.list

NE_FRAME struct
 r_gs dd ?
 r_fs dd ?
 r_es dd ?
 r_ds dd ?
 r_edi dd ?
 r_esi dd ?
 r_ebp dd ?
 ignored_esp dd ?
 r_ebx dd ?
 r_edx dd ?
 r_ecx dd ?
 r_eax dd ?
 vector dd ?
 error dd ?
 eip dd ?
 r_cs dd ?
 eflags dd ?
 user_esp dd ?
 user_ss dd ?
NE_FRAME ends

.data
align 16
native_lock dd 0
native_context dd 0
native_continuation dd 0
native_owner_esp dd 0
native_owner_cr3 dd 0
native_owner_cr4 dd 0
native_owner_cr0 dd 0
native_owner_dr6 dd 0
native_owner_dr7 dd 0
native_owner_idtr dw 0
 dd 0
native_owner_gdtr dw 0
 dd 0
native_owner_tr dw 0
native_owner_tss_access db 0
 db 0
native_kernel_cs dw 0
native_user_cs dw 0
native_user_ds dw 0
native_tss_selector dw 0
native_tss_accessible dd 0
native_tables_active dd 0
native_idtr dw 2047
 dd offset native_idt
align 16
native_idt db 2048 dup(0)
; 32-bit TSS fixed part: I/O-map offset 104 exceeds descriptor limit 103.
native_tss db 104 dup(0)
align 16
native_stack db 4096 dup(0)
native_stack_top label byte

.code
public native_entry_run
public native_entry_step
; ESI -> trusted NATIVE_ENTRY_CONTEXT. EAX=result. Other registers,
; selectors and flags are preserved. All output fields initialize on entry.
native_entry_run proc
 pushfd
 cli
 push 0
 jmp native_entry_core
native_entry_run endp

; ESI -> 136-byte trusted continuation context with CNCT magic.
; Result NE_BUDGET saves a post-instruction frame and returns suspended.
; Caller must yield to DOS/audio before the next call; it retains all pages.
native_entry_step proc
 pushfd
 cli
 push 1
 jmp native_entry_core
native_entry_step endp

native_entry_core proc
 pushad
 push ds
 push es
 push fs
 push gs
 mov ax,10h
 mov ds,ax
 mov es,ax
 cmp native_lock,0
 jne entry_busy
 mov native_lock,1
 mov native_owner_esp,esp
 mov native_context,esi
 mov eax,[esp+48]                     ; mode below saved GPRs/selectors
 mov native_continuation,eax
 cld
 lea edi,[esi].NATIVE_ENTRY_CONTEXT.result
 xor eax,eax
 mov ecx,(sizeof NATIVE_ENTRY_CONTEXT-24)/4
 rep stosd
 mov [esi].NATIVE_ENTRY_CONTEXT.result,NE_CONFIG
 cmp native_continuation,0
 je entry_base_config
 cmp dword ptr [esi+NC_MAGIC],NATIVE_CONT_MAGIC
 jne entry_release
 cmp dword ptr [esi+NC_STATE],NC_SUSPENDED
 je entry_check_continuation
 cmp dword ptr [esi+NC_STATE],NC_FRESH
 jne entry_release
 mov dword ptr [esi+NC_QUANTA],0
 mov dword ptr [esi+NC_STEPS],0
 mov eax,[esi].NATIVE_ENTRY_CONTEXT.cr3_physical
 mov [esi+NC_CR3],eax
 jmp entry_base_config
entry_check_continuation:
 mov eax,[esi].NATIVE_ENTRY_CONTEXT.cr3_physical
 cmp eax,[esi+NC_CR3]
 jne entry_release
 cmp dword ptr [esi+NC_EIP],100000h
 jb entry_release
 cmp dword ptr [esi+NC_ESP],100000h
 jbe entry_release
 mov ecx,4
 lea edi,[esi+NC_DS]
entry_check_modes:
 cmp dword ptr [edi],NC_SEG_CODE
 ja entry_release
 add edi,4
 loop entry_check_modes
entry_base_config:
 mov eax,[esi].NATIVE_ENTRY_CONTEXT.budget
 test eax,eax
 jz entry_release
 cmp eax,NE_MAX_STEPS
 ja entry_release
 mov eax,[esi].NATIVE_ENTRY_CONTEXT.cr3_physical
 test eax,0FFFh
 jnz entry_release
 test eax,eax
 jz entry_release
 mov edx,cr3
 mov native_owner_cr3,edx
 cmp eax,edx
 je entry_release
 cmp [esi].NATIVE_ENTRY_CONTEXT.entry_linear,100000h
 jb entry_release
 cmp [esi].NATIVE_ENTRY_CONTEXT.stack_linear,100000h
 jbe entry_release
 mov eax,cr4
 mov native_owner_cr4,eax
 test eax,20h                         ; non-PAE private page directories only
 jnz entry_release
 sidt native_owner_idtr
 sgdt native_owner_gdtr
 str native_owner_tr
 mov ax,cs
 mov native_kernel_cs,ax
 movzx eax,native_owner_tr
 and eax,0FFF8h
 mov edx,dword ptr native_owner_gdtr+2
 mov cl,[edx+eax+5]
 mov native_owner_tss_access,cl
 mov native_user_cs,0
 mov native_user_ds,0
 mov native_tss_selector,0
 mov native_tables_active,0
 mov native_tss_accessible,0

 ; Pinned JLOAD stack ABI: flags at ESP+4, low32 at +8, high32 at +12.
 push 00CFFA00h                         ; base0, limit4GiB, 32-bit DPL3 code
 push 0000FFFFh
 push 0
 @VMMCall _Allocate_GDT_Selector
 add esp,12
 test eax,eax
 jz entry_cleanup
 or ax,3
 mov native_user_cs,ax
 push 00CFF200h                         ; base0, limit4GiB, 32-bit DPL3 data
 push 0000FFFFh
 push 0
 @VMMCall _Allocate_GDT_Selector
 add esp,12
 test eax,eax
 jz entry_cleanup
 or ax,3
 mov native_user_ds,ax

 ; Initialize an owned TSS with a ring-0 stack and an absent I/O bitmap.
 mov edi,offset native_tss
 xor eax,eax
 mov ecx,104/4
 rep stosd
 mov dword ptr native_tss+4,offset native_stack_top
 mov word ptr native_tss+8,10h         ; Jemm FLAT_DATA_SEL
 mov word ptr native_tss+102,104
 mov eax,offset native_tss
 mov edx,eax
 shl eax,16
 or eax,103                           ; descriptor low32: limit/base low16
 and edx,0FF000000h
 mov ecx,offset native_tss
 shr ecx,16
 and ecx,0FFh
 or edx,ecx
 or edx,00008900h                     ; present available 32-bit TSS
 push edx
 push eax
 push 0
 @VMMCall _Allocate_GDT_Selector
 add esp,12
 test eax,eax
 jz entry_cleanup
 mov native_tss_selector,ax

 ; Every hardware vector has a normalized stub; only INT80 is DPL3.
 mov edi,offset native_idt
 mov esi,offset native_vector_addresses
 xor ebx,ebx
entry_idt_loop:
 lodsd
 mov [edi],ax
 mov dx,native_kernel_cs
 mov [edi+2],dx
 mov word ptr [edi+4],08E00h
 cmp ebx,80h
 jne entry_idt_kernel
 mov word ptr [edi+4],0EE00h
entry_idt_kernel:
 shr eax,16
 mov [edi+6],ax
 add edi,8
 inc ebx
 cmp ebx,256
 jb entry_idt_loop

 ; Interrupts remain disabled until the original owner context is restored.
 ; Clearing PGE flushes old global user translations from the legacy CR3.
 mov eax,dr6
 mov native_owner_dr6,eax
 mov eax,dr7
 mov native_owner_dr7,eax
 xor eax,eax
 mov dr7,eax                          ; user budget uses TF, not old breakpoints
 mov eax,native_owner_cr4
 and eax,not 80h
 mov cr4,eax
 mov eax,cr0
 mov native_owner_cr0,eax
 or eax,0Ah                           ; TS+MP: integer-only ABI, ESC/WAIT fault
 mov cr0,eax
 mov esi,native_context
 mov eax,[esi].NATIVE_ENTRY_CONTEXT.cr3_physical
 mov cr3,eax
 lidt fword ptr native_idtr
 mov native_tables_active,1
 mov ax,native_tss_selector
 ltr ax
 mov native_tss_accessible,1
 mov [esi].NATIVE_ENTRY_CONTEXT.result,NE_OK
 cmp native_continuation,0
 je entry_fresh_frame
 inc dword ptr [esi+NC_QUANTA]
 cmp dword ptr [esi+NC_STATE],NC_SUSPENDED
 je entry_resumed_frame

 ; The trusted caller supplies the data ABI; all other user GPRs start zero.
entry_fresh_frame:
 movzx eax,native_user_ds
 push eax
 push [esi].NATIVE_ENTRY_CONTEXT.stack_linear
 push 102h                            ; TF=1, IF=0, IOPL=0, VM/NT/AC=0
 movzx eax,native_user_cs
 push eax
 push [esi].NATIVE_ENTRY_CONTEXT.entry_linear
 mov edi,[esi].NATIVE_ENTRY_CONTEXT.data_bytes
 mov esi,[esi].NATIVE_ENTRY_CONTEXT.data_linear
 mov ax,native_user_ds
 mov ds,ax
 mov es,ax
 xor eax,eax
 mov fs,ax
 mov gs,ax
 xor ebx,ebx
 xor ecx,ecx
 xor edx,edx
 xor ebp,ebp
 iretd

entry_resumed_frame:
 ; Rebuild transient selectors from validated flat-segment modes. Numeric
 ; user selectors from the previous quantum have already been released.
 movzx eax,native_user_ds
 push eax
 push dword ptr [esi+NC_ESP]
 mov eax,[esi+NC_FLAGS]
 and eax,0CD5h
 or eax,102h
 push eax
 movzx eax,native_user_cs
 push eax
 push dword ptr [esi+NC_EIP]
 ; PUSHAD-compatible register block; ignored ESP is not a user register.
 push dword ptr [esi+NC_EAX]
 push dword ptr [esi+NC_ECX]
 push dword ptr [esi+NC_EDX]
 push dword ptr [esi+NC_EBX]
 push 0
 push dword ptr [esi+NC_EBP]
 push dword ptr [esi+NC_ESI]
 push dword ptr [esi+NC_EDI]
 mov eax,[esi+NC_DS]
 call native_mode_selector
 push eax
 mov eax,[esi+NC_ES]
 call native_mode_selector
 push eax
 mov eax,[esi+NC_FS]
 call native_mode_selector
 push eax
 mov eax,[esi+NC_GS]
 call native_mode_selector
 push eax
 pop gs
 pop fs
 pop es
 pop ds
 popad
 iretd

native_owner_return::
 ; Reached through IRET to the saved ring-0 CS. Its abandoned user frame
 ; lives only on our private kernel stack; restore the original stack.
 mov ax,10h
 mov ds,ax
 mov es,ax
 mov eax,native_owner_cr3
 mov cr3,eax
 mov eax,native_owner_cr4
 mov cr4,eax
 mov eax,native_owner_cr0
 mov cr0,eax
 lidt fword ptr native_owner_idtr
 mov native_tables_active,0
 ; LTR leaves the previous descriptor busy. Clear only the saved old
 ; descriptor's busy type before restoring TR, then restore its exact byte.
 movzx eax,native_owner_tr
 and eax,0FFF8h
 mov edx,dword ptr native_owner_gdtr+2
 mov cl,byte ptr native_owner_tss_access
 and cl,not 2
 mov [edx+eax+5],cl
 mov ax,native_owner_tr
 ltr ax
 movzx eax,native_owner_tr
 and eax,0FFF8h
 mov cl,byte ptr native_owner_tss_access
 mov [edx+eax+5],cl
 mov native_tss_accessible,0
 mov eax,native_owner_dr6
 mov dr6,eax
 mov eax,native_owner_dr7
 mov dr7,eax
 mov esp,native_owner_esp
entry_cleanup:
 ; All allocator calls occur after restoring Jemm's CR3/IDT/TR.
 movzx eax,native_tss_selector
 test eax,eax
 jz entry_free_ds
 mov edx,dword ptr native_owner_gdtr+2
 and byte ptr [edx+eax+5],not 2
 push 0
 push eax
 @VMMCall _Free_GDT_Selector
 add esp,8
 mov native_tss_selector,0
entry_free_ds:
 movzx eax,native_user_ds
 test eax,eax
 jz entry_free_cs
 and eax,0FFF8h
 push 0
 push eax
 @VMMCall _Free_GDT_Selector
 add esp,8
 mov native_user_ds,0
entry_free_cs:
 movzx eax,native_user_cs
 test eax,eax
 jz entry_release
 and eax,0FFF8h
 push 0
 push eax
 @VMMCall _Free_GDT_Selector
 add esp,8
 mov native_user_cs,0
entry_release:
 mov esi,native_context
 mov eax,[esi].NATIVE_ENTRY_CONTEXT.result
 cmp native_continuation,0
 je entry_result_ready
 cmp dword ptr [esi+NC_MAGIC],NATIVE_CONT_MAGIC
 jne entry_result_ready
 mov edx,[esi].NATIVE_ENTRY_CONTEXT.steps
 add [esi+NC_STEPS],edx
 cmp eax,NE_BUDGET
 je entry_result_ready
 mov dword ptr [esi+NC_STATE],NC_TERMINAL
entry_result_ready:
 mov [esp+44],eax                     ; saved EAX after four selector pushes
 mov native_context,0
 mov native_lock,0
 jmp entry_restore
entry_busy:
 mov dword ptr [esp+44],NE_BUSY
entry_restore:
 pop gs
 pop fs
 pop es
 pop ds
 popad
 add esp,4                            ; private entry mode
 popfd
 ret
native_entry_core endp

native_mode_selector proc
 test eax,eax
 jz mode_ready
 cmp eax,NC_SEG_DATA
 jne mode_code
 movzx eax,native_user_ds
 ret
mode_code:
 movzx eax,native_user_cs
mode_ready:
 ret
native_mode_selector endp

; EAX = selector, returns abstract flat mode in EAX, CF=1 for an unowned
; selector. This prevents restoring a stale or foreign user GDT reference.
native_selector_mode proc
 and eax,0FFFFh
 test eax,eax
 jz selector_null
 cmp ax,native_user_ds
 je selector_data
 cmp ax,native_user_cs
 je selector_code
 stc
 ret
selector_null:
 xor eax,eax
 clc
 ret
selector_data:
 mov eax,NC_SEG_DATA
 clc
 ret
selector_code:
 mov eax,NC_SEG_CODE
 clc
 ret
native_selector_mode endp

native_save_continuation proc
 mov ax,word ptr [ebp].NE_FRAME.r_cs
 cmp ax,native_user_cs
 jne continuation_bad
 mov ax,word ptr [ebp].NE_FRAME.user_ss
 cmp ax,native_user_ds
 jne continuation_bad
 mov eax,[ebp].NE_FRAME.r_ds
 call native_selector_mode
 jc continuation_bad
 mov [esi+NC_DS],eax
 mov eax,[ebp].NE_FRAME.r_es
 call native_selector_mode
 jc continuation_bad
 mov [esi+NC_ES],eax
 mov eax,[ebp].NE_FRAME.r_fs
 call native_selector_mode
 jc continuation_bad
 mov [esi+NC_FS],eax
 mov eax,[ebp].NE_FRAME.r_gs
 call native_selector_mode
 jc continuation_bad
 mov [esi+NC_GS],eax
 mov eax,[ebp].NE_FRAME.r_edi
 mov [esi+NC_EDI],eax
 mov eax,[ebp].NE_FRAME.r_esi
 mov [esi+NC_ESI],eax
 mov eax,[ebp].NE_FRAME.r_ebp
 mov [esi+NC_EBP],eax
 mov eax,[ebp].NE_FRAME.r_ebx
 mov [esi+NC_EBX],eax
 mov eax,[ebp].NE_FRAME.r_edx
 mov [esi+NC_EDX],eax
 mov eax,[ebp].NE_FRAME.r_ecx
 mov [esi+NC_ECX],eax
 mov eax,[ebp].NE_FRAME.r_eax
 mov [esi+NC_EAX],eax
 mov eax,[ebp].NE_FRAME.eip
 mov [esi+NC_EIP],eax
 mov eax,[ebp].NE_FRAME.user_esp
 mov [esi+NC_ESP],eax
 mov eax,[ebp].NE_FRAME.eflags
 and eax,0CD5h
 or eax,102h
 mov [esi+NC_FLAGS],eax
 mov dword ptr [esi+NC_STATE],NC_SUSPENDED
 clc
 ret
continuation_bad:
 mov [esi].NATIVE_ENTRY_CONTEXT.result,NE_FAULT
 mov [esi].NATIVE_ENTRY_CONTEXT.fault_vector,13
 mov [esi].NATIVE_ENTRY_CONTEXT.fault_error,0
 stc
 ret
native_save_continuation endp

native_common proc
 pushad
 push ds
 push es
 push fs
 push gs
 mov ax,10h
 mov ds,ax
 mov es,ax
 cld
 mov ebp,esp
 mov esi,native_context
 ; Our IDT is active only inside the bounded runner. A kernel fault is
 ; still returned as failure; never try to resume a partially failed gate.
 cmp [ebp].NE_FRAME.vector,1
 je native_debug
 cmp [ebp].NE_FRAME.vector,80h
 je native_syscall
 mov [esi].NATIVE_ENTRY_CONTEXT.result,NE_FAULT
 mov eax,[ebp].NE_FRAME.vector
 mov [esi].NATIVE_ENTRY_CONTEXT.fault_vector,eax
 mov eax,[ebp].NE_FRAME.error
 mov [esi].NATIVE_ENTRY_CONTEXT.fault_error,eax
 mov eax,cr2
 mov [esi].NATIVE_ENTRY_CONTEXT.fault_address,eax
 jmp native_abort
native_debug:
 mov eax,[ebp].NE_FRAME.r_cs
 and eax,3
 cmp eax,3
 jne native_bad_syscall
 inc [esi].NATIVE_ENTRY_CONTEXT.steps
 mov eax,[esi].NATIVE_ENTRY_CONTEXT.steps
 cmp eax,[esi].NATIVE_ENTRY_CONTEXT.budget
 jb native_resume
 jmp native_yield
native_syscall:
 mov eax,[ebp].NE_FRAME.r_cs
 and eax,3
 cmp eax,3
 jne native_bad_syscall
 inc [esi].NATIVE_ENTRY_CONTEXT.steps
 cmp [ebp].NE_FRAME.r_eax,0
 je native_exit
 cmp [ebp].NE_FRAME.r_eax,1
 jne native_unknown_syscall
 inc [esi].NATIVE_ENTRY_CONTEXT.reports
 mov eax,[ebp].NE_FRAME.r_ebx
 mov [esi].NATIVE_ENTRY_CONTEXT.report_tag,eax
 mov eax,[ebp].NE_FRAME.r_ecx
 mov [esi].NATIVE_ENTRY_CONTEXT.report_value,eax
 mov [ebp].NE_FRAME.r_eax,0
 jmp native_resume
native_unknown_syscall:
 mov [ebp].NE_FRAME.r_eax,-38
 jmp native_resume
native_yield:
 mov [esi].NATIVE_ENTRY_CONTEXT.result,NE_BUDGET
 cmp native_continuation,0
 je native_abort
 call native_save_continuation
 jmp native_abort
native_bad_syscall:
 mov [esi].NATIVE_ENTRY_CONTEXT.result,NE_FAULT
 mov [esi].NATIVE_ENTRY_CONTEXT.fault_vector,80h
 jmp native_abort
native_exit:
 mov eax,[ebp].NE_FRAME.r_ebx
 mov [esi].NATIVE_ENTRY_CONTEXT.exit_status,eax
 mov [esi].NATIVE_ENTRY_CONTEXT.result,NE_OK
native_abort:
 ; IRET clears NMI blocking if vector2 caused termination. A same-CPL0
 ; return consumes EIP/CS/EFLAGS; trampoline discards the remaining frame.
 mov [ebp].NE_FRAME.eip,offset native_owner_return
 movzx eax,native_kernel_cs
 mov [ebp].NE_FRAME.r_cs,eax
 mov [ebp].NE_FRAME.eflags,2
native_resume:
 mov eax,[ebp].NE_FRAME.r_cs
 and eax,3
 cmp eax,3
 jne native_pop_frame
 ; A syscall is already complete here: if it exhausts this quantum, save
 ; its post-instruction EIP/EAX so a later resume cannot repeat the report.
 mov eax,[esi].NATIVE_ENTRY_CONTEXT.steps
 cmp eax,[esi].NATIVE_ENTRY_CONTEXT.budget
 jae native_yield
 ; TF is tested at the beginning of an instruction, so a user POPFD that
 ; clears TF still traps afterward. Re-arm TF on every user return and
 ; retain only arithmetic/direction flags; never accept user NT/IOPL/IF.
 and [ebp].NE_FRAME.eflags,0CD5h
 or [ebp].NE_FRAME.eflags,102h
native_pop_frame:
 pop gs
 pop fs
 pop es
 pop ds
 popad
 add esp,8
 iretd
native_common endp

NE_VECTOR macro number:req
native_vector_&number:
 if (number NE 8) AND (number NE 10) AND (number NE 11) AND (number NE 12) AND (number NE 13) AND (number NE 14) AND (number NE 17)
  push 0
 endif
 push number
 jmp native_common
endm
NE_ADDRESS macro number:req
 dd offset native_vector_&number
endm
NE_V=0
REPT 256
 NE_VECTOR %NE_V
 NE_V=NE_V+1
ENDM
.data
native_vector_addresses label dword
NE_V=0
REPT 256
 NE_ADDRESS %NE_V
 NE_V=NE_V+1
ENDM
end
