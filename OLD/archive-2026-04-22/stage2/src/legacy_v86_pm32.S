/*
 * OPENGEM-044 Task B — real legacy PM32 -> v86 body.
 *
 * Called from Task A's trampoline in legacy 32-bit PM, paging off.
 * Input ABI: cdecl, one argument = legacy_v86_context_t *.
 *
 * Behavior:
 * 1. Emit existing debug marker string OPENGEM-044-B for continuity.
 * 2. Build a minimal 32-bit IDT with a #GP handler.
 * 3. Push a VM86 IRETL frame from the caller-provided entry state.
 * 4. IRETL into guest v86 code.
 * 5. On the first #GP, capture full VM86 state into out->frame,
 *    set out->reason = GP_INT and out->int_vector = 0x0D, restore the
 *    pre-entry ESP, and retl back to Task A's trampoline.
 */

    .set CTX_ENTRY,      0x00
    .set CTX_OUT,        0x08

    .set EXIT_REASON,    0x00
    .set EXIT_INTVEC,    0x04
    .set EXIT_FRAME,     0x08
    .set EXIT_FAULT,     0x20

    .set FRAME_CS,       0x00
    .set FRAME_IP,       0x02
    .set FRAME_SS,       0x04
    .set FRAME_SP,       0x06
    .set FRAME_DS,       0x08
    .set FRAME_ES,       0x0A
    .set FRAME_FS,       0x0C
    .set FRAME_GS,       0x0E
    .set FRAME_EFLAGS,   0x10
    .set FRAME_R0,       0x14
    .set FRAME_R1,       0x18
    .set FRAME_R2,       0x1C
    .set FRAME_R3,       0x20
    .set FRAME_R4,       0x24
    .set FRAME_R5,       0x28

    .set EXIT_REASON_NORMAL, 0
    .set EXIT_REASON_GP_INT, 1
    .set EXIT_REASON_HALT,   2
    .set EXIT_REASON_FAULT,  3

    .set STUB_INT20_LINEAR,  0x00000700
    .set STUB_INT21_LINEAR,  0x00000710
    .set STUB_INT03_LINEAR,  0x00000720
    .set STUB_INT20_SEG,     (STUB_INT20_LINEAR >> 4)
    .set STUB_INT21_SEG,     (STUB_INT21_LINEAR >> 4)
    .set STUB_INT03_SEG,     (STUB_INT03_LINEAR >> 4)
    .set SCR_PM32_STACK,     0x88
    .set TSS32_SEL,          0x20
    .set TSS_ESP0,           0x04
    .set TSS_SS0,            0x08
    .set TSS_IOMAP,          0x66

    .section .rodata
    .global legacy_v86_pm32_sentinel
legacy_v86_pm32_sentinel:
    .asciz "OPENGEM-044-B"

    .align 16
    .global legacy_v86_pm32_v86_code
legacy_v86_pm32_v86_code:
    .byte 0xCD, 0x21             /* INT 21h */
    .byte 0xF4                   /* HLT if dispatch ever irets back */
    .byte 0xEB, 0xFD             /* jmp .-1 paranoia */
    .fill 12, 1, 0x90

    .section .bss
    .align 8
    .global legacy_v86_pm32_idt
legacy_v86_pm32_idt:
    .skip 256 * 8

        .align 4
        .global legacy_v86_pm32_saved_ret
    legacy_v86_pm32_saved_ret:
        .long 0

    .align 4
    .global legacy_v86_pm32_saved_esp
legacy_v86_pm32_saved_esp:
    .skip 4

    .align 4
    .global legacy_v86_pm32_saved_ctx
legacy_v86_pm32_saved_ctx:
    .skip 4

    .align 16
    .global legacy_v86_pm32_tss
legacy_v86_pm32_tss:
    .skip 104

    .align 2
    .global legacy_v86_pm32_idtr
legacy_v86_pm32_idtr:
    .skip 6

    .section .text
    .code32
    .global legacy_v86_pm32_body
    .type legacy_v86_pm32_body, @function
legacy_v86_pm32_body:
    call    .L_body_base
