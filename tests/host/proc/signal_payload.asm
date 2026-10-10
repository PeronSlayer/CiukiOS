; Interim signals-fault ELF. All public values/offsets are extracted from abi.h.
; Private result page fields mirror f2_probes_signals.c, not a public ABI.
; SPDX-License-Identifier: GPL-2.0-only
bits 32
%include "proc_abi.inc"
org CIUKI_IMAGE_BASE - CIUKI_PAGE_SIZE
%define DATA (CIUKI_IMAGE_BASE + 4*CIUKI_PAGE_SIZE)
%define MODE (DATA + 0)
%define STAGE (DATA + 4)
%define ERRORS (DATA + 8)
%define ENTRIES (DATA + 12)
%define RETURNS (DATA + 16)
%define DEPTH (DATA + 20)
%define MAX_DEPTH (DATA + 24)
%define LAST_SIGNO (DATA + 28)
%define LAST_VECTOR (DATA + 32)
%define LAST_CODE (DATA + 36)
%define LAST_ERROR (DATA + 40)
%define LAST_ADDRESS (DATA + 44)
%define LAST_EIP (DATA + 48)
%define SENDER (DATA + 52)
%define TID (DATA + 56)
%define PID (DATA + 60)
%define PEER (DATA + 64)
%define OTHER_GROUP (DATA + 68)
%define RESUME (DATA + 72)
%define EXPECT_EIP (DATA + 76)
%define EXPECT_VECTOR (DATA + 80)
%define EXPECT_ERROR (DATA + 84)
%define EXPECT_CODE (DATA + 88)
%define EXPECT_ADDRESS (DATA + 92)
%define EXPECT_SIGNO (DATA + 96)
%define SLEEP_RESULT (DATA + 100)
%define FIRST_TICK (DATA + 104)
%define LAST_TICK (DATA + 108)
%define ORDER (DATA + 112)
%define SAVED_EAX (DATA + 116)
%define RESUMED_EAX (DATA + 120)
%define PROGRESS (DATA + 124)
%define SAVED_MASK (DATA + 128)
%define FP_DIGEST (DATA + 132)
%define EXPECT_TLS (DATA + 136)
%define RELEASE (DATA + 140)
%define PEER_TID (DATA + 144)
%define RELEASE_COUNT (DATA + 148)
%define COMPLETION (DATA + 152)
%define HANDLER_COMPLETION (DATA + 156)
%define HANDLER_REMAIN (DATA + 160)
%define AC_FALLBACK (DATA + 164)
%define PAIR (DATA + 176)
%define MESSAGE (DATA + 640)
%define ACTION (DATA + 256)
%define MASK (DATA + 288)
%define REQUEST (DATA + 304)
%define REMAIN (DATA + 320)
%define QUERY (DATA + 512)
%define FP (DATA + 1024)
%define CW (DATA + 1152)
%define CREG(r) (ABI_OFFSETOF_CIUKI_UCONTEXT_GREGS + 4*(r))
%macro CALL 1
    mov eax, %1
    int 0x80
%endmacro
%macro EQ 2
    cmp %1, %2
    je %%ok
    inc dword [ERRORS]
%%ok:
%endmacro
%macro FAULT 6
    mov dword [EXPECT_VECTOR], %1
    mov dword [EXPECT_SIGNO], %2
    mov dword [EXPECT_ERROR], %3
    mov dword [EXPECT_CODE], %4
    mov dword [EXPECT_EIP], %5
    mov dword [EXPECT_ADDRESS], %6
%endmacro
    db 0x7f, 'ELF', 1, 1, 1, 0, 0
    times 7 db 0
    dw 2, 3
    dd 1, entry, phdr-$$, 0, 0
    dw 52, 32, 2, 0, 0, 0
phdr:
    dd 1, CIUKI_PAGE_SIZE, CIUKI_IMAGE_BASE, 0, code_end-entry, 4*CIUKI_PAGE_SIZE, 5, CIUKI_PAGE_SIZE
    dd 1, 5*CIUKI_PAGE_SIZE, DATA, 0, 4, CIUKI_PAGE_SIZE, 6, CIUKI_PAGE_SIZE
    times CIUKI_PAGE_SIZE-($-$$) db 0
