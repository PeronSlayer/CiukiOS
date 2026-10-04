; Synchronous CN32 process address space. All Jemm-owned allocations are
; supervisor mappings in the owner CR3. The process gets a private directory:
; inherited PDEs have U/S cleared, and only one newly owned PT has user PTEs.
; The gate in native_entry.asm owns the CPL3 transition, IDT and TSS.
include native_entry_abi.inc
NPROC_OWNER_PD equ 0F8001000h          ; pinned JLOAD ?PAGEDIR
NPROC_USER_PDE equ 100h               ; 40000000h..403FFFFFh
NPROC_CODE equ 40001000h
NPROC_DATA equ 40020000h
NPROC_STACK equ 40040000h
NPROC_MAX_IMAGE equ 61440
NPROC_MAX_STACK equ 65536
NPROC_CTX_BYTES equ 60
NPROC_MAX_SLOTS equ 4
NPROC_SLOT_FREE equ 0
NPROC_SLOT_READY equ 1
NPROC_SLOT_RUNNING equ 2
NPROC_SLOT_ENDED equ 3
NPROC_SLOT_CLEANUP equ 4
NPROC_SLOT_STATE equ 0
NPROC_SLOT_GENERATION equ 4
NPROC_SLOT_OWNER equ 8
NPROC_SLOT_RESULT equ 12
NPROC_SLOT_REPORTS equ 16
NPROC_SLOT_TAG equ 20
NPROC_SLOT_VALUE equ 24
NPROC_SLOT_STEPS equ 28
NPROC_SLOT_VECTOR equ 32
NPROC_SLOT_ERROR equ 36
NPROC_SLOT_EXIT equ 40
NPROC_SLOT_CONTEXT equ 44
NPROC_SLOT_BYTES equ NPROC_SLOT_CONTEXT+NATIVE_CONT_BYTES

.data
nproc_busy dd 0
nproc_serial dd 0
nproc_owner dd 0
nproc_live_before dd 0
nproc_source dd 0
nproc_length dd 0
nproc_code_bytes dd 0
nproc_data_bytes dd 0
nproc_stack_bytes dd 0
nproc_code_pages dd 0
nproc_data_pages dd 0
nproc_stack_pages dd 0
nproc_entry_offset dd 0
nproc_pd_linear dd 0
nproc_pt_linear dd 0
nproc_code_linear dd 0
nproc_data_linear dd 0
nproc_stack_linear dd 0
nproc_cr3 dd 0
nproc_result dd 0
nproc_last_tag dd 0
nproc_last_value dd 0
nproc_report_count dd 0
nproc_context dd NPROC_CTX_BYTES/4 dup (0)
align 4
nproc_slots db NPROC_SLOT_BYTES*NPROC_MAX_SLOTS dup (0)
nproc_next dd 0
nproc_last_tick dd -1
nproc_candidate_index dd 0

.code
extrn native_entry_run@0:near
extrn native_entry_step@0:near

; EAX = supervisor linear page, returns EAX = physical page or zero.
nproc_physical_page proc uses edx
 test eax,0FFFh
 jnz invalid
 shr eax,12
 mov eax,[PAGE_MAP+eax*4]
 test eax,1
 jz invalid
 and eax,0FFFFF000h
 ret
invalid:
 xor eax,eax
 ret
nproc_physical_page endp

; Validate every byte's backing page before touching the V86 source. A
; source entirely within one 16-bit segment also prevents segment wrap.
; EBP = Jemm client frame. On success retains source and image lengths.
nproc_validate proc uses ebx ecx edx esi edi
 mov ecx,[ebp].Client_Reg_Struc.Client_ECX
 cmp ecx,36
 jb invalid
 cmp ecx,NPROC_MAX_IMAGE
 ja invalid
 mov nproc_length,ecx
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_ESI
 mov edx,eax
 add edx,ecx
 cmp edx,10000h
 ja invalid
 movzx edx,word ptr [ebp].Client_Reg_Struc.Client_DS
 shl edx,4
 add eax,edx
 cmp eax,10000h
 jb invalid
 mov nproc_source,eax
 mov edx,eax
 add edx,ecx
 jc invalid
 cmp edx,0A0000h
 ja invalid
 dec edx
 shr edx,12
 mov esi,eax
 shr esi,12
