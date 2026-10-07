; CiukiOS desktop framebuffer service for CVSESSION (system VM).
; Included by session_jlm.asm.
;
; With the VM manager resident the desktop runs in V86 and cannot use its
; linear-framebuffer bursts (they switch CR0). It draws through the VBE bank
; window at A000 instead, and every firmware bank switch costs a guest memory
; remap (about 6 ms under KVM). While no DOS-window session owns INT 10h this
; hook therefore:
;   4F01  records which mode's information the caller's buffer receives
;         (the firmware fills it; a nested INT 10h here faults the monitor);
;   4F02  reads that filled buffer for the mode being set: a banked set of a
;         packed/direct mode with window A at A000 and a PhysBasePtr arms
;         the mapping; anything else disarms it;
;   AH=00 disarms (VGA/text mode for a full-screen program);
;   4F05  (window A, set/get) maps the 16 PTEs A0000-AFFFF straight onto the
;         framebuffer at the selected bank and answers without the firmware.
; Disarming restores the identity PTEs, so a full-screen program (and any
; VCPI client copying page table 0) sees the real VGA window. A DOS-window
; session saves and restores these PTEs itself; desk_resync then follows the
; bank its host mode selected.

DESK_SLOTS equ 8

.data
desk_hooked dd 0
desk_query_mode dd DESK_SLOTS dup (-1)  ; 4F01 mode per recorded buffer
desk_query_addr dd DESK_SLOTS dup (-1)  ; linear address of that buffer
desk_next dd 0
desk_phys dd 0                          ; armed mode: PhysBasePtr
desk_gran dd 0                          ;             granularity in bytes
desk_armed dd -1                        ; armed VBE mode, -1 when disarmed
desk_mapped dd 0
desk_offset dd 0
desk_saved dd 16 dup (0)
desk_previous dd 0

.code

; Identity window back (TLB flushed). Preserves registers.
desk_unmap proc
 cmp desk_mapped,0
 je unmap_done
 pushad
 mov esi,offset desk_saved
 mov edi,PAGE_MAP + 0A0h*4
 mov ecx,16
 cld
 rep movsd
 mov eax,cr3
 mov cr3,eax
 mov desk_mapped,0
 popad
unmap_done:
 ret
desk_unmap endp

; Window A0000-AFFFF onto the armed framebuffer at desk_offset.
desk_map proc
 pushad
 cmp desk_armed,-1
 je map_done
 cmp desk_mapped,0
 jne map_pages
 mov esi,PAGE_MAP + 0A0h*4
 mov edi,offset desk_saved
 mov ecx,16
 cld
 rep movsd
map_pages:
 mov eax,desk_phys
 add eax,desk_offset
 and eax,0FFFFF000h
 or eax,7
 mov edi,PAGE_MAP + 0A0h*4
 mov ecx,16
map_next:
 mov [edi],eax
 add eax,1000h
 add edi,4
 loop map_next
 mov eax,cr3
 mov cr3,eax
 mov desk_mapped,1
map_done:
 popad
 ret
desk_map endp

