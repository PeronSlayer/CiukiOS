; Trusted page-block ownership for the future 32-bit process host.
; Jemm remains the sole allocator of physical frames. Handles include a
; generation so a released slot cannot be freed through a stale handle.
NATIVE_PAGE_SLOTS equ 64
NATIVE_PROBE_PAGES equ 257
NATIVE_PROBE_DWORDS equ NATIVE_PROBE_PAGES*1024
NATIVE_PAGE_RECORD struct
 linear dd ?
 pages dd ?
 owner dd ?
 generation dd ?
NATIVE_PAGE_RECORD ends

.data
native_page_records NATIVE_PAGE_RECORD NATIVE_PAGE_SLOTS dup (<0,0,0,0>)
native_page_live dd 0
native_probe_busy dd 0
native_probe_h1 dd 0
native_probe_h2 dd 0
native_probe_h3 dd 0
native_probe_p1 dd 0
native_probe_p2 dd 0
native_probe_live_before dd 0

.code
; EBX = nonzero owner, ECX = number of 4-KiB pages.
; Success: EAX = generation-tagged handle, EDX = supervisor linear address.
; Failure: EAX = EDX = 0. All committed pages start zero-filled.
native_page_alloc proc uses ebx ecx esi edi
 test ebx,ebx
 jz failed
 test ecx,ecx
 jz failed
 cmp ecx,0FFFFFh                      ; byte count must fit in 32 bits
 ja failed
 push ebx
 push ecx
 mov esi,1
 mov edi,offset native_page_records
find_slot:
 cmp [edi].NATIVE_PAGE_RECORD.linear,0
 je reserve
 add edi,sizeof NATIVE_PAGE_RECORD
 inc esi
 cmp esi,NATIVE_PAGE_SLOTS
 jbe find_slot
 jmp no_slot
reserve:
 mov ecx,[esp]
 push edi
 push 0
 push ecx
 push PR_SYSTEM
 @VMMCall _PageReserve
 add esp,12
 pop edi
 cmp eax,-1
 je no_slot
 mov [edi].NATIVE_PAGE_RECORD.linear,eax
 mov ecx,[esp]
 mov [edi].NATIVE_PAGE_RECORD.pages,ecx
 mov ebx,[esp+4]
 mov [edi].NATIVE_PAGE_RECORD.owner,ebx
 mov eax,[edi].NATIVE_PAGE_RECORD.generation
 inc eax
 and eax,0FFFFFFh
 jnz generation_ready
 mov eax,1
generation_ready:
 mov [edi].NATIVE_PAGE_RECORD.generation,eax
 shl eax,8
 or eax,esi
 inc native_page_live
 push eax                            ; retain the handle through commit
 mov eax,[edi].NATIVE_PAGE_RECORD.linear
 shr eax,12
 push edi
 push PC_FIXED or PC_WRITEABLE
 push 0
 push PD_FIXEDZERO
 push ecx
 push eax
 @VMMCall _PageCommit
 add esp,20
 pop edi
 test eax,eax
 jnz committed
 pop eax
 mov ebx,[edi].NATIVE_PAGE_RECORD.owner
 call native_page_free               ; failed free remains tracked by owner
 jmp no_slot
committed:
 pop eax
 mov edx,[edi].NATIVE_PAGE_RECORD.linear
 add esp,8
 ret
no_slot:
 add esp,8
failed:
 xor eax,eax
 xor edx,edx
 ret
native_page_alloc endp

; EAX = handle, EBX = owning process id. Returns EAX = 0 on success,
; VM_ERROR_ADDRESS if the handle is stale, foreign or out of range.
native_page_free proc uses ebx esi edi edx
 mov esi,eax
 and esi,0FFh
 test esi,esi
 jz invalid
 cmp esi,NATIVE_PAGE_SLOTS
 ja invalid
 mov edx,eax
 shr edx,8
 test edx,edx
 jz invalid
 dec esi
 imul esi,sizeof NATIVE_PAGE_RECORD
 lea edi,[native_page_records+esi]
 cmp [edi].NATIVE_PAGE_RECORD.generation,edx
 jne invalid
 cmp [edi].NATIVE_PAGE_RECORD.owner,ebx
 jne invalid
 mov eax,[edi].NATIVE_PAGE_RECORD.linear
 test eax,eax
 jz invalid
 push edi
 push 0
 push eax
 @VMMCall _PageFree
 add esp,8
 pop edi
 test eax,eax
 jz free_failed
 mov [edi].NATIVE_PAGE_RECORD.linear,0
 mov [edi].NATIVE_PAGE_RECORD.pages,0
 mov [edi].NATIVE_PAGE_RECORD.owner,0
 dec native_page_live
 xor eax,eax
 ret
free_failed:
 mov eax,VM_ERROR_MEMORY
 ret
invalid:
 mov eax,VM_ERROR_ADDRESS
 ret
native_page_free endp