page_check:
 mov ebx,[PAGE_MAP+esi*4]
 and ebx,5
 cmp ebx,5
 jne invalid
 inc esi
 cmp esi,edx
 jbe page_check
 mov edi,nproc_source
 cmp dword ptr [edi],32334E43h       ; 'CN32'
 jne bad_abi
 cmp word ptr [edi+4],1
 jne bad_abi
 cmp word ptr [edi+6],36
 jne bad_abi
 cmp dword ptr [edi+8],1
 jne bad_abi
 mov eax,[edi+16]
 test eax,eax
 jz bad_abi
 cmp eax,NPROC_MAX_IMAGE-36
 ja bad_abi
 mov nproc_code_bytes,eax
 mov ebx,[edi+20]
 mov nproc_data_bytes,ebx
 add eax,ebx
 jc bad_abi
 cmp eax,[edi+28]
 jne bad_abi
 add eax,36
 jc bad_abi
 cmp eax,nproc_length
 jne bad_abi
 mov eax,[edi+12]
 cmp eax,nproc_code_bytes
 jae bad_abi
 mov nproc_entry_offset,eax
 mov eax,[edi+24]
 test eax,eax
 jz bad_abi
 cmp eax,NPROC_MAX_STACK
 ja bad_abi
 mov nproc_stack_bytes,eax
 ; CRC-32/IEEE, bit-reflected polynomial, over code followed by data.
 lea esi,[edi+36]
 mov ecx,[edi+28]
 mov eax,0FFFFFFFFh
crc_byte:
 movzx ebx,byte ptr [esi]
 xor eax,ebx
 mov edx,8
crc_bit:
 shr eax,1
 jnc crc_next
 xor eax,0EDB88320h
crc_next:
 dec edx
 jnz crc_bit
 inc esi
 dec ecx
 jnz crc_byte
 not eax
 cmp eax,[edi+32]
 jne bad_abi
 xor eax,eax
 ret
invalid:
 mov eax,VM_ERROR_ADDRESS
 ret
bad_abi:
 mov eax,VM_ERROR_ABI
 ret
nproc_validate endp

; Map each committed supervisor page into one private user PT. ESI is the
; supervisor base, EDI the first PTE within the private PT, ECX page count,
; EBX PTE attributes (5 code, 7 data/stack). Returns EAX=0 or mapping error.
nproc_map_pages proc uses ebx ecx edx esi edi
 test ecx,ecx
 jz all_mapped
next_page:
 mov eax,esi
 call nproc_physical_page
 test eax,eax
 jz failed
 or eax,ebx
 mov edx,nproc_pt_linear
 mov [edx+edi*4],eax
 add esi,1000h
 inc edi
 dec ecx
 jnz next_page
all_mapped:
 xor eax,eax
 ret
failed:
 mov eax,VM_ERROR_MAPPING
 ret
nproc_map_pages endp

; Release precisely the pages tagged to this process, including failure
; paths. A failed Jemm free stays recorded and blocks DLL unload for retry.
nproc_release proc uses ebx
 mov ebx,nproc_owner
 test ebx,ebx
 jz done
 call native_page_release_owner
 cmp eax,-1
 je failure
 mov eax,native_page_live
 cmp eax,nproc_live_before
 jne failure
 mov nproc_owner,0
done:
 xor eax,eax
 ret
failure:
 mov eax,VM_ERROR_MEMORY
 ret
nproc_release endp