; After a DOS-window session restored the aperture PTEs: follow the bank its
; host mode left selected (the desktop's bank cache holds that one).
desk_resync proc
 cmp vmm_current,0
 jne resync_done
 cmp desk_mapped,0
 je resync_done
 pushad
 call cvvid_host_offset
 mov desk_offset,eax
 popad
 call desk_map
resync_done:
 ret
desk_resync endp

; Hooks require a pointer to their saved-chain variable at entry-4.
align 4
 dd offset desk_previous
desktop_interrupt proc
 cmp active,0                           ; a window session owns INT 10h
 jne desk_chain
 pushad
 call check_context
 test eax,eax
 jnz desk_pass
 mov eax,[ebp].Client_Reg_Struc.Client_EAX
 test ah,ah
 jz desk_vga_mode
 cmp ax,4F01h
 je desk_query
 cmp ax,4F02h
 je desk_set
 cmp ax,4F05h
 je desk_bank
desk_pass:
 popad
desk_chain:
 stc
 ret
desk_vga_mode:
 call desk_unmap
 mov desk_armed,-1
 jmp desk_pass

desk_set:
 call desk_unmap
 mov desk_armed,-1
 mov desk_offset,0
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_EBX
 test ecx,4000h                         ; a linear set: the owner draws itself
 jnz desk_pass
 and ecx,1FFh
 xor ebx,ebx
set_find:
 cmp desk_query_mode[ebx*4],ecx
 je set_found
 inc ebx
 cmp ebx,DESK_SLOTS
 jb set_find
 jmp desk_pass
set_found:
 mov esi,desk_query_addr[ebx*4]
 mov ax,[esi]                           ; supported, graphics, linear
 and ax,0099h
 cmp ax,0099h
 jne desk_pass
 mov al,[esi+2]                         ; window A readable and writable
 and al,7
 cmp al,7
 jne desk_pass
 cmp word ptr [esi+6],64                ; 64 KiB window
 jne desk_pass
 cmp word ptr [esi+8],0A000h
 jne desk_pass
 mov al,[esi+27]                        ; packed pixel or direct colour
 cmp al,4
 je set_model
 cmp al,6
 jne desk_pass
set_model:
 movzx eax,word ptr [esi+4]             ; granularity in KiB: 4..64, power of 2
 cmp eax,4
 jb desk_pass
 cmp eax,64
 ja desk_pass
 lea edx,[eax-1]
 test eax,edx
 jnz desk_pass
 shl eax,10
 mov edx,[esi+40]
 test edx,0FFFh
 jnz desk_pass
 test edx,edx
 jz desk_pass
 ; PhysBasePtr only describes linear mode. A banked mode can bypass its
 ; hardware window solely for a documented device/BAR alias; use the same
 ; allowlist as CVSESSION binding, not CPUID or a fixed PCI bus/device.
 pushad
 mov dev_args,edx
 mov eax,offset cvlegacy_vbe_alias
 mov ecx,1
 call dev_call
 cmp eax,1                             ; a busy/reentered trampoline is not proof
 popad
 jne desk_pass
 mov desk_gran,eax
 mov desk_phys,edx
 mov desk_armed,ecx
 jmp desk_pass

desk_query:
 movzx esi,word ptr [ebp].Client_Reg_Struc.Client_ES
 shl esi,4
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EDI
 add esi,eax
 cmp esi,100000h-256
 ja desk_pass
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_ECX
 and ecx,1FFh
 xor ebx,ebx
query_find:                             ; this buffer now describes this mode
 cmp desk_query_addr[ebx*4],esi
 je query_store
 inc ebx
 cmp ebx,DESK_SLOTS
 jb query_find
 mov ebx,desk_next
 inc desk_next
 and desk_next,DESK_SLOTS-1
query_store:
 mov desk_query_addr[ebx*4],esi
 mov desk_query_mode[ebx*4],ecx
 jmp desk_pass

desk_bank:
 cmp desk_armed,-1
 je desk_pass
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 test al,al                             ; window A only
 jnz desk_pass
 test ah,ah
 jz bank_set
 cmp ah,1
 jne desk_pass
 mov eax,desk_offset                    ; get: DX = window position
 xor edx,edx
 div desk_gran
 mov word ptr [ebp].Client_Reg_Struc.Client_EDX,ax
 jmp bank_ok
bank_set:
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EDX
 mul desk_gran
 test edx,edx
 jnz desk_pass
 cmp eax,1000000h                       ; stay inside a 16 MiB aperture
 jae desk_pass
 mov desk_offset,eax
 call desk_map
bank_ok:
 mov word ptr [ebp].Client_Reg_Struc.Client_EAX,004Fh
 popad
 clc
 ret
desktop_interrupt endp

; DllMain attach/detach.
desk_install proc
 cmp desk_hooked,0
 jne install_done
 mov eax,10h
 mov esi,offset desktop_interrupt
 @VMMCall Hook_V86_Int_Chain
 jc install_done
 mov desk_hooked,1
install_done:
 ret
desk_install endp

desk_remove proc
 call desk_unmap
 cmp desk_hooked,0
 je remove_done
 mov eax,10h
 mov esi,offset desktop_interrupt
 @VMMCall Unhook_V86_Int_Chain
 jc remove_failed
 mov desk_hooked,0
remove_done:
 clc
 ret
remove_failed:
 stc
 ret
desk_remove endp
