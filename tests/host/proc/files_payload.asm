; Interim files/clocks ring-3 workload, values from the public ABI header.
; SPDX-License-Identifier: GPL-2.0-only
bits 32
%include "proc_abi.inc"
org CIUKI_IMAGE_BASE-CIUKI_PAGE_SIZE
%define R (CIUKI_IMAGE_BASE+2*CIUKI_PAGE_SIZE)
%define DONE (R+0)
%define RELEASE (R+4)
%define CHECKS (R+8)
%define ERRORS (R+12)
%define STAGE (R+16)
%define TID (R+20)
%define READS (R+24)
%define DECREASES (R+28)
%define MONO (R+32)
%define REAL (R+48)
%define CPU0 (R+64)
%define CPU1 (R+80)
%define SCPU0 (R+96)
%define SCPU1 (R+112)
%define BEGIN (R+128)
%define END (R+144)
%define REM (R+160)
%define SLEEP_RC (R+176)
%define INTERRUPT_RC (R+180)
%define HANDLERS (R+184)
%define CASES (R+188)
%define NOW (R+256)
%define REQUEST (R+272)
%define ACTION (R+288)
%define BUFFER (R+512)
%define ST (R+768)
%define FD (R+900)
%define DUPFD (R+904)
%define OFF (R+912)
%macro CALL 1
    mov eax,%1
    int 0x80
%endmacro
%macro EQ 2
    inc dword [CHECKS]
    cmp %1,%2
    je %%good
    inc dword [ERRORS]
%%good:
%endmacro
%macro TIME 2
    mov ebx,%1
    mov ecx,%2
    CALL CIUKI_SYS_CLOCK_GETTIME
    EQ eax,0
%endmacro
    db 0x7f,'ELF',1,1,1,0,0
    times 7 db 0
    dw 2,3
    dd 1,entry,phdr-$$,0,0
    dw 52,32,2,0,0,0
phdr:
    dd 1,CIUKI_PAGE_SIZE,CIUKI_IMAGE_BASE,0,code_end-entry,2*CIUKI_PAGE_SIZE,5,CIUKI_PAGE_SIZE
    dd 1,3*CIUKI_PAGE_SIZE,R,0,4,2*CIUKI_PAGE_SIZE,6,CIUKI_PAGE_SIZE
    times CIUKI_PAGE_SIZE-($-$$) db 0
entry:
    mov eax,[gs:4]
    mov [TID],eax
    ; Register a persistent catcher for the controller's selected-thread kill.
    mov dword [ACTION],handler
    mov dword [ACTION+16],restorer
    mov ebx,SIGUSR1
    mov ecx,ACTION
    xor edx,edx
    CALL CIUKI_SYS_SIGACTION
    EQ eax,0
    ; Bad flags and output buffers have no external side effects.
    mov ebx,path
    mov ecx,0x80000000
    CALL CIUKI_SYS_OPEN
    EQ eax,-EINVAL
    mov ebx,path
    mov ecx,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC
    mov edx,0666o
    CALL CIUKI_SYS_OPEN
    test eax,eax
    js failed
    mov [FD],eax
    mov ebx,path
    CALL CIUKI_SYS_OPEN
    EQ eax,-EEXIST
    mov ebx,[FD]
    mov ecx,F_GETFD
    CALL CIUKI_SYS_FCNTL
    EQ eax,FD_CLOEXEC
    mov ebx,[FD]
    mov ecx,CIUKI_IMAGE_BASE
    mov edx,8
    CALL CIUKI_SYS_READ
    EQ eax,-EFAULT
    mov ebx,[FD]
    mov ecx,0xffffffff
    mov edx,8
    CALL CIUKI_SYS_WRITE
    EQ eax,-EFAULT
    mov ebx,[FD]
    mov ecx,message
    mov edx,8
    CALL CIUKI_SYS_WRITE
    EQ eax,8
    mov ebx,[FD]
    CALL CIUKI_SYS_DUP
    mov [DUPFD],eax
    mov ebx,eax
    mov ecx,F_GETFD
    CALL CIUKI_SYS_FCNTL
    EQ eax,0
    mov ebx,[FD]
    mov ecx,BUFFER
    mov edx,8
    xor esi,esi
    xor edi,edi
    CALL CIUKI_SYS_PREAD
    EQ eax,8
    EQ dword [BUFFER],0x33323130
    mov ebx,[DUPFD]
    xor ecx,ecx
    xor edx,edx
    mov esi,SEEK_CUR
    mov edi,OFF
    CALL CIUKI_SYS_LSEEK64
    EQ eax,0
    EQ dword [OFF],8
    mov ebx,[FD]
    mov ecx,F_SETFL
    mov edx,O_APPEND
    CALL CIUKI_SYS_FCNTL
    EQ eax,0
    mov ebx,[DUPFD]
    mov ecx,F_GETFL
    CALL CIUKI_SYS_FCNTL
    EQ eax,O_RDWR|O_APPEND
    mov ebx,[FD]
    mov ecx,message
    mov edx,2
    xor esi,esi
    xor edi,edi
    CALL CIUKI_SYS_PWRITE
    EQ eax,2
    mov ebx,[FD]
    mov ecx,F_SETFL
    xor edx,edx
    CALL CIUKI_SYS_FCNTL
    EQ eax,0
    mov ebx,[FD]
    xor ecx,ecx
    mov edx,1
    mov esi,SEEK_SET
    mov edi,OFF
    CALL CIUKI_SYS_LSEEK64
    EQ eax,0
    EQ dword [OFF+4],1
    mov ebx,[FD]
    mov ecx,message
    mov edx,1
    CALL CIUKI_SYS_WRITE
    EQ eax,-EFBIG
    mov ebx,[FD]
    mov ecx,64
    xor edx,edx
    CALL CIUKI_SYS_FTRUNCATE
    EQ eax,0
    mov ebx,[FD]
    mov ecx,BUFFER
    mov edx,56
    mov esi,8
    xor edi,edi
    CALL CIUKI_SYS_PREAD
    EQ eax,56
    mov esi,BUFFER
    mov edi,56