; Internal setup: private PD, private PT, code, data, stack and context.
; The user regions are separated by unmapped guard pages. Code is read-only.
nproc_setup proc uses ebx ecx edx esi edi
 mov eax,nproc_code_bytes
 add eax,0FFFh
 shr eax,12
 mov nproc_code_pages,eax
 mov eax,nproc_data_bytes
 add eax,0FFFh
 shr eax,12
 mov nproc_data_pages,eax
 mov eax,nproc_stack_bytes
 add eax,0FFFh
 shr eax,12
 mov nproc_stack_pages,eax
 mov ebx,nproc_owner
 mov ecx,1
 call native_page_alloc
 test eax,eax
 jz no_memory
 mov nproc_pd_linear,edx
 mov ebx,nproc_owner
 mov ecx,1
 call native_page_alloc
 test eax,eax
 jz no_memory
 mov nproc_pt_linear,edx
 mov ebx,nproc_owner
 mov ecx,nproc_code_pages
 call native_page_alloc
 test eax,eax
 jz no_memory
 mov nproc_code_linear,edx
 mov ecx,nproc_data_pages
 test ecx,ecx
 jz data_done
 mov ebx,nproc_owner
 call native_page_alloc
 test eax,eax
 jz no_memory
 mov nproc_data_linear,edx
data_done:
 mov ebx,nproc_owner
 mov ecx,nproc_stack_pages
 call native_page_alloc
 test eax,eax
 jz no_memory
 mov nproc_stack_linear,edx
 ; Copy from the validated guest buffer before entering ring 3. The image
 ; remains private even if its DOS source is modified after this call.
 mov esi,nproc_source
 add esi,36
 mov edi,nproc_code_linear
 mov ecx,nproc_code_bytes
 rep movsb
 mov edi,nproc_data_linear
 test edi,edi
 jz no_data_copy
 mov ecx,nproc_data_bytes
 rep movsb
no_data_copy:
 ; PDE 256 must be vacant in the Jemm owner map. Copy every inherited PDE
 ; with U/S=0, including low DOS pages; only the new PT exposes user pages.
 mov esi,NPROC_OWNER_PD
 cmp dword ptr [esi+NPROC_USER_PDE*4],0
 jne mapping_error
 mov esi,NPROC_OWNER_PD
 mov edi,nproc_pd_linear
 mov ecx,1024
copy_pde:
 lodsd
 and eax,0FFFFFFFBh
 stosd
 dec ecx
 jnz copy_pde
 mov eax,nproc_pt_linear
 call nproc_physical_page
 test eax,eax
 jz mapping_error
 or eax,7
 mov edi,nproc_pd_linear
 mov [edi+NPROC_USER_PDE*4],eax
 mov esi,nproc_code_linear
 mov edi,(NPROC_CODE shr 12) and 3FFh
 mov ecx,nproc_code_pages
 mov ebx,5
 call nproc_map_pages
 test eax,eax
 jnz done
 mov esi,nproc_data_linear
 mov edi,(NPROC_DATA shr 12) and 3FFh
 mov ecx,nproc_data_pages
 mov ebx,7
 call nproc_map_pages
 test eax,eax
 jnz done
 mov esi,nproc_stack_linear
 mov edi,(NPROC_STACK shr 12) and 3FFh
 mov ecx,nproc_stack_pages
 mov ebx,7
 call nproc_map_pages
 test eax,eax
 jnz done
 mov eax,nproc_pd_linear
 call nproc_physical_page
 test eax,eax
 jz mapping_error
 mov nproc_cr3,eax
 mov [nproc_context+0],eax
 mov eax,NPROC_CODE
 add eax,nproc_entry_offset
 mov [nproc_context+4],eax
 mov eax,nproc_stack_pages
 shl eax,12
 add eax,NPROC_STACK
 mov [nproc_context+8],eax
 mov dword ptr [nproc_context+12],NPROC_DATA
 mov eax,nproc_data_bytes
 mov [nproc_context+16],eax
 mov dword ptr [nproc_context+20],256
 mov edi,offset nproc_context+24
 mov ecx,(NPROC_CTX_BYTES-24)/4
 xor eax,eax
 rep stosd
 xor eax,eax
done:
 ret
mapping_error:
 mov eax,VM_ERROR_MAPPING
 ret
no_memory:
 mov eax,VM_ERROR_MEMORY
 ret
nproc_setup endp