entry:
    CALL CIUKI_SYS_GETPID
    mov [PID], eax
    mov eax, [gs:ABI_OFFSETOF_CIUKI_TCB_TID]
    mov [TID], eax
    mov eax, [gs:ABI_OFFSETOF_CIUKI_TCB_SELF]
    mov [EXPECT_TLS], eax
    ; Install persistent SA_SIGINFO actions through the public syscall.
    mov dword [ACTION + ABI_OFFSETOF_CIUKI_SIGACTION_HANDLER], handler
    mov dword [ACTION + ABI_OFFSETOF_CIUKI_SIGACTION_FLAGS], SA_SIGINFO
    mov dword [ACTION + ABI_OFFSETOF_CIUKI_SIGACTION_RESTORER], restorer
    mov esi, signals
.install:
    mov ebx, [esi]
    test ebx, ebx
    jz .installed
    mov ecx, ACTION
    xor edx, edx
    CALL CIUKI_SYS_SIGACTION
    EQ eax, 0
    add esi, 4
    jmp .install
.installed:
    cmp dword [MODE], 20
    je survivor
    cmp dword [MODE], 0
    je faults
    cmp dword [MODE], 1
    je async_tests
    cmp dword [MODE], 2
    je default_fault
    cmp dword [MODE], 3
    je blocked_fault
    cmp dword [MODE], 4
    je ignored_fault
    cmp dword [MODE], 5
    je bad_stack
    cmp dword [MODE], 6
    je handler_fault
    cmp dword [MODE], 7
    je forged_return
    cmp dword [MODE], 8
    je outside_return
    cmp dword [MODE], 9
    je wait_kill
    cmp dword [MODE], 10
    je handler_blocking
    cmp dword [MODE], 11
    je handler_blocking
    cmp dword [MODE], 12
    je handler_blocking
    cmp dword [MODE], 13
    je sleep_interrupted
    cmp dword [MODE], 14
    je thread_target
    cmp dword [MODE], 15
    je channel_interrupted
    cmp dword [MODE], 16
    je close_deferred
    cmp dword [MODE], 17
    je dup2_deferred
    jmp fail_exit
faults:
    fld1
    fldpi
    fnsave [FP]
    frstor [FP]
    mov eax, [FP + 28]
    xor eax, [FP + 32]
    mov [FP_DIGEST], eax
    mov edi, 0x12345678
    mov esi, 0x87654321
    mov ebp, 0x13579bdf
    FAULT 14, SIGSEGV, 4, SEGV_MAPERR, .unmapped, CIUKI_MMAP_LIMIT - CIUKI_PAGE_SIZE
    mov dword [RESUME], .unmapped_done
    std
.unmapped:
    mov eax, [CIUKI_MMAP_LIMIT - CIUKI_PAGE_SIZE]
.unmapped_done:
    pushfd
    pop eax
    test eax, 1 << 10
    jnz .df_ok
    inc dword [ERRORS]
.df_ok:
    cld
    call check_regs
    FAULT 14, SIGSEGV, 7, SEGV_ACCERR, .readonly, entry
    mov dword [RESUME], .readonly_done
.readonly:
    mov byte [entry], 0
.readonly_done:
    call check_regs
    FAULT 6, SIGILL, 0, ILL_ILLOPC, .ud, .ud
    mov dword [RESUME], .ud_done
.ud:
    ud2
.ud_done:
    call check_regs
    FAULT 0, SIGFPE, 0, FPE_INTDIV, .divide, .divide
    mov dword [RESUME], .divide_done
    xor ecx, ecx
    xor edx, edx
    mov eax, 1
.divide:
    div ecx
.divide_done:
    call check_regs
    FAULT 13, SIGSEGV, 0, SEGV_ACCERR, .gp, .gp
    mov dword [RESUME], .gp_done
.gp:
    cli
.gp_done:
    call check_regs
    FAULT 17, SIGBUS, 0, BUS_ADRALN, .ac, 0
    mov dword [RESUME], .ac_done
    pushfd
    or dword [esp], 1 << 18
    popfd
.ac:
    mov eax, [DATA + 1]
    ; Intel requires #AC here. QEMU 11 TCG's scalar loads omit the check.
    ; Accept that omission only for its exact CPUID signature, and exercise
    ; another real fault with AC set so the seven-delivery checks stay strict.
    ; Evidence/limitations: docs/validation/2026-10-09-f0/README.md (f2-14).
    pushad
    mov eax, 0x40000000
    xor ecx, ecx
    cpuid
    cmp eax, 0x40000001
    jb .ac_missing
    cmp ebx, 0x54474354             ; "TCGT"
    jne .ac_missing
    cmp ecx, 0x43544743             ; "CGTC"
    jne .ac_missing
    cmp edx, 0x47435447             ; "GTCG"
    jne .ac_missing
    mov dword [AC_FALLBACK], 1
    jmp .ac_fallback