.zeros:
    EQ byte [esi],0
    inc esi
    dec edi
    jnz .zeros
    mov ebx,[FD]
    CALL CIUKI_SYS_FSYNC
    EQ eax,0
    mov ebx,path
    CALL CIUKI_SYS_UNLINK
    EQ eax,0
    mov ebx,[FD]
    mov ecx,ST
    CALL CIUKI_SYS_FSTAT
    EQ eax,0
    EQ dword [ST+ABI_OFFSETOF_CIUKI_STAT_ST_NLINK],0
    mov ebx,[FD]
    CALL CIUKI_SYS_CLOSE
    EQ eax,0
    mov ebx,[DUPFD]
    CALL CIUKI_SYS_CLOSE
    EQ eax,0
    mov ebx,[DUPFD]
    CALL CIUKI_SYS_CLOSE
    EQ eax,-EBADF
    ; Two real threads append complete four-byte records through independent
    ; descriptions. A third read verifies no overwrite or interleaved record.
    mov ebx,append_path
    mov ecx,O_CREAT|O_EXCL|O_RDWR|O_APPEND
    mov edx,0666o
    CALL CIUKI_SYS_OPEN
    test eax,eax
    js failed
    mov [FD],eax
    mov ebx,append_path
    mov ecx,O_RDWR|O_APPEND
    CALL CIUKI_SYS_OPEN
    test eax,eax
    js failed
    mov [DUPFD],eax
    mov dword [R+1024],ABI_SIZEOF_CIUKI_THREAD_ARGS
    mov dword [R+1028],append_worker
    mov [R+1032],eax
    mov dword [R+1036],worker_return
    mov dword [R+1040],CIUKI_THREAD_STACK_MIN
    mov ebx,R+1024
    CALL CIUKI_SYS_THREAD_CREATE
    test eax,eax
    js failed
    mov [R+1056],eax
    mov ebp,32
.append_main:
    mov ebx,[FD]
    mov ecx,record_a
    mov edx,4
    CALL CIUKI_SYS_WRITE
    EQ eax,4
    dec ebp
    jnz .append_main
    mov ebx,[R+1056]
    xor ecx,ecx
    CALL CIUKI_SYS_THREAD_JOIN
    EQ eax,0
    mov ebx,[FD]
    mov ecx,BUFFER
    mov edx,256
    xor esi,esi
    xor edi,edi
    CALL CIUKI_SYS_PREAD
    EQ eax,256
    mov esi,BUFFER
    mov ebp,64
    xor edx,edx
    xor ecx,ecx
.records:
    lodsd
    cmp eax,0x41414141
    jne .record_b
    inc edx
    jmp .record_next
.record_b:
    EQ eax,0x42424242
    inc ecx