; V86 service. EAX=VM_ERROR_* on failure; successful native exit returns
; EAX=0..4 (gate result) with report registers published in the client frame.
nproc_run proc uses ebx ecx edx esi edi
 cmp vmm_current,0
 jne forbidden
 cmp nproc_busy,0
 jne busy
 ; A previous _PageFree failure keeps its owner records. Reclaim those
 ; records before allocating a new ID, so a retry can never orphan them.
 cmp nproc_owner,0
 je owner_clear
 call nproc_release
 test eax,eax
 jnz pending_cleanup
owner_clear:
 mov nproc_busy,1
 call nproc_validate
 test eax,eax
 jnz reset_busy
 mov eax,native_page_live
 mov nproc_live_before,eax
 mov eax,nproc_serial
 inc eax
 and eax,00FFFFFFh
 jnz serial_ready
 mov eax,1
serial_ready:
 mov nproc_serial,eax
 or eax,4E000000h
 mov nproc_owner,eax
 call nproc_setup
 test eax,eax
 jnz release_error
 mov esi,offset nproc_context
 call native_entry_run@0
 mov nproc_result,eax
 mov eax,[nproc_context+40]
 mov nproc_report_count,eax
 mov eax,[nproc_context+44]
 mov nproc_last_tag,eax
 mov eax,[nproc_context+48]
 mov nproc_last_value,eax
 call nproc_release
 test eax,eax
 jnz reset_busy
 mov eax,nproc_last_tag
 mov [ebp].Client_Reg_Struc.Client_EBX,eax
 mov eax,nproc_last_value
 mov [ebp].Client_Reg_Struc.Client_ECX,eax
 mov eax,nproc_report_count
 mov [ebp].Client_Reg_Struc.Client_EDX,eax
 mov eax,[nproc_context+28]
 mov [ebp].Client_Reg_Struc.Client_ESI,eax
 mov eax,[nproc_context+52]
 mov [ebp].Client_Reg_Struc.Client_EDI,eax
 mov eax,nproc_result
 test eax,eax
 jnz publish_fault_error
 mov eax,[nproc_context+56]            ; native exit status, including nonzero
 jmp publish_auxiliary
publish_fault_error:
 mov eax,[nproc_context+32]
publish_auxiliary:
 mov [ebp].Client_Reg_Struc.Client_EBP,eax
 mov eax,nproc_result
 mov [ebp].Client_Reg_Struc.Client_EAX,eax
 mov nproc_busy,0
 clc
 ret
release_error:
 push eax
 call nproc_release
 test eax,eax
 jnz cleanup_failed
 pop eax
reset_busy:
 mov nproc_busy,0
 stc
 ret
cleanup_failed:
 add esp,4
 mov nproc_busy,0
 mov eax,VM_ERROR_MEMORY
 stc
 ret
busy:
 mov eax,VM_ERROR_ACTIVE
 stc
 ret
pending_cleanup:
 mov eax,VM_ERROR_MEMORY
 stc
 ret
forbidden:
 mov eax,VM_ERROR_CONTEXT
 stc
 ret
nproc_run endp

; A generation-tagged process handle is (generation << 8) | (slot+1).
; EBX=handle, returns EAX=0 and ESI=slot or VM_ERROR_ADDRESS.
nproc_slot_for_handle proc uses ebx ecx edx
 movzx edx,bl
 test edx,edx
 jz invalid
 cmp edx,NPROC_MAX_SLOTS
 ja invalid
 mov ecx,ebx
 shr ecx,8
 test ecx,ecx
 jz invalid
 dec edx
 imul edx,NPROC_SLOT_BYTES
 lea esi,[nproc_slots+edx]
 cmp [esi+NPROC_SLOT_GENERATION],ecx
 jne invalid
 cmp dword ptr [esi+NPROC_SLOT_STATE],NPROC_SLOT_FREE
 je invalid
 xor eax,eax
 ret
invalid:
 mov eax,VM_ERROR_ADDRESS
 ret
nproc_slot_for_handle endp

