; Two ordinary ring-3 processes, copied PCM writes and devctl.
; SPDX-License-Identifier: GPL-2.0-only
bits 32
%include "proc_abi.inc"
org CIUKI_IMAGE_BASE - CIUKI_PAGE_SIZE
%define DATA (CIUKI_IMAGE_BASE + CIUKI_PAGE_SIZE)
%define MODE (DATA+0)
%define READY (DATA+4)
%define START (DATA+8)
%define DONE (DATA+12)
%define ERRORS (DATA+16)
%define FD (DATA+20)
%define FRAME (DATA+24)
%define COUNT (DATA+28)
%define STATUS (DATA+32)
%define GAIN (DATA+256)
%define COMMAND (DATA+288)
%define PCM (DATA+512)
%macro CALL 1
    mov eax, %1
    int 0x80
%endmacro
    db 0x7f,'ELF',1,1,1,0,0
    times 7 db 0
    dw 2,3
    dd 1,entry,phdr-$$,0,0
    dw 52,32,2,0,0,0
phdr:
    dd 1,CIUKI_PAGE_SIZE,CIUKI_IMAGE_BASE,0,code_end-entry,CIUKI_PAGE_SIZE,5,CIUKI_PAGE_SIZE
    dd 1,2*CIUKI_PAGE_SIZE,DATA,0,4,CIUKI_PAGE_SIZE,6,CIUKI_PAGE_SIZE
    times CIUKI_PAGE_SIZE-($-$$) db 0
entry:
    mov ebx,path
    mov ecx,O_WRONLY
    xor edx,edx
    CALL CIUKI_SYS_OPEN
    test eax,eax
    js failed
    mov [FD],eax
    mov dword [READY],1
.wait:
    cmp dword [START],0
    jne .play
    mov ebx,1
    CALL CIUKI_SYS_SLEEP_MS
    jmp .wait
.play:
    xor esi,esi ; exact rational waveform phase (0..47999)
.next:
    cmp dword [FRAME],192000
    jae .drain
    cmp dword [FRAME],96000
    jne .generate
    mov dword [GAIN],1
    mov dword [GAIN+4],24
    mov eax,50
    cmp dword [MODE],0
    je .gain
    mov eax,25
.gain:
    mov [GAIN+8],eax
    mov [GAIN+12],eax
    mov ebx,[FD]
    mov ecx,AUDIO_SET_GAIN
    mov edx,GAIN
    mov edi,esi
    mov esi,24
    CALL CIUKI_SYS_DEVCTL
    mov esi,edi
    test eax,eax
    js failed
.generate:
    mov ecx,256
    mov eax,192000
    sub eax,[FRAME]
    cmp eax,ecx
    jae .bounded
    mov ecx,eax
.bounded:
    mov [COUNT],ecx
    mov edi,PCM
.sample:
    mov eax,esi
    shl eax,8
    xor edx,edx
    mov ebx,48000
    div ebx
    movsx ebp,word [sine+eax*2]
    mov eax,[FRAME]
    cmp dword [MODE],0
    jne .right
    cmp eax,48000
    jb .left_on
    cmp eax,96000
    jae .left_on
    xor ebp,ebp
.left_on:
    mov [edi],bp
    mov word [edi+2],0
    add esi,440
    jmp .phase
.right:
    cmp eax,48000
    jae .right_on
    xor ebp,ebp
.right_on:
    mov word [edi],0
    mov [edi+2],bp
    add esi,880
.phase:
    cmp esi,48000
    jb .store
    sub esi,48000
.store:
    add edi,4
    inc dword [FRAME]
    loop .sample
    mov ebx,[FD]
    mov ecx,PCM
    mov edx,[COUNT]
    shl edx,2
    CALL CIUKI_SYS_WRITE
    cmp eax,edx
    jne failed
    jmp .next
.drain:
    mov dword [COMMAND],1
    mov dword [COMMAND+4],16
    mov ebx,[FD]
    mov ecx,AUDIO_DRAIN
    mov edx,COMMAND
    mov esi,16
    CALL CIUKI_SYS_DEVCTL
    test eax,eax
    js failed
    mov dword [STATUS],1
    mov dword [STATUS+4],112
    mov ebx,[FD]
    mov ecx,AUDIO_GET_STATUS
    mov edx,STATUS
    mov esi,112
    CALL CIUKI_SYS_DEVCTL
    test eax,eax
    js failed
    mov dword [DONE],1
.hold:
    mov ebx,10
    CALL CIUKI_SYS_SLEEP_MS
    jmp .hold
failed:
    mov [ERRORS],eax
    mov dword [DONE],1
    jmp entry.hold
path db '/dev/audio',0
align 2
sine:
    dw 0,294,589,883,1176,1469,1761,2052,2341,2629,2916,3201,3483,3764,4043,4319
    dw 4592,4863,5131,5395,5657,5915,6169,6420,6667,6910,7148,7383,7613,7838,8059,8274
    dw 8485,8691,8891,9087,9276,9460,9638,9811,9978,10138,10293,10441,10583,10719,10848,10971
    dw 11087,11196,11299,11394,11483,11565,11640,11708,11769,11823,11870,11910,11942,11967,11986,11996
    dw 12000,11996,11986,11967,11942,11910,11870,11823,11769,11708,11640,11565,11483,11394,11299,11196
    dw 11087,10971,10848,10719,10583,10441,10293,10138,9978,9811,9638,9460,9276,9087,8891,8691
    dw 8485,8274,8059,7838,7613,7383,7148,6910,6667,6420,6169,5915,5657,5395,5131,4863
    dw 4592,4319,4043,3764,3483,3201,2916,2629,2341,2052,1761,1469,1176,883,589,294
    dw 0,-294,-589,-883,-1176,-1469,-1761,-2052,-2341,-2629,-2916,-3201,-3483,-3764,-4043,-4319
    dw -4592,-4863,-5131,-5395,-5657,-5915,-6169,-6420,-6667,-6910,-7148,-7383,-7613,-7838,-8059,-8274
    dw -8485,-8691,-8891,-9087,-9276,-9460,-9638,-9811,-9978,-10138,-10293,-10441,-10583,-10719,-10848,-10971
    dw -11087,-11196,-11299,-11394,-11483,-11565,-11640,-11708,-11769,-11823,-11870,-11910,-11942,-11967,-11986,-11996
    dw -12000,-11996,-11986,-11967,-11942,-11910,-11870,-11823,-11769,-11708,-11640,-11565,-11483,-11394,-11299,-11196
    dw -11087,-10971,-10848,-10719,-10583,-10441,-10293,-10138,-9978,-9811,-9638,-9460,-9276,-9087,-8891,-8691
    dw -8485,-8274,-8059,-7838,-7613,-7383,-7148,-6910,-6667,-6420,-6169,-5915,-5657,-5395,-5131,-4863
    dw -4592,-4319,-4043,-3764,-3483,-3201,-2916,-2629,-2341,-2052,-1761,-1469,-1176,-883,-589,-294
code_end:
    times 2*CIUKI_PAGE_SIZE-($-$$) db 0
    dd 0