.L_body_base:
    popl    %ebp
    subl    $(.L_body_base - legacy_v86_pm32_body), %ebp

    /* cdecl arg: [esp+4] = legacy_v86_context_t* */
    movl    4(%esp), %esi
    movl    %esi, legacy_v86_pm32_saved_ctx - legacy_v86_pm32_body(%ebp)
    movl    0(%esp), %eax
    movl    %eax, legacy_v86_pm32_saved_ret - legacy_v86_pm32_body(%ebp)
    movl    %esp, legacy_v86_pm32_saved_esp - legacy_v86_pm32_body(%ebp)

    /* entry marker: lowercase 'b' on 0xE9 */
    movb    $'b', %al
    outb    %al, $0xE9

    movw    $0x00E9, %dx

    movb    $'O', %al
    outb    %al, %dx
    movb    $'P', %al
    outb    %al, %dx
    movb    $'E', %al
    outb    %al, %dx
    movb    $'N', %al
    outb    %al, %dx
    movb    $'G', %al
    outb    %al, %dx
    movb    $'E', %al
    outb    %al, %dx
    movb    $'M', %al
    outb    %al, %dx
    movb    $'-', %al
    outb    %al, %dx
    movb    $'0', %al
    outb    %al, %dx
    movb    $'4', %al
    outb    %al, %dx
    movb    $'4', %al
    outb    %al, %dx
    movb    $'-', %al
    outb    %al, %dx
    movb    $'B', %al
    outb    %al, %dx
    movb    $'c', %al
    outb    %al, $0xE9

    /* Minimal IDT: fill all vectors with spurious, then overwrite #GP. */
    leal    legacy_v86_pm32_idt - legacy_v86_pm32_body(%ebp), %edi
    movl    $256, %ecx