; ESI=retained process slot. Every still-owned page must disappear, including
; after an earlier failed free; leave the owner ID in place for STOP retry.
nproc_release_slot proc uses ebx ecx edx edi
 mov ebx,[esi+NPROC_SLOT_OWNER]
 test ebx,ebx
 jz released
 mov ecx,native_page_live
 xor edx,edx
 mov edi,offset native_page_records
count_owned:
 cmp [edi].NATIVE_PAGE_RECORD.owner,ebx
 jne count_next
 cmp [edi].NATIVE_PAGE_RECORD.linear,0
 je count_next
 inc edx
count_next:
 add edi,sizeof NATIVE_PAGE_RECORD
 cmp edi,offset native_page_records+NATIVE_PAGE_SLOTS*sizeof NATIVE_PAGE_RECORD
 jb count_owned
 push ecx
 push edx
 call native_page_release_owner
 pop edx
 pop ecx
 cmp eax,-1
 je failed
 cmp eax,edx
 jne failed
 sub ecx,edx
 cmp native_page_live,ecx
 jne failed
 mov dword ptr [esi+NPROC_SLOT_OWNER],0
released:
 xor eax,eax
 ret
failed:
 mov eax,VM_ERROR_MEMORY
 ret
nproc_release_slot endp

; Start a retained CN32 process. The V86 source is fully copied before this
; call returns; the caller may close its file and free its DOS block at once.
; EBP=client frame. CF=0/EAX=0/EBX=handle or CF=1/EAX=VM_ERROR_*.
nproc_start proc uses ebx ecx edx esi edi
 cmp vmm_current,0                    ; only the system VM owns native tasks
 jne context_error
 cmp vmm_ready,0                      ; VMM_INIT: kernel InDOS/FAT gates
 je not_initialized
 cmp nproc_busy,0
 jne active_error
 ; Quanta run only from jlm_poll, which the first VMM_CREATE would arm. A
 ; native process started before any DOS VM fork arms it here, and the
 ; canonical API frame lets nproc_poll recognise the return-to-V86 boundary.
 mov vmm_client_frame,ebp
 call vmm_arm
 test eax,eax
 jnz start_error
 cmp nproc_owner,0                    ; retry a prior partial cleanup first
 je scratch_clear
 call nproc_release
 test eax,eax
 jnz memory_error
scratch_clear:
 mov nproc_busy,1
 call nproc_validate
 test eax,eax
 jnz start_error
 mov edi,offset nproc_slots
 xor edx,edx
find_slot:
 mov eax,[edi+NPROC_SLOT_STATE]
 cmp eax,NPROC_SLOT_FREE
 je slot_found
 cmp eax,NPROC_SLOT_ENDED
 je slot_found
 add edi,NPROC_SLOT_BYTES
 inc edx
 cmp edx,NPROC_MAX_SLOTS
 jb find_slot
 mov eax,VM_ERROR_VMM_FULL
 jmp start_error
slot_found:
 mov nproc_candidate_index,edx
 mov eax,native_page_live
 mov nproc_live_before,eax
 mov eax,nproc_serial
 inc eax
 and eax,00FFFFFFh
 jnz nstart_serial_ready
 mov eax,1
nstart_serial_ready:
 mov nproc_serial,eax
 or eax,4E000000h
 mov nproc_owner,eax
 call nproc_setup
 test eax,eax
 jnz start_release_error
 ; Recycle only a terminal slot. The generation change invalidates all old
 ; handles before its old result is overwritten.
 mov ebx,[edi+NPROC_SLOT_GENERATION]
 inc ebx
 and ebx,00FFFFFFh
 jnz nstart_generation_ready
 mov ebx,1