; EBX = owner. Release every page block of a terminating process.
; EAX = count released, or -1 for an invalid owner/metadata failure.
native_page_release_owner proc uses ebx ecx esi edi edx
 test ebx,ebx
 jz invalid_owner
 xor ecx,ecx
 xor esi,esi
 mov edi,offset native_page_records
next_record:
 cmp [edi].NATIVE_PAGE_RECORD.owner,ebx
 jne next
 cmp [edi].NATIVE_PAGE_RECORD.linear,0
 je next
 mov eax,[edi].NATIVE_PAGE_RECORD.generation
 shl eax,8
 lea edx,[esi+1]
 or eax,edx
 push ecx
 call native_page_free
 pop ecx
 test eax,eax
 jnz invalid_owner
 inc ecx
next:
 add edi,sizeof NATIVE_PAGE_RECORD
 inc esi
 cmp esi,NATIVE_PAGE_SLOTS
 jb next_record
 mov eax,ecx
 ret
invalid_owner:
 mov eax,-1
 ret
native_page_release_owner endp

; Ring-0 allocation probe. Exercises zeroing, two owners, stale-handle
; rejection and release. No raw pointer is returned to a V86 caller.
native_page_probe proc uses ebx ecx edx esi edi
 cmp native_probe_busy,0
 jne busy
 mov native_probe_busy,1
 mov eax,native_page_live
 mov native_probe_live_before,eax
 mov native_probe_h1,0
 mov native_probe_h2,0
 mov native_probe_h3,0
 mov ebx,11h
 mov ecx,NATIVE_PROBE_PAGES
 call native_page_alloc
 test eax,eax
 jz memory_error
 cmp edx,100000h
 jbe memory_error
 mov native_probe_h1,eax
 mov native_probe_p1,edx
 mov ebx,22h
 mov ecx,NATIVE_PROBE_PAGES
 call native_page_alloc
 test eax,eax
 jz memory_error
 cmp edx,100000h
 jbe memory_error
 mov native_probe_h2,eax
 mov native_probe_p2,edx
 cmp edx,native_probe_p1
 je memory_error
 mov edi,native_probe_p1
 mov ecx,NATIVE_PROBE_DWORDS
 xor eax,eax
 repe scasd
 jne memory_error
 mov edi,native_probe_p2
 mov ecx,NATIVE_PROBE_DWORDS
 xor eax,eax
 repe scasd
 jne memory_error
 mov edi,native_probe_p1
 mov ecx,NATIVE_PROBE_DWORDS
 mov eax,11223344h
 rep stosd
 mov edi,native_probe_p2
 mov ecx,NATIVE_PROBE_DWORDS
 mov eax,55667788h
 rep stosd
 mov edi,native_probe_p1
 mov ecx,NATIVE_PROBE_DWORDS
 mov eax,11223344h
 repe scasd
 jne memory_error
 mov edi,native_probe_p2
 mov ecx,NATIVE_PROBE_DWORDS
 mov eax,55667788h
 repe scasd
 jne memory_error
 mov eax,native_probe_h1
 mov ebx,22h
 call native_page_free
 cmp eax,VM_ERROR_ADDRESS
 jne memory_error
 mov eax,native_probe_h1
 mov ebx,11h
 call native_page_free
 test eax,eax
 jnz memory_error
 ; Keep h1 as a stale handle and ensure a reused slot rejects it.
 mov ebx,11h
 mov ecx,NATIVE_PROBE_PAGES
 call native_page_alloc
 test eax,eax
 jz memory_error
 mov native_probe_h3,eax
 mov edi,edx
 mov ecx,NATIVE_PROBE_DWORDS
 xor eax,eax
 repe scasd
 jne memory_error
 mov eax,native_probe_h1
 mov ebx,11h
 call native_page_free
 cmp eax,VM_ERROR_ADDRESS
 jne memory_error
 mov eax,native_probe_h3
 mov ebx,11h
 call native_page_free
 test eax,eax
 jnz memory_error
 mov native_probe_h3,0
 mov ebx,22h
 call native_page_release_owner
 cmp eax,1
 jne memory_error
 mov eax,native_probe_h2
 mov ebx,22h
 call native_page_free
 cmp eax,VM_ERROR_ADDRESS
 jne memory_error
 mov native_probe_h2,0
 mov native_probe_h1,0
 mov eax,native_page_live
 cmp eax,native_probe_live_before
 jne memory_error
 mov native_probe_busy,0
 xor eax,eax
 ret
memory_error:
 mov ebx,11h
 call native_page_release_owner
 mov ebx,22h
 call native_page_release_owner
 mov native_probe_h1,0
 mov native_probe_h2,0
 mov native_probe_h3,0
 mov native_probe_busy,0
 mov eax,VM_ERROR_MEMORY
 ret
busy:
 mov eax,VM_ERROR_ACTIVE
 ret
native_page_probe endp
