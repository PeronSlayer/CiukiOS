/*
 * OPENGEM-044 Stage 3C — v86 entry body (PM32, paging off).
 *
 * Called from Task A's mode_switch_run_legacy_pm after a full transition
 * out of IA-32e. Executes the first IRETL that brings the CPU into
 * virtual-8086 mode, catches the guest's first trap via a minimal IDT,
 * and returns to the caller.
 *
 * Runtime layout (static, within stage2 .data/.rodata, low 4 GiB):
 *   v86_code_page          : 16-byte page @ linear X containing HLT (0xF4)
 *   legacy_v86_live_idt    : 256 gates × 8 bytes (only #GP vector 0x0D wired)
 *   legacy_v86_live_idtr   : 6-byte limit:base pseudo-descriptor
 *   legacy_v86_live_saved_esp : scratch for #GP-unwind
 *
 * v86 IRET frame pushed on stack (top→bottom in IRETL pop order):
 *   EIP       (v86 target IP      = 0x0000)
 *   CS        (v86 target segment = v86_code_page_seg)
 *   EFLAGS    (VM=1, IOPL=0, IF=0, reserved1=1)
 *   ESP       (v86 stack pointer  = 0x0100)
 *   SS        (v86 stack segment  = 0x2000)
 *   ES, DS, FS, GS (all 0)
 *
 * In v86 mode, ANY privileged instruction (HLT included) faults to #GP
 * on the host IDT. Our #GP stub writes "V86!" to debugcon and unwinds
 * directly to the body's return site via saved ESP.
 *
 * CAVEATS:
 * - This asm MUST NOT be executed unless every arm flag is set. The
 *   C-side gate in legacy_v86_live.c enforces this.
 * - The v86 code page lives in the stage2 image at a link-time-known
 *   linear address; we derive the v86 CS from (page_linear >> 4).
 * - The body uses a data32 code selector value 0x10 (CODE32_SEL) per
 *   mode_switch_asm.S's legacy GDT. DO NOT change without coordinating.
 */

    /* ---- .rodata: sentinel + v86 code page + IDTR pseudo-descriptor ---- */

    .section .rodata
    .global legacy_v86_v86_body_sentinel
legacy_v86_v86_body_sentinel:
    .asciz "V86-BODY-3C"

    /* v86 code page: HLT then an infinite jmp. Aligned to 16 so the
     * linear base is paragraph-aligned (v86 CS = linear >> 4). */
    .align 16
    .global legacy_v86_live_v86_code
legacy_v86_live_v86_code:
    .byte 0xF4                   /* HLT → #GP in v86 */
    .byte 0xEB, 0xFD             /* jmp .-1 (paranoia if HLT ever returned) */
    .fill 13, 1, 0x90            /* pad paragraph with NOPs */

    /* ---- .bss: IDT + scratch ---- */

    .section .bss
    .align 8
    .global legacy_v86_live_idt
legacy_v86_live_idt:
    .skip 256 * 8                /* 256 gates × 8 bytes */

    .align 4
    .global legacy_v86_live_saved_esp
legacy_v86_live_saved_esp:
    .skip 4

    .align 2
    .global legacy_v86_live_idtr
legacy_v86_live_idtr:
    .skip 6                      /* 2 bytes limit + 4 bytes base */

    /* ---- .text: body + #GP handler ---- */

    .section .text
    .code32

    /* ------------------------------------------------------------------ */
    /* legacy_v86_v86_body(void *user)                                    */
    /*   Entered from Task A's trampoline in legacy 32-bit PM,            */
    /*   CS=CODE32_SEL=0x10, DS/ES/FS/GS/SS=DATA32_SEL=0x18,              */
    /*   paging off, interrupts off.                                      */
    /* ------------------------------------------------------------------ */
    .global legacy_v86_v86_body
    .type legacy_v86_v86_body, @function
legacy_v86_v86_body:
    /* Prologue: we will be entering v86 and trapping back via a direct
     * jump that unwinds past IRETL. Save ESP in a fixed slot so the
     * handler can restore it before retl. */
    movl %esp, legacy_v86_live_saved_esp

    /* ---- Build IDT: all 256 gates → spurious stub, then overwrite #GP. ---- */
    /*
     * 32-bit interrupt gate format (8 bytes):
     *   +0  offset_lo (16 bits)
     *   +2  selector  (16 bits)
     *   +4  0x00  (ist=0 / reserved)
     *   +5  type/attr: 0x8E = present | ring0 | 32-bit interrupt gate
     *   +6  offset_hi (16 bits)
     */
    leal legacy_v86_live_idt, %edi
    movl $256, %ecx