nstart_generation_ready:
 mov esi,edi
 push esi
 mov ecx,NPROC_SLOT_BYTES/4
 xor eax,eax
 rep stosd
 pop esi
 mov [esi+NPROC_SLOT_GENERATION],ebx
 mov eax,nproc_owner
 mov [esi+NPROC_SLOT_OWNER],eax
 mov dword ptr [esi+NPROC_SLOT_STATE],NPROC_SLOT_READY
 push esi
 lea edi,[esi+NPROC_SLOT_CONTEXT]
 mov esi,offset nproc_context
 mov ecx,NPROC_CTX_BYTES/4
 rep movsd
 pop esi
 mov dword ptr [esi+NPROC_SLOT_CONTEXT+NC_MAGIC],NATIVE_CONT_MAGIC
 mov dword ptr [esi+NPROC_SLOT_CONTEXT+NC_STATE],NC_FRESH
 mov nproc_owner,0                   ; ownership is now retained by the slot
 mov nproc_busy,0
 shl ebx,8
 mov edx,nproc_candidate_index
 inc edx
 or ebx,edx
 mov [ebp].Client_Reg_Struc.Client_EBX,ebx
 mov dword ptr [ebp].Client_Reg_Struc.Client_EAX,0
 xor eax,eax
 clc
 ret
start_release_error:
 push eax
 call nproc_release
 test eax,eax
 jnz start_cleanup_failed
 pop eax
start_error:
 mov nproc_busy,0
 stc
 ret
start_cleanup_failed:
 add esp,4
 mov nproc_busy,0
memory_error:
 mov eax,VM_ERROR_MEMORY
 stc
 ret
active_error:
 mov eax,VM_ERROR_ACTIVE
 stc
 ret
not_initialized:
 mov eax,VM_ERROR_VMM_INIT
 stc
 ret
context_error:
 mov eax,VM_ERROR_CONTEXT
 stc
 ret
nproc_start endp

; Query a retained process by handle in EBX. Terminal result 0 is normal;
; 1/3 are fault/setup error and 5 is explicit STOP. Ended records persist
; until their slot is reused, when the generation changes.
nproc_state proc uses ebx ecx edx esi edi
 cmp vmm_current,0
 jne context_error
 mov ebx,[ebp].Client_Reg_Struc.Client_EBX
 call nproc_slot_for_handle
 test eax,eax
 jnz failed
 mov eax,[esi+NPROC_SLOT_STATE]
 mov [ebp].Client_Reg_Struc.Client_EAX,eax
 mov eax,[esi+NPROC_SLOT_TAG]
 mov [ebp].Client_Reg_Struc.Client_EBX,eax
 mov eax,[esi+NPROC_SLOT_STEPS]
 mov [ebp].Client_Reg_Struc.Client_ECX,eax
 mov eax,[esi+NPROC_SLOT_RESULT]
 mov [ebp].Client_Reg_Struc.Client_EDX,eax
 mov eax,[esi+NPROC_SLOT_REPORTS]
 mov [ebp].Client_Reg_Struc.Client_ESI,eax
 mov eax,[esi+NPROC_SLOT_RESULT]
 test eax,eax
 jnz publish_vector
 mov eax,[esi+NPROC_SLOT_EXIT]
 jmp publish_terminal
publish_vector:
 mov eax,[esi+NPROC_SLOT_VECTOR]
publish_terminal:
 mov [ebp].Client_Reg_Struc.Client_EDI,eax
 mov eax,[esi+NPROC_SLOT_VALUE]
 mov [ebp].Client_Reg_Struc.Client_EBP,eax
 mov eax,[esi+NPROC_SLOT_STATE]
 clc
 ret
failed:
 stc
 ret
context_error:
 mov eax,VM_ERROR_CONTEXT
 stc
 ret
nproc_state endp

; Explicit cancellation by handle in EBX. Even a suspended process releases
; all its private mappings. A failed Jemm free enters CLEANUP and can be
; retried with the same handle.
nproc_stop proc uses ebx ecx edx esi edi
 cmp vmm_current,0
 jne context_error
 mov ebx,[ebp].Client_Reg_Struc.Client_EBX
 call nproc_slot_for_handle
 test eax,eax
 jnz failed
 cmp nproc_busy,0
 jne active_error
 cmp dword ptr [esi+NPROC_SLOT_STATE],NPROC_SLOT_ENDED
 je stopped
 call nproc_release_slot
 test eax,eax
 jnz cleanup_pending
 mov dword ptr [esi+NPROC_SLOT_STATE],NPROC_SLOT_ENDED
 mov dword ptr [esi+NPROC_SLOT_RESULT],5
