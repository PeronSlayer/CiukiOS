; Real user instructions verify private state and post-syscall continuation.
bits 32
cpu 386
org 0
%ifndef NATIVE_COOKIE
 %define NATIVE_COOKIE 0x41414943
%endif
%ifdef NATIVE_DATA_ONLY
 dd NATIVE_COOKIE
 dd 0
%else
 mov eax,1
 mov ebx,1
 mov ecx,NATIVE_COOKIE
 times 252 nop
 int 0x80                            ; exactly step256: report must not replay
 test eax,eax
 jnz .bad
 cmp dword [esi],NATIVE_COOKIE
 jne .bad
 cmp dword [esi+4],0
 jne .bad
 mov eax,0x12345678 ^ NATIVE_COOKIE
 mov ebx,0x2468ACE0 ^ NATIVE_COOKIE
 mov edx,0x13579BDF ^ NATIVE_COOKIE
 mov ebp,0x0BADCAFE ^ NATIVE_COOKIE
 mov edi,0x11223344 ^ NATIVE_COOKIE
 mov ecx,400
 push dword 0x89ABCDEF ^ NATIVE_COOKIE
.count:
 inc dword [esi+4]
 add eax,1
 dec ecx
 jnz .count
 cmp eax,(0x12345678 ^ NATIVE_COOKIE)+400
 jne .bad
 cmp ebx,0x2468ACE0 ^ NATIVE_COOKIE
 jne .bad
 cmp edx,0x13579BDF ^ NATIVE_COOKIE
 jne .bad
 cmp ebp,0x0BADCAFE ^ NATIVE_COOKIE
 jne .bad
 cmp edi,0x11223344 ^ NATIVE_COOKIE
 jne .bad
 cmp dword [esi+4],400
 jne .bad
 cmp dword [esi],NATIVE_COOKIE
 jne .bad
 pop ebx
 cmp ebx,0x89ABCDEF ^ NATIVE_COOKIE
 jne .bad
 mov eax,1
 mov ebx,2
 mov ecx,[esi]
 int 0x80
 xor eax,eax
 xor ebx,ebx
 int 0x80
.bad:
 ud2
%endif