.ac_missing:
    inc dword [ERRORS]             ; hardware must deliver SIGBUS/#AC
.ac_fallback:
    popad
    FAULT 14, SIGSEGV, 4, SEGV_MAPERR, .ac_page_fault, CIUKI_MMAP_LIMIT - CIUKI_PAGE_SIZE
    mov dword [RESUME], .ac_done
.ac_page_fault:
    mov eax, [CIUKI_MMAP_LIMIT - CIUKI_PAGE_SIZE]
.ac_done:
    pushfd
    pop eax
    test eax, 1 << 18
    jnz .ac_ok
    inc dword [ERRORS]
.ac_ok:
    pushfd
    and dword [esp], ~(1 << 18)
    popfd
    call check_regs
    ; Unmask divide-by-zero and wait at a known EIP. Handler repairs FSW/FCW.
    FAULT 16, SIGFPE, 0, CIUKI_SI_X87, .mf, .mf
    mov dword [RESUME], .mf_done
    fnstcw [CW]
    and word [CW], ~(1 << 2)
    fldcw [CW]
    fld1
    fldz
    fdivp st1, st0
.mf:
    fwait
.mf_done:
    fwait
    call check_regs
    EQ dword [ENTRIES], 7
    EQ dword [RETURNS], 7
    jmp done
check_regs:
    EQ edi, 0x12345678
    EQ esi, 0x87654321
    EQ ebp, 0x13579bdf
    mov eax, [gs:ABI_OFFSETOF_CIUKI_TCB_SELF]
    EQ eax, [EXPECT_TLS]
    ret
async_tests:
    mov dword [MASK], 1 << (SIGUSR1 - 1)
    mov ebx, SIG_BLOCK
    mov ecx, MASK
    xor edx, edx
    CALL CIUKI_SYS_SIGPROCMASK
    EQ eax, 0
    mov ebx, [PID]
    mov ecx, SIGUSR1
    CALL CIUKI_SYS_KILL
    EQ eax, 0
    CALL CIUKI_SYS_KILL
    EQ eax, 0
    EQ dword [ENTRIES], 0
    mov ebx, SIG_UNBLOCK
    mov ecx, MASK
    xor edx, edx
    CALL CIUKI_SYS_SIGPROCMASK
    EQ eax, 0
    EQ dword [ENTRIES], 1
    EQ dword [RETURNS], 1
    ; Public authority checks (controller supplies a same-group peer).
    mov ebx, [PEER]
    mov ecx, SIGUSR1
    CALL CIUKI_SYS_KILL
    EQ eax, 0
    mov ebx, [OTHER_GROUP]
    xor ecx, ecx
    CALL CIUKI_SYS_KILL
    EQ eax, -EPERM
    mov ebx, 1
    CALL CIUKI_SYS_KILL
    EQ eax, -EPERM
    mov ebx, -1
    CALL CIUKI_SYS_KILL
    EQ eax, -EINVAL
    xor ebx, ebx
    CALL CIUKI_SYS_KILL
    EQ eax, 0
    mov ebx, [TID]
    mov ecx, SIGUSR2
    CALL CIUKI_SYS_THREAD_KILL
    EQ eax, 0
    EQ dword [ENTRIES], 2
    EQ dword [RETURNS], 2
    jmp done
default_fault:
    mov dword [ACTION + ABI_OFFSETOF_CIUKI_SIGACTION_HANDLER], SIG_DFL
    jmp set_segv
ignored_fault:
    mov dword [ACTION + ABI_OFFSETOF_CIUKI_SIGACTION_HANDLER], SIG_IGN
set_segv:
    mov ebx, SIGSEGV
    mov ecx, ACTION
    xor edx, edx
    CALL CIUKI_SYS_SIGACTION
    mov eax, [CIUKI_MMAP_LIMIT - CIUKI_PAGE_SIZE]
    jmp fail_exit
blocked_fault:
    mov dword [MASK], 1 << (SIGSEGV - 1)
    mov ebx, SIG_BLOCK
    mov ecx, MASK
    xor edx, edx
    CALL CIUKI_SYS_SIGPROCMASK
    mov eax, [CIUKI_MMAP_LIMIT - CIUKI_PAGE_SIZE]
    jmp fail_exit