._idt_fill_loop:
    movl $legacy_v86_live_spurious, %eax
    movw %ax, (%edi)                     /* offset_lo */
    movw $0x10, 2(%edi)                  /* CODE32_SEL */
    movb $0x00, 4(%edi)
    movb $0x8E, 5(%edi)                  /* present, ring0, int32 gate */
    shrl $16, %eax
    movw %ax, 6(%edi)                    /* offset_hi */
    addl $8, %edi
    loop ._idt_fill_loop

    /* Overwrite vector 0x0D (#GP) with our v86-capturing handler. */
    leal legacy_v86_live_idt, %edi
    addl $(0x0D * 8), %edi
    movl $legacy_v86_live_gp_handler, %eax
    movw %ax, (%edi)
    shrl $16, %eax
    movw %ax, 6(%edi)
    /* selector + attrs already set by fill loop */

    /* ---- Build IDTR pseudo-descriptor and load it. ---- */
    movw $(256 * 8 - 1), %ax
    movw %ax, legacy_v86_live_idtr
    movl $legacy_v86_live_idt, %eax
    movl %eax, (legacy_v86_live_idtr + 2)
    lidt legacy_v86_live_idtr

    /* ---- Compute v86 CS from v86 code page linear address. ---- */
    /* The code page is in .rodata at a link-time address. v86 CS = addr>>4.
     * We need the 16-bit seg value pushed in the IRET frame.
     * Use %ebx as scratch.
     */
    movl $legacy_v86_live_v86_code, %ebx
    shrl $4, %ebx                        /* v86 CS (paragraph number) */

    /* ---- Push v86 IRET frame, then IRETL. ---- */
    /*
     * Order of pushes (so CPU pops in reverse as IRET frame):
     *   top-of-stack after all pushes:
     *     [ESP+ 0] = EIP
     *     [ESP+ 4] = CS
     *     [ESP+ 8] = EFLAGS
     *     [ESP+12] = ESP (v86)
     *     [ESP+16] = SS
     *     [ESP+20] = ES
     *     [ESP+24] = DS
     *     [ESP+28] = FS
     *     [ESP+32] = GS
     */
    pushl $0                              /* GS  */
    pushl $0                              /* FS  */
    pushl $0                              /* DS  */
    pushl $0                              /* ES  */
    pushl $0x2000                         /* SS  (v86 stack seg, arbitrary) */
    pushl $0x00000100                     /* ESP */
    pushl $0x00020002                     /* EFLAGS: VM=1 (bit17), IF=0, reserved1=1 (bit1) */
    pushl %ebx                            /* CS  (v86 code page paragraph) */
    pushl $0                              /* EIP */
    iretl                                 /* → v86 mode at CS:0000 */

    /* Not reached. If IRETL somehow falls through, hang. */
._body_iret_fell_through:
    hlt
    jmp ._body_iret_fell_through

    /* ------------------------------------------------------------------ */
    /* legacy_v86_live_spurious                                           */
    /*   Catch-all for any unexpected vector. Marker + hlt.               */
    /* ------------------------------------------------------------------ */
    .global legacy_v86_live_spurious
legacy_v86_live_spurious:
    movw $0xE9, %dx
    movb $'?', %al
    outb %al, %dx
    movb $'?', %al
    outb %al, %dx
._spurious_spin:
    hlt
    jmp ._spurious_spin

    /* ------------------------------------------------------------------ */
    /* legacy_v86_live_gp_handler                                         */
    /*   Host #GP in 32-bit PM. On entry (from v86 HLT trap):             */
    /*     stack = [err, EIP, CS, EFLAGS, ESP, SS, ES, DS, FS, GS]        */
    /*   Actions:                                                         */
    /*     1. Write "V86!" to debugcon (port 0xE9).                       */
    /*     2. Restore ESP from saved slot.                                */
    /*     3. retl — returns to whoever called legacy_v86_v86_body.       */
    /*                                                                    */
    /*   This DELIBERATELY does not IRETL back into v86. The one-shot     */
    /*   test is: did we enter v86 and trap? If yes, unwind cleanly.      */
    /* ------------------------------------------------------------------ */
    .global legacy_v86_live_gp_handler
legacy_v86_live_gp_handler:
    /* Emit marker. */
    movw $0xE9, %dx
    movb $'V', %al
    outb %al, %dx
    movb $'8', %al
    outb %al, %dx
    movb $'6', %al
    outb %al, %dx
    movb $'!', %al
    outb %al, %dx

    /* Unwind to the body's caller (Task A's trampoline).
     * At this point %esp points somewhere inside the v86 IRET trap frame.
     * We restore the saved pre-entry ESP and return normally. */
    movl legacy_v86_live_saved_esp, %esp
    xorl %eax, %eax
    retl

    .size legacy_v86_v86_body, . - legacy_v86_v86_body

    /* NX: explicit stack-exec marker. */
    .section .note.GNU-stack,"",@progbits