.L_idt_fill:
    leal    legacy_v86_pm32_spurious - legacy_v86_pm32_body(%ebp), %eax
    movw    %ax, (%edi)
    movw    $0x10, 2(%edi)
    movb    $0x00, 4(%edi)
    movb    $0x8E, 5(%edi)
    shrl    $16, %eax
    movw    %ax, 6(%edi)
    addl    $8, %edi
    loop    .L_idt_fill

    leal    legacy_v86_pm32_idt - legacy_v86_pm32_body(%ebp), %edi
    addl    $(0x0D * 8), %edi
    leal    legacy_v86_pm32_gp_handler - legacy_v86_pm32_body(%ebp), %eax
    movw    %ax, (%edi)
    shrl    $16, %eax
    movw    %ax, 6(%edi)

    movw    $(256 * 8 - 1), %ax
    movw    %ax, legacy_v86_pm32_idtr - legacy_v86_pm32_body(%ebp)
    leal    legacy_v86_pm32_idt - legacy_v86_pm32_body(%ebp), %eax
    movl    %eax, legacy_v86_pm32_idtr + 2 - legacy_v86_pm32_body(%ebp)
    leal    legacy_v86_pm32_idtr - legacy_v86_pm32_body(%ebp), %eax
    lidt    (%eax)
    movb    $'d', %al
    outb    %al, $0xE9

    /* Install low-memory real-mode interrupt stubs. Each stub executes
     * HLT. With VM=1 and IOPL=3 this remains privileged and returns to
     * the host #GP path, while ordinary guest CLI/STI can execute per the
     * Task-B contract. The handler recognizes the stub CS and reconstructs
     * the pre-INT guest return frame from the v86 stack. This turns the
     * guest's first INT 03h/20h/21h into a typed host-side exit without
     * needing the full DOS IVT surface yet. */
    movl    $STUB_INT20_LINEAR, %edi
    movb    $0xF4, 0(%edi)              /* hlt */
    movb    $0xEB, 1(%edi)
    movb    $0xFD, 2(%edi)
    movl    $STUB_INT21_LINEAR, %edi
    movb    $0xF4, 0(%edi)
    movb    $0xEB, 1(%edi)
    movb    $0xFD, 2(%edi)
    movl    $STUB_INT03_LINEAR, %edi
    movb    $0xF4, 0(%edi)
    movb    $0xEB, 1(%edi)
    movb    $0xFD, 2(%edi)

    movl    $0x0000000C, %edi          /* IVT[03h] */
    movw    $0x0000, 0(%edi)
    movw    $STUB_INT03_SEG, 2(%edi)
    movl    $0x00000080, %edi          /* IVT[20h] */
    movw    $0x0000, 0(%edi)
    movw    $STUB_INT20_SEG, 2(%edi)
    movl    $0x00000084, %edi          /* IVT[21h] */
    movw    $0x0000, 0(%edi)
    movw    $STUB_INT21_SEG, 2(%edi)
    movb    $'e', %al
    outb    %al, $0xE9

    /* Build/load a real TSS32 so v86 faults can switch back to ring0 on
     * a known stack. Without this, the first guest trap triple-faults
     * before our PM32 handler runs. %ebx still holds Task A scratch. */
    leal    legacy_v86_pm32_tss - legacy_v86_pm32_body(%ebp), %edi
    xorl    %eax, %eax
    movl    $(104 / 4), %ecx
    rep stosl
    movl    SCR_PM32_STACK(%ebx), %eax
    movl    %eax, legacy_v86_pm32_tss + TSS_ESP0 - legacy_v86_pm32_body(%ebp)
    movw    $0x18, legacy_v86_pm32_tss + TSS_SS0 - legacy_v86_pm32_body(%ebp)
    movw    $104, legacy_v86_pm32_tss + TSS_IOMAP - legacy_v86_pm32_body(%ebp)
    movw    $TSS32_SEL, %ax
    ltr     %ax
    movb    $'h', %al
    outb    %al, $0xE9

    /* CRITICAL: Save ESP BEFORE building v86 frame.
     * The body stack has [esp+0]=return_address and [esp+4]=body_user.
     * When v86 traps, handler needs to restore this ESP to access the return address. */
    movl    %esp, legacy_v86_pm32_saved_esp - legacy_v86_pm32_body(%ebp)

    /* Build v86 IRET frame from caller entry state. */
    movl    CTX_ENTRY(%esi), %edi
    pushl   FRAME_GS(%edi)
    pushl   FRAME_FS(%edi)
    pushl   FRAME_DS(%edi)
    pushl   FRAME_ES(%edi)
    movzwl  FRAME_SS(%edi), %eax
    /* SS in v86 mode: use CODE32_SEL (0x10) which has base=0.
     * In v86 mode, segment registers use real-mode addressing: (SELECTOR<<4)+OFFSET.
     * So (0x10<<4)+OFFSET = 0x100+OFFSET. This is a temporary workaround;
     * the proper fix requires setting up the image at an address that maps
     * correctly via the flat model. */
    /* SS: Use actual segment value from frame, not selector */
    movzwl  FRAME_SS(%edi), %eax
    pushl   %eax
    movzwl  FRAME_SP(%edi), %eax
    pushl   %eax
    movl    FRAME_EFLAGS(%edi), %eax
    orl     $0x00023000, %eax     /* VM=1, IOPL=3, bit1 reserved */
    pushl   %eax
    /* CS: Use actual segment value from frame, not selector */
    movzwl  FRAME_CS(%edi), %eax
    pushl   %eax
    movzwl  FRAME_IP(%edi), %eax
    pushl   %eax
    /* Keep the contract explicit in trace: we are entering v86 with
     * VM=1 and IOPL=3 as specified by OPENGEM-044 Task B. */
    movb    $'g', %al
    outb    %al, $0xE9
    movb    $'f', %al
    outb    %al, $0xE9
    /* Restore guest GPRs from frame.reserved[0..5] = EAX/EBX/ECX/EDX/ESI/EDI so
     * the host can return syscall results to the v86 guest. EDI is the frame
     * pointer so it must be loaded LAST. */
    movl    FRAME_R1(%edi), %ebx
    movl    FRAME_R2(%edi), %ecx
    movl    FRAME_R3(%edi), %edx
    movl    FRAME_R4(%edi), %esi
    movl    FRAME_R0(%edi), %eax
    movl    FRAME_R5(%edi), %edi
    iretl

legacy_v86_pm32_spurious:
    movw    $0x00E9, %dx
    movb    $'?', %al
    outb    %al, %dx
    movb    $'?', %al
    outb    %al, %dx
.L_spurious_spin:
    hlt
    jmp     .L_spurious_spin

legacy_v86_pm32_gp_handler:
    /* Preserve guest ESI/EDI on the kernel stack BEFORE any clobber
     * so we can snapshot them once the frame pointer is available. */
    pushl   %esi
    pushl   %edi
    /* Guest 16-bit code is free to modify BP, so EBP cannot be trusted
     * across VM86 traps. Recompute our PIC base on every handler entry. */
    call    .L_gp_base