bad_stack:
    mov esp, [gs:ABI_OFFSETOF_CIUKI_TCB_STACK_BASE]
    mov eax, [CIUKI_MMAP_LIMIT - CIUKI_PAGE_SIZE]
    jmp fail_exit
handler_fault:
forged_return:
    mov ebx, [TID]
    mov ecx, SIGUSR1
    CALL CIUKI_SYS_THREAD_KILL
    jmp fail_exit
outside_return:
    mov ebx, DATA
    CALL CIUKI_SYS_SIGRETURN
    jmp fail_exit
wait_kill:
    mov dword [STAGE], 1
.spin:
    inc dword [PROGRESS]
    jmp .spin
handler_blocking:
    mov ebx, [TID]
    mov ecx, SIGUSR1
    CALL CIUKI_SYS_THREAD_KILL
    EQ eax, 0
    EQ dword [ENTRIES], 2
    EQ dword [RETURNS], 2
    EQ dword [MAX_DEPTH], 1
    EQ dword [ORDER], 3
    jmp done
sleep_interrupted:
    mov dword [REMAIN + ABI_OFFSETOF_CIUKI_TIMESPEC_TV_NSEC], 0x7f7f7f7f
    mov dword [STAGE], 2
    call sample_start
    call sleep20
    mov [SLEEP_RESULT], eax
    mov [RESUMED_EAX], eax
    EQ eax, -EINTR
    EQ dword [SAVED_EAX], -EINTR
    cmp dword [REMAIN + ABI_OFFSETOF_CIUKI_TIMESPEC_TV_NSEC], 20000000
    jbe .remaining_ok
    inc dword [ERRORS]
.remaining_ok:
    call sample_end
    jmp done
channel_interrupted:
    mov ebx, PAIR
    CALL CIUKI_SYS_CHANNEL_PAIR
    EQ eax, 0
    mov dword [STAGE], 2
    mov ebx, [PAIR]
    mov ecx, MESSAGE
    xor edx, edx
    CALL CIUKI_SYS_CHANNEL_RECV
    mov [SLEEP_RESULT], eax
    mov [RESUMED_EAX], eax
    EQ eax, -EINTR
    EQ dword [SAVED_EAX], -EINTR
    mov ebx, [PAIR]
    CALL CIUKI_SYS_CLOSE
    EQ eax, 0
    mov ebx, [PAIR+4]
    CALL CIUKI_SYS_CLOSE
    EQ eax, 0
    jmp done
close_deferred:
    mov ebx, 8
    CALL CIUKI_SYS_CLOSE
    mov [SLEEP_RESULT], eax
    mov [RESUMED_EAX], eax
    EQ eax, 0
    EQ dword [SAVED_EAX], 0
    EQ dword [RELEASE_COUNT], 1
    EQ dword [HANDLER_COMPLETION], 1
    mov ebx, 8
    CALL CIUKI_SYS_CLOSE
    EQ eax, -EBADF
    jmp done
dup2_deferred:
    mov ebx, 9
    mov ecx, 8
    CALL CIUKI_SYS_DUP2
    mov [SLEEP_RESULT], eax
    mov [RESUMED_EAX], eax
    EQ eax, 8
    EQ dword [SAVED_EAX], 8
    EQ dword [RELEASE_COUNT], 1
    EQ dword [HANDLER_COMPLETION], 1
    jmp done
thread_target:
    ; A second real thread installs the same process disposition and waits.
    mov dword [REQUEST + ABI_OFFSETOF_CIUKI_THREAD_ARGS_SIZE], ABI_SIZEOF_CIUKI_THREAD_ARGS
    mov dword [REQUEST + ABI_OFFSETOF_CIUKI_THREAD_ARGS_ENTRY], worker
    mov dword [REQUEST + ABI_OFFSETOF_CIUKI_THREAD_ARGS_RETURN_TRAMPOLINE], worker_exit
    mov dword [REQUEST + ABI_OFFSETOF_CIUKI_THREAD_ARGS_STACK_BYTES], CIUKI_THREAD_STACK_MIN
    mov ebx, REQUEST
    CALL CIUKI_SYS_THREAD_CREATE
    test eax, eax
    js fail_exit
    mov [PEER_TID], eax
    mov dword [STAGE], 3
