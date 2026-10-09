; Boot-device PIO adapter in CVSESSION's extended-memory module. No kernel
; growth, no DOS calls. Jemm nested execution serializes every firmware INT13.
; EDD 1.1 packets/DPTE and the pinned Jemm JEMM32.ASM are the ABI references.
cvata_prepare proto c :dword
cvata_observe_control proto c :dword,:dword
cvata_bind proto c :dword
cvata_transfer proto c :dword,:dword,:dword
cvata_get_status proto c
cvata_discovery_begin proto c :dword
cvata_discovery_io proto c :dword,:dword,:dword
cvata_discovery_end proto c :dword
cvata_discovery_cancel proto c

.data
disk_previous_int13 dd 0
disk_hooked dd 0
disk_trapped_port dd 0
disk_discovery_traps dd 0
disk_discovery_ports dw 1F2h,1F3h,1F4h,1F5h,1F6h,1F7h,3F6h
                     dw 172h,173h,174h,175h,176h,177h,376h
align 4
disk_drive dd 0FFFFFFFFh
disk_firmware_active dd 0
disk_busy dd 0
disk_saved_esp dd 0
disk_args dd 3 dup (0)
disk_last_bios_error db 0
align 4
disk_stack db 8192 dup (0)
disk_stack_end label byte

.code
; DllMain calibrates this against PIT2 before any V86 API can bind a disk.
; Zero means calibration was not established; C must refuse native binding.
; P6 TSC is not invariant across firmware-controlled frequency changes.
cvata_clock_khz proc c public
 mov eax,vmm_tsc_measured
 ret
cvata_clock_khz endp

; ATA data phase only: DRQ and the exact 512-byte buffer are validated by C
; and the INT13 adapter. A single bounded string transfer avoids 256 ABI
; calls/VM exits per sector. Ring-0 OUT/IN bypass the V86 control observer.
; cdecl nonvolatile registers, segment selectors and all input FLAGS survive.
cvata_pio_read proc c public port:dword, data_buffer:dword
 pushfd
 push edi
 push edx
 push es
 mov ax,ss
 mov es,ax
 mov edi,data_buffer
 mov edx,port
 mov ecx,256
 cld
 rep insw
 pop es
 pop edx
 pop edi
 popfd
 ret
cvata_pio_read endp

cvata_pio_write proc c public port:dword, data_buffer:dword
 pushfd
 push esi
 push edx
 push ds
 mov ax,ss
 mov ds,ax
 mov esi,data_buffer
 mov edx,port
 mov ecx,256
 cld
 rep outsw
 pop ds
 pop edx
 pop esi
 popfd
 ret
cvata_pio_write endp

; Dedicated stack: firmware runs outside this trampoline, so its observed
; control OUT can safely enter C. Native ring-0 OUT does not hit a V86 trap.
disk_call proc uses ebx esi edi
 cmp disk_busy,0
 jne busy
 pushfd
 cli
 mov disk_busy,1
 push ds
 push es
 mov ebx,ss
 mov ds,ebx
 mov es,ebx
 mov disk_saved_esp,esp
 mov esp,offset disk_stack_end
 cld
 mov edx,ecx
args_loop:
 test edx,edx
 jz args_done
 dec edx
 push [disk_args+edx*4]
 jmp args_loop
args_done:
 call eax
 mov esp,disk_saved_esp
 pop es
 pop ds
 mov disk_busy,0
 popfd
 ret
busy:
 mov eax,4
 ret
disk_call endp

disk_status proc
 mov eax,offset cvata_get_status
 xor ecx,ecx
 call disk_call
 ret
disk_status endp

; EDI linear, ECX size: mapped writable V86 RAM below A0000, including DOS
; metadata buffers below 10000. All buffer accesses are CPU/page translated.
disk_guest_range proc uses eax edx esi
 cmp edi,1000h
 jb bad
 test ecx,ecx
 jz bad
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
 clc
 ret
bad:
 stc
 ret
disk_guest_range endp

; Observe only actual firmware/VM writes, then perform exactly that I/O.
disk_control_port proc
 pushad
 test ecx,OUTPUT
 jz observed
 mov disk_args,eax
 mov eax,1
 test ecx,WORD_IO
 jz not_word
 mov eax,2