.record_next:
    dec ebp
    jnz .records
    EQ edx,32
    EQ ecx,32
    mov ebx,append_path
    CALL CIUKI_SYS_UNLINK
    EQ eax,0
    mov ebx,[FD]
    CALL CIUKI_SYS_CLOSE
    EQ eax,0
    mov ebx,[DUPFD]
    CALL CIUKI_SYS_CLOSE
    EQ eax,0
    mov dword [CASES],1
    TIME CLOCK_PROCESS_CPUTIME_ID,CPU0
    TIME CLOCK_MONOTONIC,MONO
.loop:
    TIME CLOCK_MONOTONIC,NOW
    mov eax,[NOW+4]
    cmp eax,[MONO+4]
    jb .decrease
    ja .next
    mov eax,[NOW]
    cmp eax,[MONO]
    jb .decrease
    ja .next
    mov eax,[NOW+8]
    cmp eax,[MONO+8]
    jae .next
.decrease:
    inc dword [DECREASES]
.next:
    mov esi,NOW
    mov edi,MONO
    mov ecx,4
    rep movsd
    inc dword [READS]
    cmp dword [READS],10000
    jne .loop
    TIME CLOCK_PROCESS_CPUTIME_ID,CPU1
    TIME CLOCK_MONOTONIC,MONO
    TIME CLOCK_REALTIME,REAL
    TIME CLOCK_PROCESS_CPUTIME_ID,SCPU0
    mov dword [REQUEST+8],100000000
    mov ebx,REQUEST
    mov ecx,REM
    CALL CIUKI_SYS_NANOSLEEP
    EQ eax,0
    TIME CLOCK_PROCESS_CPUTIME_ID,SCPU1
    TIME CLOCK_MONOTONIC,BEGIN
    mov dword [REQUEST+8],20000000
    mov ebx,REQUEST
    mov ecx,REM
    CALL CIUKI_SYS_NANOSLEEP
    mov [SLEEP_RC],eax
    EQ eax,0
    TIME CLOCK_MONOTONIC,END
    EQ dword [REM],0
    EQ dword [REM+8],0
    ; The controller waits ~10 ms after stage publication, then thread_kill.
    mov dword [STAGE],1
    mov ebx,REQUEST
    mov ecx,REM
    CALL CIUKI_SYS_NANOSLEEP
    mov [INTERRUPT_RC],eax
    EQ eax,-EINTR
    EQ dword [HANDLERS],1
    mov ebx,3
    mov ecx,NOW
    CALL CIUKI_SYS_CLOCK_GETTIME
    EQ eax,-EINVAL
    mov dword [REQUEST+8],1000000000
    mov ebx,REQUEST
    xor ecx,ecx
    CALL CIUKI_SYS_NANOSLEEP
    EQ eax,-EINVAL
    mov dword [REQUEST],0xffffffff
    mov dword [REQUEST+4],0x7fffffff
    mov dword [REQUEST+8],0
    mov ebx,REQUEST
    CALL CIUKI_SYS_NANOSLEEP
    EQ eax,-EOVERFLOW
    mov dword [DONE],1
.wait:
    cmp dword [RELEASE],1
    je .exit
    mov ebx,RELEASE
    xor ecx,ecx
    xor edx,edx
    mov esi,CLOCK_MONOTONIC
    CALL CIUKI_SYS_WAIT_WORD
    jmp .wait
.exit:
    xor ebx,ebx
    CALL CIUKI_SYS_EXIT
failed:
    inc dword [ERRORS]
    mov dword [DONE],1
    jmp entry.wait
append_worker:
    mov ebx,[esp+4]
    mov ebp,32
.loop:
    mov ecx,record_b
    mov edx,4
    CALL CIUKI_SYS_WRITE
    EQ eax,4
    dec ebp
    jnz .loop
worker_return:
    xor ebx,ebx
    CALL CIUKI_SYS_THREAD_EXIT
    ud2
handler:
    inc dword [HANDLERS]
    ret
restorer:
    lea ebx,[esp-4]
    CALL CIUKI_SYS_SIGRETURN
    ud2
path: db '/tmp/f2-user-files.bin',0
message: db '01234567'
append_path: db '/tmp/f2-append.bin',0
record_a: db 'AAAA'
record_b: db 'BBBB'
code_end:
    times 3*CIUKI_PAGE_SIZE-($-$$) db 0
    dd 0