.wait_worker:
    cmp dword [ORDER], 1
    je .join
    CALL CIUKI_SYS_YIELD
    jmp .wait_worker
.join:
    mov ebx, [PEER_TID]
    xor ecx, ecx
    CALL CIUKI_SYS_THREAD_JOIN
    EQ eax, 0
    EQ dword [ENTRIES], 1
    jmp done
worker:
    cmp dword [ORDER], 1
    je worker_exit
    CALL CIUKI_SYS_YIELD
    jmp worker
worker_exit:
    xor ebx, ebx
    CALL CIUKI_SYS_THREAD_EXIT
    ud2
sample_start:
    mov ebx, QUERY
    mov ecx, CIUKI_PROBE_QUERY_MAX
    ; Output actual request length fits the result page; cap is only a bound.
    CALL CIUKI_SYS_PROBE_QUERY
    mov eax, [QUERY + 4]
    mov [FIRST_TICK], eax
    ret
sample_end:
    mov ebx, QUERY
    mov ecx, CIUKI_PROBE_QUERY_MAX
    CALL CIUKI_SYS_PROBE_QUERY
    mov eax, [QUERY + 4]
    mov [LAST_TICK], eax
    ret
sleep20:
    mov dword [REQUEST + ABI_OFFSETOF_CIUKI_TIMESPEC_TV_SEC], 0
    mov dword [REQUEST + ABI_OFFSETOF_CIUKI_TIMESPEC_TV_SEC + 4], 0
    mov dword [REQUEST + ABI_OFFSETOF_CIUKI_TIMESPEC_TV_NSEC], 20000000
    mov dword [REQUEST + ABI_OFFSETOF_CIUKI_TIMESPEC_RESERVED], 0
    mov ebx, REQUEST
    mov ecx, REMAIN
    CALL CIUKI_SYS_NANOSLEEP
    ret
handler:
    ; Entry AC/DF/TF must already be clear before even the first push.
    pushfd
    pop eax
    test eax, (1 << 18) | (1 << 10) | (1 << 8)
    jz .flags_ok
    inc dword [ERRORS]
.flags_ok:
    inc dword [ENTRIES]
    inc dword [DEPTH]
    mov eax, [DEPTH]
    cmp eax, [MAX_DEPTH]
    jbe .depth_ok
    mov [MAX_DEPTH], eax
.depth_ok:
    mov eax, [esp + 4]
    mov [LAST_SIGNO], eax
    mov esi, [esp + 8]
    mov edi, [esp + 12]
    EQ eax, [esi + ABI_OFFSETOF_CIUKI_SIGINFO_SIGNO]
    mov eax, [esi + ABI_OFFSETOF_CIUKI_SIGINFO_VECTOR]
    mov [LAST_VECTOR], eax
    mov eax, [esi + ABI_OFFSETOF_CIUKI_SIGINFO_CODE]
    mov [LAST_CODE], eax
    mov eax, [esi + ABI_OFFSETOF_CIUKI_SIGINFO_TRAP_ERROR]
    mov [LAST_ERROR], eax
    mov eax, [esi + ABI_OFFSETOF_CIUKI_SIGINFO_FAULT_ADDR]
    mov [LAST_ADDRESS], eax
    mov eax, [esi + ABI_OFFSETOF_CIUKI_SIGINFO_SENDER_PID]
    mov [SENDER], eax
    mov eax, [edi + CREG(CIUKI_REG_EIP)]
    mov [LAST_EIP], eax
    mov eax, [edi + CREG(CIUKI_REG_EAX)]
    mov [SAVED_EAX], eax
    mov eax, [COMPLETION]
    mov [HANDLER_COMPLETION], eax
    cmp dword [MODE], 13
    jne .no_remainder
    mov eax, [REMAIN + ABI_OFFSETOF_CIUKI_TIMESPEC_TV_NSEC]
    mov [HANDLER_REMAIN], eax
.no_remainder:
    mov eax, [edi + ABI_OFFSETOF_CIUKI_UCONTEXT_MASK]
    mov [SAVED_MASK], eax
    ; Handler x87 must start reset even after a pending unmasked exception.
    fnstcw [CW]
    EQ word [CW], 0x037f
    fnstsw ax
    EQ ax, 0
    fld1
    fstp st0
    cmp dword [MODE], 0
    je .repair_fault
    cmp dword [MODE], 6
    je .nested_fault
    cmp dword [MODE], 7
    je .forge
    cmp dword [MODE], 10
    je .blocking
    cmp dword [MODE], 11
    je .blocking
    cmp dword [MODE], 12
    je .blocking
    cmp dword [MODE], 14
    je .target
    jmp .return