not_word:
 test ecx,DWORD_IO
 jz not_dword
 mov eax,4
not_dword:
 test ecx,STRING_IO
 jz width_done
 xor eax,eax                          ; unknown string output invalidates
width_done:
 mov [disk_args+4],eax
 mov eax,offset cvata_observe_control
 mov ecx,2
 call disk_call
observed:
 popad
 @VMMCall Simulate_IO
 ret
disk_control_port endp

; Only VM0 firmware OUTs provide identity evidence. Neither guest game
; traffic nor native ring-0 I/O participates in discovery. Forward all
; input/output instructions exactly once, with their original registers.
disk_discovery_port proc
 pushad
 cmp vmm_current,0
 jne observed
 cmp disk_firmware_active,0
 je observed
 test ecx,OUTPUT
 jz observed
 mov disk_args,edx
 mov [disk_args+4],eax
 mov eax,1
 test ecx,WORD_IO OR DWORD_IO OR STRING_IO
 jz width_done
 xor eax,eax
width_done:
 mov [disk_args+8],eax
 mov eax,offset cvata_discovery_io
 mov ecx,3
 call disk_call
observed:
 popad
 @VMMCall Simulate_IO
 ret
disk_discovery_port endp

; Keep the count until each removal succeeds. A partially removed callback
; set remains owned by this module and blocks unload/restarting discovery.
disk_discovery_remove proc uses ebx edx
again:
 mov ebx,disk_discovery_traps
 test ebx,ebx
 jz complete
 dec ebx
 movzx edx,word ptr disk_discovery_ports[ebx*2]
 pushad
 @VMMCall Remove_IO_Handler
 popad
 jc failed
 dec disk_discovery_traps
 jmp again
complete:
 clc
 ret
failed:
 stc
 ret
disk_discovery_remove endp

disk_dispatch proc uses esi edi ebx
 cmp eax,VM_OP_DISK_STATUS
 je query
 cmp vmm_current,0
 jne bad_context
 cmp disk_firmware_active,0
 jne bad_context
 cmp eax,VM_OP_DISK_BIND
 je bind_disk
 cmp eax,VM_OP_DISK_DISCOVER_BEGIN
 je discover_begin
 cmp eax,VM_OP_DISK_DISCOVER_END
 je discover_end
 mov ecx,64
 call guest_destination
 test eax,eax
 jnz done
 movzx eax,byte ptr [edi+6]
 cmp eax,disk_drive
 jne bad_context
 mov disk_args,edi
 mov eax,offset cvata_prepare
 mov ecx,1
 call disk_call
 test eax,eax
 jnz done
select_control:
 call disk_status
 mov edx,[eax+16]                      ; exact validated control port
 cmp edx,disk_trapped_port
 je ok
 cmp disk_trapped_port,0
 je install
 push edx
 mov edx,disk_trapped_port
 @VMMCall Remove_IO_Handler
 pop edx
 jc trap_error
 mov disk_trapped_port,0
install:
 mov esi,offset disk_control_port
 push edx
 @VMMCall Install_IO_Handler
 pop edx
 jc trap_error
 mov disk_trapped_port,edx
 jmp ok
discover_begin:
 cmp disk_discovery_traps,0
 jne bad_context
 cmp disk_trapped_port,0
 jne bad_context
 mov ecx,64
 call guest_destination
 test eax,eax
 jnz done
 movzx eax,byte ptr [edi+6]
 cmp eax,disk_drive
 jne bad_context
 mov disk_args,edi
 mov eax,offset cvata_discovery_begin
 mov ecx,1
 call disk_call
 test eax,eax
 jnz done
discover_install:
 mov ebx,disk_discovery_traps
 cmp ebx,14
 je ok
 movzx edx,word ptr disk_discovery_ports[ebx*2]
 mov esi,offset disk_discovery_port
 pushad
 @VMMCall Install_IO_Handler
 popad
 jc discovery_install_error
 inc disk_discovery_traps
 jmp discover_install
discovery_install_error:
 call disk_discovery_remove
 mov eax,offset cvata_discovery_cancel
 xor ecx,ecx
 call disk_call
 jmp trap_error
