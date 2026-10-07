; Optional CVSWTR02 VMM frame and guest-stack switch trace.
; this file is empty in ordinary builds unless VMM_SWITCH_TRACE is defined.

ifdef VMM_SWITCH_TRACE
.data
align 4
public vmm_switch_trace_magic
vmm_switch_trace_magic db 'CVSWTR02'
vmm_switch_trace_next dd 0
vmm_switch_trace_count dd 0
vmm_switch_trace_sequence dd 0
vmm_switch_trace_frozen dd 0       ; bit0: destination saved CS match, bit1: loaded CS match
vmm_switch_trace_pending_index dd 0
vmm_switch_trace_pending dd 0
vmm_switch_trace_records dd 32*32 dup (0)
vmm_switch_trace_live_cvif dd 0

.code

; Called with PUSHFD/PUSHAD active. At vmm_switch's real switch_to label,
; EDI=outgoing VMREC, ESI=incoming VMREC, EBX=incoming VM id, EBP=live frame.
vmm_switch_trace_begin proc
 cmp vmm_switch_trace_frozen,0
 jne vmm_switch_trace_begin_done
 mov eax,vmm_switch_trace_next
 and eax,31
 mov vmm_switch_trace_pending_index,eax
 imul eax,eax,128
 lea edx,vmm_switch_trace_records[eax]

 mov ecx,vmm_switch_trace_sequence
 inc ecx
 mov vmm_switch_trace_sequence,ecx
 mov [edx],ecx
 mov eax,vmm_current
 mov [edx+4],eax
 mov [edx+8],ebx
 mov eax,[edi].VMREC.state
 mov [edx+12],eax
 mov eax,[esi].VMREC.state
 mov [edx+16],eax
 mov [edx+20],ebp

 mov eax,[ebp].Client_Reg_Struc.Client_CS
 mov [edx+24],eax
 mov eax,[ebp].Client_Reg_Struc.Client_EIP
 mov [edx+28],eax
 mov eax,[ebp].Client_Reg_Struc.Client_SS
 mov [edx+32],eax
 mov eax,[ebp].Client_Reg_Struc.Client_ESP
 mov [edx+36],eax
 mov eax,[ebp].Client_Reg_Struc.Client_EFlags
 mov [edx+40],eax

 pushad
 mov ebx,[ebp].Client_Reg_Struc.Client_SS
 and ebx,0FFFFh
 shl ebx,4
 mov eax,[ebp].Client_Reg_Struc.Client_ESP
 lea edi,[edx+96]
 call vmm_switch_trace_stack
 popad

 lea eax,[esi].VMREC.frame
 mov ecx,[eax+44]
 mov [edx+44],ecx
 mov ecx,[eax+40]
 mov [edx+48],ecx
 mov ecx,[eax+56]
 mov [edx+52],ecx
 mov ecx,[eax+52]
 mov [edx+56],ecx
 mov ecx,[eax+48]
 mov [edx+60],ecx
 mov eax,vmm_switch_trace_live_cvif
 mov [edx+64],eax
 lea eax,[esi].VMREC.cv
 mov eax,[eax]
 mov [edx+68],eax
 mov eax,[esi].VMREC.cv_valid
 mov [edx+72],eax

 mov eax,[edx+44]
 cmp eax,3146h
 jne vmm_switch_trace_saved_checked
 or dword ptr vmm_switch_trace_frozen,1
vmm_switch_trace_saved_checked:
 mov eax,vmm_switch_trace_next
 inc eax
 and eax,31
 mov vmm_switch_trace_next,eax
 mov eax,vmm_switch_trace_count
 cmp eax,32
 jae vmm_switch_trace_count_ready
 inc eax
 mov vmm_switch_trace_count,eax
vmm_switch_trace_count_ready:
 mov vmm_switch_trace_pending,1
vmm_switch_trace_begin_done:
 ret
vmm_switch_trace_begin endp

; Complete the just-created entry directly after the incoming frame copy.
; A saved-CS freeze still permits this post-load half to run once.
vmm_switch_trace_loaded proc
 cmp vmm_switch_trace_pending,1
 jne vmm_switch_trace_loaded_done
 mov eax,vmm_switch_trace_pending_index
 imul eax,eax,128
 lea edi,vmm_switch_trace_records[eax]
 mov eax,[ebp].Client_Reg_Struc.Client_CS
 mov [edi+76],eax
 cmp eax,3146h
 jne vmm_switch_trace_loaded_ip
 or dword ptr vmm_switch_trace_frozen,2
vmm_switch_trace_loaded_ip:
 mov eax,[ebp].Client_Reg_Struc.Client_EIP
 mov [edi+80],eax
 mov eax,[ebp].Client_Reg_Struc.Client_SS
 mov [edi+84],eax
 mov eax,[ebp].Client_Reg_Struc.Client_ESP
 mov [edi+88],eax
 mov eax,[ebp].Client_Reg_Struc.Client_EFlags
 mov [edi+92],eax
 mov ebx,[ebp].Client_Reg_Struc.Client_SS
 and ebx,0FFFFh
 shl ebx,4
 mov eax,[ebp].Client_Reg_Struc.Client_ESP
 add edi,112
 call vmm_switch_trace_stack
 mov vmm_switch_trace_pending,0
vmm_switch_trace_loaded_done:
 ret
vmm_switch_trace_loaded endp

; EBX = guest SS base, AX = SP, EDI = diagnostic destination.
; The active guest pages are mapped. Wrap each byte within its 64 KiB stack.
vmm_switch_trace_stack proc
 mov ecx,16
vmm_switch_trace_stack_byte:
 movzx esi,ax
 mov dl,[ebx+esi]
 mov [edi],dl
 inc edi
 inc ax
 loop vmm_switch_trace_stack_byte
 ret
vmm_switch_trace_stack endp
endif