stopped:
 xor eax,eax
 clc
 ret
cleanup_pending:
 mov dword ptr [esi+NPROC_SLOT_STATE],NPROC_SLOT_CLEANUP
 mov eax,VM_ERROR_MEMORY
 stc
 ret
active_error:
 mov eax,VM_ERROR_ACTIVE
 stc
 ret
failed:
 stc
 ret
context_error:
 mov eax,VM_ERROR_CONTEXT
 stc
 ret
nproc_stop endp

; One bounded native quantum per physical timer tick, in round-robin order.
; Called only by jlm_poll immediately before returning to V86. The DOS/FAT
; and urgent-audio gates prevent a native diagnostic from delaying guest I/O.
nproc_poll proc uses ebx ecx edx esi edi
 cmp vmm_installed,0
 je done
 cmp nproc_busy,0
 jne done
 cmp vmm_borrowed,-1
 jne done
 cmp ebp,vmm_client_frame
 jne done
 mov eax,vmm_audio_urgent
 cmp eax,VMM_MAX
 jb done
 mov eax,vmm_kbase
 test eax,eax
 jz done
 cmp byte ptr [eax+KL_INDOS],0
 jne done
 cmp byte ptr [eax+KL_FAT_DIRTY],0
 jne done
 mov eax,vmm_ticks
 cmp eax,nproc_last_tick
 je done
 mov nproc_last_tick,eax
 mov ebx,nproc_next
 mov ecx,NPROC_MAX_SLOTS
find_ready:
 mov eax,ebx
 imul eax,NPROC_SLOT_BYTES
 lea edi,[nproc_slots+eax]
 cmp dword ptr [edi+NPROC_SLOT_STATE],NPROC_SLOT_READY
 je run_slot
 inc ebx
 and ebx,NPROC_MAX_SLOTS-1
 dec ecx
 jnz find_ready
 jmp done
run_slot:
 mov eax,ebx
 inc eax
 and eax,NPROC_MAX_SLOTS-1
 mov nproc_next,eax
 mov nproc_busy,1
 mov dword ptr [edi+NPROC_SLOT_STATE],NPROC_SLOT_RUNNING
 lea esi,[edi+NPROC_SLOT_CONTEXT]
 call native_entry_step@0
 mov ebx,eax
 mov eax,[esi+40]                    ; reports made in this quantum
 test eax,eax
 jz no_reports
 add [edi+NPROC_SLOT_REPORTS],eax
 mov eax,[esi+44]
 mov [edi+NPROC_SLOT_TAG],eax
 mov eax,[esi+48]
 mov [edi+NPROC_SLOT_VALUE],eax
no_reports:
 mov eax,[esi+NC_STEPS]
 mov [edi+NPROC_SLOT_STEPS],eax
 cmp ebx,NE_BUSY
 je yielded
 cmp ebx,NE_BUDGET
 jne terminal
 cmp dword ptr [esi+NC_STATE],NC_SUSPENDED
 je yielded
 mov ebx,NE_CONFIG
terminal:
 mov [edi+NPROC_SLOT_RESULT],ebx
 mov eax,[esi+28]
 mov [edi+NPROC_SLOT_VECTOR],eax
 mov eax,[esi+32]
 mov [edi+NPROC_SLOT_ERROR],eax
 mov eax,[esi+56]
 mov [edi+NPROC_SLOT_EXIT],eax
 mov esi,edi
 call nproc_release_slot
 test eax,eax
 jnz cleanup_pending
 mov dword ptr [edi+NPROC_SLOT_STATE],NPROC_SLOT_ENDED
 jmp idle
cleanup_pending:
 mov dword ptr [edi+NPROC_SLOT_STATE],NPROC_SLOT_CLEANUP
 jmp idle
yielded:
 mov dword ptr [edi+NPROC_SLOT_STATE],NPROC_SLOT_READY
idle:
 mov nproc_busy,0
done:
 ret
nproc_poll endp