.L_gp_base:
    popl    %ebp
    subl    $(.L_gp_base - legacy_v86_pm32_body), %ebp

    /* Trap frame on entry from v86 for #GP with error code (after the
     * two pushl above shifted everything by 8 bytes):
     * [esp+00] saved guest edi
     * [esp+04] saved guest esi
     * [esp+08] error code
     * [esp+12] eip
     * [esp+16] cs
     * [esp+20] eflags
     * [esp+24] esp
     * [esp+28] ss
     * [esp+32] es
     * [esp+36] ds
     * [esp+40] fs
     * [esp+44] gs
     */
    /* Snapshot guest GPRs before we repurpose them in the handler.
     * reserved[0..5] are the agreed runtime ABI slots for EAX..EDX,ESI,EDI. */
    movl    legacy_v86_pm32_saved_ctx - legacy_v86_pm32_body(%ebp), %esi
    movl    CTX_OUT(%esi), %edi
    movl    %eax, EXIT_FRAME + FRAME_R0(%edi)
    movl    %ebx, EXIT_FRAME + FRAME_R1(%edi)
    movl    %ecx, EXIT_FRAME + FRAME_R2(%edi)
    movl    %edx, EXIT_FRAME + FRAME_R3(%edi)
    /* Pop stashed guest EDI then ESI into frame. Use %eax (already saved). */
    popl    %eax                        /* guest EDI */
    movl    %eax, EXIT_FRAME + FRAME_R5(%edi)
    popl    %eax                        /* guest ESI */
    movl    %eax, EXIT_FRAME + FRAME_R4(%edi)

    movw    $0x00E9, %dx
    movb    $'V', %al
    outb    %al, %dx
    movb    $'8', %al
    outb    %al, %dx
    movb    $'6', %al
    outb    %al, %dx
    movb    $'!', %al
    outb    %al, %dx

    /* Emit CS:IP at trap point (hex debug). */
    movw    8(%esp), %ax
    movb    $'[', %al
    outb    %al, $0xE9
    movw    8(%esp), %ax
    movb    %ah, %al
    shr     $4, %al
    cmp     $9, %al
    jbe     .L_hex_cs_h_dec2
    add     $('A' - 10), %al
    jmp     .L_hex_cs_h_out2
.L_hex_cs_h_dec2:
    add     $'0', %al
.L_hex_cs_h_out2:
    outb    %al, $0xE9
    movw    8(%esp), %ax
    movb    %al, %al
    and     $0x0F, %al
    cmp     $9, %al
    jbe     .L_hex_cs_l_dec2
    add     $('A' - 10), %al
    jmp     .L_hex_cs_l_out2
.L_hex_cs_l_dec2:
    add     $'0', %al
.L_hex_cs_l_out2:
    outb    %al, $0xE9
    movb    $':', %al
    outb    %al, $0xE9
    movw    4(%esp), %ax
    movb    %ah, %al
    shr     $4, %al
    cmp     $9, %al
    jbe     .L_hex_ip_h_dec2
    add     $('A' - 10), %al
    jmp     .L_hex_ip_h_out2
.L_hex_ip_h_dec2:
    add     $'0', %al
.L_hex_ip_h_out2:
    outb    %al, $0xE9
    movw    4(%esp), %ax
    movb    %al, %al
    and     $0x0F, %al
    cmp     $9, %al
    jbe     .L_hex_ip_l_dec2
    add     $('A' - 10), %al
    jmp     .L_hex_ip_l_out2
.L_hex_ip_l_dec2:
    add     $'0', %al
.L_hex_ip_l_out2:
    outb    %al, $0xE9
    movb    $']', %al
    outb    %al, $0xE9

    /* Default classification if opcode decode below does not match a
     * supported first-trap pattern. */
    movl    $EXIT_REASON_FAULT, EXIT_REASON(%edi)
    movb    $0x00, EXIT_INTVEC(%edi)
    movl    0(%esp), %eax
    movl    %eax, EXIT_FAULT(%edi)

    movw    8(%esp), %ax
    movw    %ax, EXIT_FRAME + FRAME_CS(%edi)
    movw    4(%esp), %ax
    movw    %ax, EXIT_FRAME + FRAME_IP(%edi)
    movw    20(%esp), %ax
    movw    %ax, EXIT_FRAME + FRAME_SS(%edi)
    movw    16(%esp), %ax
    movw    %ax, EXIT_FRAME + FRAME_SP(%edi)
    movw    28(%esp), %ax
    movw    %ax, EXIT_FRAME + FRAME_DS(%edi)
    movw    24(%esp), %ax
    movw    %ax, EXIT_FRAME + FRAME_ES(%edi)
    movw    32(%esp), %ax
    movw    %ax, EXIT_FRAME + FRAME_FS(%edi)
    movw    36(%esp), %ax
    movw    %ax, EXIT_FRAME + FRAME_GS(%edi)
    movl    12(%esp), %eax
    movl    %eax, EXIT_FRAME + FRAME_EFLAGS(%edi)

    /* If we trapped in one of our real-mode INT stubs, reconstruct the
     * caller-visible post-INT state from the v86 stack frame that the
     * CPU built for the software interrupt. */
    movzwl  EXIT_FRAME + FRAME_IP(%edi), %eax
    cmpw    $0x0000, %ax
    jne     .L_decode_opcode
    movzwl  EXIT_FRAME + FRAME_CS(%edi), %eax
    cmpw    $STUB_INT03_SEG, %ax
    je      .L_stub_int03
    cmpw    $STUB_INT20_SEG, %ax
    je      .L_stub_int20
    cmpw    $STUB_INT21_SEG, %ax
    je      .L_stub_int21
    jmp     .L_decode_opcode

