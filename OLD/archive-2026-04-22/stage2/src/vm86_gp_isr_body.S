/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPENGEM-037 - PE32 #GP ISR real body (arm-gated, never installed).
 *
 * This file introduces the real 32-bit ISR entry that OPENGEM-038+
 * will install in the PE32 IDT at vector 0x0D. It is completely
 * separate from the OPENGEM-035 halt-stub (vm86_gp_dispatch.S): that
 * symbol stays untouched as a safety anchor.
 *
 * Safety contract:
 *   - This ISR is NEVER reached in the default boot path: no IDT
 *     in 035..037 references `vm86_gp_isr_real_entry`.
 *   - The body does NOT yet emit IRETD / IRETQ / LIDT / LGDT /
 *     CR-writes. It captures the trap frame into a static area and
 *     halts deterministically. Returning from #GP via IRETD is the
 *     OPENGEM-038 pending surface.
 *   - `.code32` is mandatory: the ISR runs in the PE32 compat task.
 *
 * Hardware-pushed #GP frame on entry (all 32-bit values unless noted):
 *   [esp+ 0] error code
 *   [esp+ 4] guest EIP
 *   [esp+ 8] guest CS     (low 16 bits valid)
 *   [esp+12] guest EFLAGS
 *   [esp+16] guest ESP
 *   [esp+20] guest SS     (low 16 bits valid)
 *   [esp+24] guest ES     (v86 flavor: CPU pushes all seg selectors)
 *   [esp+28] guest DS
 *   [esp+32] guest FS
 *   [esp+36] guest GS
 *
 * Capture layout (vm86_gp_isr_capture_area, 64 bytes):
 *   +0x00 eax   +0x04 ebx   +0x08 ecx   +0x0C edx
 *   +0x10 esi   +0x14 edi   +0x18 ebp   +0x1C esp(guest)
 *   +0x20 eip   +0x24 eflags
 *   +0x28 cs    +0x2A ds    +0x2C es    +0x2E fs
 *   +0x30 gs    +0x32 ss
 *   +0x34 err   +0x38 seq   +0x3C reserved
 *
 * The capture_flag byte transitions 0 -> 1 on first entry; the seq
 * counter increments; the reserved slot is left zero. No boot code
 * ever observes these symbols, but host-side C probes may read them.
 */

    .section .rodata
    .global vm86_gp_isr_real_sentinel
vm86_gp_isr_real_sentinel:
    .asciz "OPENGEM-037"

    .section .bss
    .align 16
    .global vm86_gp_isr_capture_area
    .type   vm86_gp_isr_capture_area, @object
    .size   vm86_gp_isr_capture_area, 64
vm86_gp_isr_capture_area:
    .zero 64

    .align 4
    .global vm86_gp_isr_capture_flag
    .type   vm86_gp_isr_capture_flag, @object
    .size   vm86_gp_isr_capture_flag, 1
vm86_gp_isr_capture_flag:
    .zero 1

    .align 4
    .global vm86_gp_isr_capture_seq
    .type   vm86_gp_isr_capture_seq, @object
    .size   vm86_gp_isr_capture_seq, 4
vm86_gp_isr_capture_seq:
    .zero 4

    .section .text
    .code32
    .global vm86_gp_isr_real_entry
    .type   vm86_gp_isr_real_entry, @function
vm86_gp_isr_real_entry:
    /* --- prologue: stash segment selectors, push GPRs ---           */
    pushl   %gs                     /* [esp+ 0] gs                    */
    pushl   %fs                     /* [esp+ 4] fs                    */
    pushl   %es                     /* [esp+ 8] es                    */
    pushl   %ds                     /* [esp+12] ds                    */
    pushal                          /* 32 bytes: edi,esi,ebp,esp_h,   */
                                    /*          ebx,edx,ecx,eax       */

    /* Stack layout now (bytes from current %esp):                    */
    /*   [esp+ 0] edi                                                 */
    /*   [esp+ 4] esi                                                 */
    /*   [esp+ 8] ebp                                                 */
    /*   [esp+12] esp_handler (unused, not the guest's)               */
    /*   [esp+16] ebx                                                 */
    /*   [esp+20] edx                                                 */
    /*   [esp+24] ecx                                                 */
    /*   [esp+28] eax                                                 */
    /*   [esp+32] ds  (pushed as 32-bit)                              */
    /*   [esp+36] es                                                  */
    /*   [esp+40] fs                                                  */
    /*   [esp+44] gs                                                  */
    /*   [esp+48] error code                                          */
    /*   [esp+52] guest EIP                                           */
    /*   [esp+56] guest CS                                            */
    /*   [esp+60] guest EFLAGS                                        */
    /*   [esp+64] guest ESP                                           */
    /*   [esp+68] guest SS                                            */
    /*   [esp+72] guest ES                                            */
    /*   [esp+76] guest DS                                            */
    /*   [esp+80] guest FS                                            */
    /*   [esp+84] guest GS                                            */

    /* --- copy captured state into vm86_gp_isr_capture_area ---      */
    movl    $vm86_gp_isr_capture_area, %edi

    movl    28(%esp), %eax          /* guest EAX                      */
    movl    %eax, 0x00(%edi)
    movl    16(%esp), %eax          /* guest EBX                      */
    movl    %eax, 0x04(%edi)
    movl    24(%esp), %eax          /* guest ECX                      */
    movl    %eax, 0x08(%edi)
    movl    20(%esp), %eax          /* guest EDX                      */
    movl    %eax, 0x0C(%edi)
    movl    4(%esp), %eax           /* guest ESI                      */
    movl    %eax, 0x10(%edi)
    movl    0(%esp), %eax           /* guest EDI                      */
    movl    %eax, 0x14(%edi)
    movl    8(%esp), %eax           /* guest EBP                      */
    movl    %eax, 0x18(%edi)
    movl    64(%esp), %eax          /* guest ESP                      */
    movl    %eax, 0x1C(%edi)
    movl    52(%esp), %eax          /* guest EIP                      */
    movl    %eax, 0x20(%edi)
    movl    60(%esp), %eax          /* guest EFLAGS                   */
    movl    %eax, 0x24(%edi)

    movw    56(%esp), %ax           /* guest CS                       */
    movw    %ax, 0x28(%edi)
    movw    76(%esp), %ax           /* guest DS                       */
    movw    %ax, 0x2A(%edi)
    movw    72(%esp), %ax           /* guest ES                       */
    movw    %ax, 0x2C(%edi)
    movw    80(%esp), %ax           /* guest FS                       */
    movw    %ax, 0x2E(%edi)
    movw    84(%esp), %ax           /* guest GS                       */
    movw    %ax, 0x30(%edi)
    movw    68(%esp), %ax           /* guest SS                       */
    movw    %ax, 0x32(%edi)

    movl    48(%esp), %eax          /* error code                     */
    movl    %eax, 0x34(%edi)

    /* --- bump sequence counter, set flag --------------------------- */
    movl    vm86_gp_isr_capture_seq, %eax
    incl    %eax
    movl    %eax, vm86_gp_isr_capture_seq
    movl    %eax, 0x38(%edi)

    movb    $1, vm86_gp_isr_capture_flag

    /* --- deterministic halt (no IRETD until OPENGEM-038) ---------- */
    .byte   0xF4                    /* hlt                            */
    .byte   0xEB, 0xFE              /* jmp .                          */
    .byte   0x90, 0x90, 0x90        /* pad                            */
    .size   vm86_gp_isr_real_entry, . - vm86_gp_isr_real_entry

    .section .note.GNU-stack,"",@progbits