discover_end:
 cmp word ptr [ebp].Client_Reg_Struc.Client_ECX,0
 jz discover_abort
 cmp disk_discovery_traps,14
 jne bad_context
 mov ecx,64
 call guest_destination
 test eax,eax
 jnz done
 mov disk_args,edi
 mov eax,offset cvata_discovery_end
 mov ecx,1
 call disk_call
 push eax
 call disk_discovery_remove
 pop eax
 jc trap_error
 test eax,eax
 jnz done
 jmp select_control
discover_abort:
 call disk_discovery_remove
 jc trap_error
 mov eax,offset cvata_discovery_cancel
 xor ecx,ecx
 call disk_call
 jmp ok
bind_disk:
 mov ecx,512
 call guest_destination
 test eax,eax
 jnz done
 mov disk_args,edi
 mov eax,offset cvata_bind
 mov ecx,1
 call disk_call
 ret
query:
 cmp disk_hooked,0
 jne disk_hook_ready
 cmp vmm_current,0
 jne bad_context
 mov eax,13h
 mov esi,offset disk_interrupt
 @VMMCall Hook_V86_Int_Chain
 jc trap_error
 mov disk_hooked,1
disk_hook_ready:
 cmp vmm_ready,0
 je bad_context
 mov eax,vmm_kbase
 movzx eax,byte ptr [eax+KL_BOOT_DRIVE]
 mov disk_drive,eax
 mov word ptr [ebp].Client_Reg_Struc.Client_EBX,ax
 mov ecx,VM_DISK_STATUS_SIZE
 call guest_destination
 test eax,eax
 jnz done
 push edi
 call disk_status
 pop edi
 mov esi,eax
 mov dword ptr [edi],31534443h          ; CDS1
 mov dword ptr [edi+4],00500001h
 add edi,8
 mov ecx,18
 cld
 rep movsd
ok:
 xor eax,eax
done:
 ret
bad_context:
 mov eax,VM_ERROR_CONTEXT
 ret
trap_error:
 mov eax,VM_ERROR_TRAP
 ret
disk_dispatch endp

; Carry set means chain, clear means the client's CF/AH already describe
; the result. Recursive firmware INT13 stays inside the same guard.
align 4
 dd offset disk_previous_int13
disk_interrupt proc
 cmp disk_firmware_active,0
 jne chain
 pushad
 call disk_status
 mov esi,eax
 cmp dword ptr [esi+4],3              ; an uncertain issued command owns
 je quarantined                       ; the whole channel until reboot
 movzx eax,byte ptr [ebp].Client_Reg_Struc.Client_EDX
 cmp eax,disk_drive
 jne firmware
 cmp dword ptr [esi],1
 jne firmware
 mov al,byte ptr [ebp].Client_Reg_Struc.Client_EAX+1
 cmp al,42h
 je native
 cmp al,43h
 je native
 cmp al,2
 je native
 cmp al,3
 je native
 cmp al,0
 je reset_status
 cmp al,1
 je previous_status
 ; Metadata/geometry queries remain BIOS-owned, under serialization.
firmware:
 inc disk_firmware_active
 @VMMCall Begin_Nest_Exec
 mov eax,13h
 @VMMCall Exec_Int
 @VMMCall End_Nest_Exec
 dec disk_firmware_active
 popad
 clc
 ret
native:
 call disk_native_transfer
 jmp return_status
quarantined:
 mov eax,80h
 jmp return_status
reset_status:
 xor eax,eax                          ; never issue a BIOS reset as retry
 jmp return_status
previous_status:
 movzx eax,disk_last_bios_error
return_status:
 mov disk_last_bios_error,al
 mov byte ptr [ebp].Client_Reg_Struc.Client_EAX+1,al
 and [ebp].Client_Reg_Struc.Client_EFlags,not 1
 test al,al
 jz returned
 or [ebp].Client_Reg_Struc.Client_EFlags,1
returned:
 popad
 clc
 ret
chain:
 stc
 ret
disk_interrupt endp