.L_stub_int03:
    movb    $'I', %al
    outb    %al, $0xE9
    movb    $'3', %al
    outb    %al, $0xE9
    movb    $0x03, EXIT_INTVEC(%edi)
    movl    $EXIT_REASON_GP_INT, EXIT_REASON(%edi)
    movl    $0, EXIT_FAULT(%edi)
    jmp     .L_unwind_softint_frame

.L_stub_int20:
    movb    $'I', %al
    outb    %al, $0xE9
    movb    $'2', %al
    outb    %al, $0xE9
    movb    $'0', %al
    outb    %al, $0xE9
    movb    $0x20, EXIT_INTVEC(%edi)
    movl    $EXIT_REASON_NORMAL, EXIT_REASON(%edi)
    movl    $0, EXIT_FAULT(%edi)
    jmp     .L_unwind_softint_frame

.L_stub_int21:
    movb    $'I', %al
    outb    %al, $0xE9
    movb    $'2', %al
    outb    %al, $0xE9
    movb    $'1', %al
    outb    %al, $0xE9
    movb    $0x21, EXIT_INTVEC(%edi)
    movl    $EXIT_REASON_GP_INT, EXIT_REASON(%edi)
    movl    $0, EXIT_FAULT(%edi)
    jmp     .L_unwind_softint_frame

.L_unwind_softint_frame:
    movzwl  EXIT_FRAME + FRAME_SS(%edi), %ecx
    shll    $4, %ecx
    movzwl  EXIT_FRAME + FRAME_SP(%edi), %eax
    addl    %eax, %ecx                    /* ECX = guest int return frame */
    movw    0(%ecx), %ax                  /* old IP */
    movw    %ax, EXIT_FRAME + FRAME_IP(%edi)
    movw    2(%ecx), %ax                  /* old CS */
    movw    %ax, EXIT_FRAME + FRAME_CS(%edi)
    movl    4(%ecx), %eax                 /* old FLAGS */
    movl    %eax, EXIT_FRAME + FRAME_EFLAGS(%edi)
    movzwl  EXIT_FRAME + FRAME_SP(%edi), %eax
    addw    $6, %ax
    movw    %ax, EXIT_FRAME + FRAME_SP(%edi)
    jmp     .L_decode_done

    /* Decode the faulting v86 opcode so software INT/HLT become typed
     * exits instead of opaque host #GP faults. */
.L_decode_opcode:
    movzwl  EXIT_FRAME + FRAME_CS(%edi), %ecx
    shll    $4, %ecx
    movzwl  EXIT_FRAME + FRAME_IP(%edi), %eax
    addl    %eax, %ecx                   /* ECX = linear guest PC */
    movzbl  (%ecx), %eax
    cmpb    $0xCD, %al                   /* INT imm8 */
    je      .L_decode_int_imm
    cmpb    $0xCC, %al                   /* INT3 */
    je      .L_decode_int3
    cmpb    $0xF4, %al                   /* HLT */
    je      .L_decode_hlt
    /* Unknown-opcode trace: emit 'X<hex>' on debugcon so we can see
     * which instruction generated the #GP when it's not INT/HLT.
     * EAX still holds the faulting byte here. */
    pushl   %eax
    movb    $'X', %al
    outb    %al, $0xE9
    popl    %eax
    pushl   %eax
    movb    %al, %ah
    shr     $4, %al
    andb    $0x0F, %al
    cmpb    $9, %al
    jbe     1f
    addb    $('A' - 10), %al
    jmp     2f