.repair_fault:
    mov eax, [EXPECT_VECTOR]
    EQ eax, [LAST_VECTOR]
    EQ eax, [edi + CREG(CIUKI_REG_VECTOR)]
    mov eax, [EXPECT_SIGNO]
    EQ eax, [LAST_SIGNO]
    mov eax, [EXPECT_ERROR]
    EQ eax, [LAST_ERROR]
    EQ eax, [edi + CREG(CIUKI_REG_ERROR)]
    mov eax, [EXPECT_CODE]
    EQ eax, [LAST_CODE]
    mov eax, [EXPECT_EIP]
    EQ eax, [LAST_EIP]
    mov eax, [EXPECT_ADDRESS]
    EQ eax, [LAST_ADDRESS]
    EQ dword [SENDER], 0
    EQ dword [edi + CREG(CIUKI_REG_EDI)], 0x12345678
    EQ dword [edi + CREG(CIUKI_REG_ESI)], 0x87654321
    EQ dword [edi + CREG(CIUKI_REG_EBP)], 0x13579bdf
    mov eax, [RESUME]
    mov [edi + CREG(CIUKI_REG_EIP)], eax
    cmp dword [LAST_VECTOR], 16
    je .repair_fp
    mov eax, [edi + ABI_OFFSETOF_CIUKI_UCONTEXT_FP_STATE + 28]
    xor eax, [edi + ABI_OFFSETOF_CIUKI_UCONTEXT_FP_STATE + 32]
    EQ eax, [FP_DIGEST]
    jmp .return
.repair_fp:
    or word [edi + ABI_OFFSETOF_CIUKI_UCONTEXT_FP_STATE], 0x3f
    and word [edi + ABI_OFFSETOF_CIUKI_UCONTEXT_FP_STATE + 4], 0x7f00
    jmp .return
.nested_fault:
    ud2
.forge:
    xor dword [esp + ABI_OFFSETOF_CIUKI_SIGNAL_FRAME_TOKEN], 1
    jmp .return
.target:
    mov eax, [gs:ABI_OFFSETOF_CIUKI_TCB_TID]
    EQ eax, [PEER_TID]
    mov dword [ORDER], 1
    jmp .return
.blocking:
    cmp dword [LAST_SIGNO], SIGUSR2
    je .inner
    mov dword [ORDER], 1
    call sample_start
    mov dword [STAGE], 2
    call sleep20
    mov [SLEEP_RESULT], eax
    EQ eax, 0
    call sample_end
    mov eax, [LAST_TICK]
    sub eax, [FIRST_TICK]
    cmp eax, 20
    jae .slept
    inc dword [ERRORS]
.slept:
    EQ dword [ENTRIES], 1
    mov dword [ORDER], 2
    jmp .return
.inner:
    EQ dword [ORDER], 2
    mov dword [ORDER], 3
.return:
    cmp dword [MODE], 0
    jne .reported
    mov eax, [ENTRIES]
    add eax, 50
    mov [STAGE], eax
.report_wait:
    cmp dword [STAGE], 0
    je .reported
    CALL CIUKI_SYS_YIELD
    jmp .report_wait
.reported:
    dec dword [DEPTH]
    inc dword [RETURNS]
    ret
restorer:
    lea ebx, [esp - 4]
    CALL CIUKI_SYS_SIGRETURN
    ud2
survivor:
    mov dword [STAGE], 1
.loop:
    inc dword [PROGRESS]
    cmp dword [RELEASE], 0
    je .loop
    jmp exit_ok
done:
    mov dword [STAGE], 100
.wait_release:
    cmp dword [RELEASE], 0
    jne exit_ok
    CALL CIUKI_SYS_YIELD
    jmp .wait_release
exit_ok:
    mov ebx, [ERRORS]
    CALL CIUKI_SYS_EXIT
    ud2
fail_exit:
    mov ebx, 99
    CALL CIUKI_SYS_EXIT
    ud2
signals: dd SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGUSR1, SIGUSR2, 0
code_end:
    times 5*CIUKI_PAGE_SIZE-($-$$) db 0
    dd 0