; ESI status. Locals are on the Jemm stack and survive the private C stack.
; Validate the entire guest range before issuing anything; process one
; sector at a time, reporting partial progress in DAP count / CHS AL.
disk_native_transfer proc uses ebx esi edi
 sub esp,24
 ; [0] DAP or0, [4] buffer, [8] LBA, [12] requested, [16] done, [20] write
 xor eax,eax
 mov [esp],eax
 mov [esp+16],eax
 mov al,byte ptr [ebp].Client_Reg_Struc.Client_EAX+1
 and eax,1
 mov [esp+20],eax
 cmp byte ptr [ebp].Client_Reg_Struc.Client_EAX+1,40h
 jb chs
 movzx edi,word ptr [ebp].Client_Reg_Struc.Client_ESI
 cmp edi,0FFF0h
 ja invalid
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_DS
 shl eax,4
 add edi,eax
 mov ecx,16
 call disk_guest_range
 jc invalid
 cmp byte ptr [edi],10h
 jb invalid
 cmp byte ptr [edi+1],0
 jne invalid
 cmp dword ptr [edi+12],0
 jne invalid
 cmp byte ptr [ebp].Client_Reg_Struc.Client_EAX+1,43h
 jne edd_flags_ok
 ; AH43 AL bit0 is write-and-verify: unsupported, reject before transfer.
 cmp byte ptr [ebp].Client_Reg_Struc.Client_EAX,0
 jne invalid
edd_flags_ok:
 mov [esp],edi
 mov eax,[edi+8]
 mov [esp+8],eax
 movzx ecx,word ptr [edi+2]
 mov word ptr [edi+2],0
 mov [esp+12],ecx
 movzx ebx,word ptr [edi+4]
 movzx eax,word ptr [edi+6]
 shl eax,4
 add eax,ebx
 mov [esp+4],eax
 jmp validate
chs:
 movzx ecx,byte ptr [ebp].Client_Reg_Struc.Client_EAX
 mov [esp+12],ecx
 mov byte ptr [ebp].Client_Reg_Struc.Client_EAX,0
 movzx ebx,word ptr [ebp].Client_Reg_Struc.Client_EBX
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_ES
 shl eax,4
 add eax,ebx
 mov [esp+4],eax
 movzx eax,byte ptr [ebp].Client_Reg_Struc.Client_ECX+1
 movzx edx,byte ptr [ebp].Client_Reg_Struc.Client_ECX
 and edx,0C0h
 shl edx,2
 or eax,edx
 cmp eax,[esi+68]
 jae invalid
 mul dword ptr [esi+60]
 movzx edx,byte ptr [ebp].Client_Reg_Struc.Client_EDX+1
 cmp edx,[esi+60]
 jae invalid
 add eax,edx
 mul dword ptr [esi+64]
 movzx edx,byte ptr [ebp].Client_Reg_Struc.Client_ECX
 and edx,3Fh
 jz invalid
 cmp edx,[esi+64]
 ja invalid
 dec edx
 add eax,edx
 mov [esp+8],eax
validate:
 mov ecx,[esp+12]
 test ecx,ecx
 jz invalid
 cmp ecx,127
 ja invalid
 mov eax,[esp+8]
 add eax,ecx
 jc invalid
 cmp eax,[esi+56]
 ja range_error
 shl ecx,9
 add ebx,ecx
 cmp ebx,10000h
 ja invalid
 mov edi,[esp+4]
 call disk_guest_range
 jc invalid
transfer:
 mov eax,[esp+20]
 mov disk_args,eax
 mov eax,[esp+8]
 mov [disk_args+4],eax
 mov eax,[esp+4]
 mov [disk_args+8],eax
 mov eax,offset cvata_transfer
 mov ecx,3
 call disk_call
 test eax,eax
 jnz native_error
 inc dword ptr [esp+16]
 inc dword ptr [esp+8]
 add dword ptr [esp+4],512
 mov eax,[esp+16]
 cmp eax,[esp+12]
 jb transfer
 xor eax,eax
 jmp finish
native_error:
 cmp eax,7
 je timeout
 cmp eax,8
 je timeout
 cmp eax,9
 je range_error
 mov eax,20h
 jmp finish
timeout:
 mov eax,80h
 jmp finish
range_error:
 mov eax,4
 jmp finish
invalid:
 mov eax,1
finish:
 mov edx,[esp+16]
 mov edi,[esp]
 test edi,edi
 jz chs_result
 mov [edi+2],dx
 jmp done
chs_result:
 cmp byte ptr [ebp].Client_Reg_Struc.Client_EAX+1,40h
 jae done
 mov byte ptr [ebp].Client_Reg_Struc.Client_EAX,dl
done:
 add esp,24
 ret
disk_native_transfer endp