1:  addb    $'0', %al
2:  outb    %al, $0xE9
    popl    %eax
    pushl   %eax
    andb    $0x0F, %al
    cmpb    $9, %al
    jbe     3f
    addb    $('A' - 10), %al
    jmp     4f
3:  addb    $'0', %al
4:  outb    %al, $0xE9
    popl    %eax
    jmp     .L_decode_done

.L_decode_int_imm:
    movzbl  1(%ecx), %eax
    movb    %al, EXIT_INTVEC(%edi)
    movzwl  EXIT_FRAME + FRAME_IP(%edi), %ecx
    addw    $2, %cx
    movw    %cx, EXIT_FRAME + FRAME_IP(%edi)
    cmpl    $0x20, %eax
    je      .L_decode_normal
    movl    $EXIT_REASON_GP_INT, EXIT_REASON(%edi)
    movl    $0, EXIT_FAULT(%edi)
    jmp     .L_decode_done

.L_decode_int3:
    movb    $0x03, EXIT_INTVEC(%edi)
    movzwl  EXIT_FRAME + FRAME_IP(%edi), %ecx
    addw    $1, %cx
    movw    %cx, EXIT_FRAME + FRAME_IP(%edi)
    movl    $EXIT_REASON_GP_INT, EXIT_REASON(%edi)
    movl    $0, EXIT_FAULT(%edi)
    jmp     .L_decode_done

.L_decode_hlt:
    movzwl  EXIT_FRAME + FRAME_IP(%edi), %ecx
    addw    $1, %cx
    movw    %cx, EXIT_FRAME + FRAME_IP(%edi)
    movl    $EXIT_REASON_HALT, EXIT_REASON(%edi)
    movl    $0, EXIT_FAULT(%edi)
    jmp     .L_decode_done

.L_decode_normal:
    movl    $EXIT_REASON_NORMAL, EXIT_REASON(%edi)
    movl    $0, EXIT_FAULT(%edi)

.L_decode_done:

    /* exit marker: lowercase 'r' on 0xE9 just before retl */
    movb    $'r', %al
    outb    %al, $0xE9

    /* Debug: about to restore ESP and jump to exit label */
    movb    $'E', %al
    outb    %al, $0xE9
    movb    $'S', %al
    outb    %al, $0xE9
    movb    $'P', %al
    outb    %al, $0xE9
    /* CRITICAL FIX: Extract old EIP from trap frame BEFORE changing ESP.
     * The trap frame is at [esp+0..40], and old EIP is at [esp+4].
     * We must preserve this EIP to use with retl below. */
    movl    4(%esp), %eax       /* Save old EIP from trap frame */
    movb    $'L', %al
    outb    %al, $0xE9
    movb    $'D', %al
    outb    %al, $0xE9
    movl    legacy_v86_pm32_saved_esp - legacy_v86_pm32_body(%ebp), %esp
    movb    $'P', %al
    outb    %al, $0xE9
    movb    $'S', %al
    outb    %al, $0xE9
    movb    $'H', %al
    outb    %al, $0xE9
    /* Now push the exit label address so retl jumps back to body exit. */
    leal    .L_v86_exit - legacy_v86_pm32_body(%ebp), %ecx
    pushl   %ecx
    movb    $'J', %al
    outb    %al, $0xE9
    movb    $'M', %al
    outb    %al, $0xE9
    movb    $'P', %al
    outb    %al, $0xE9
    xorl    %eax, %eax
    retl

.L_v86_exit:
    /* Handler returns here after trap. Exit state is in legacy_v86_exit_t. */
    movb    $'X', %al
    outb    %al, $0xE9
    movb    $'!', %al
    outb    %al, $0xE9
    /* Body now returns to mode_switch_asm. */
    xorl    %eax, %eax
    movb    $'E', %al
    outb    %al, $0xE9
    movb    $'X', %al
    outb    %al, $0xE9
    movb    $'I', %al
    outb    %al, $0xE9
    movb    $'T', %al
    outb    %al, $0xE9
    addl    $4, %esp
    movl    legacy_v86_pm32_saved_ret - legacy_v86_pm32_body(%ebp), %ecx
    jmp     *%ecx
    .code64
    .size legacy_v86_pm32_body, .-legacy_v86_pm32_body

    .section .note.GNU-stack,"",@progbits