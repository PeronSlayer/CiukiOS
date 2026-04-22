/*
 * stage2/src/vm86_switch.S
 *
 * OPENGEM-027: v8086 live mode-switch trampoline — stub phase.
 *
 * All entry points here are deliberate no-ops. Each `ret` is the
 * observability-only body for this phase. The symbols must exist so the
 * linker can resolve future call sites added in OPENGEM-029/030 without
 * requiring any header/ABI change at that time.
 *
 * Rules enforced by scripts/test_vm86_switch.sh:
 *   - every symbol is declared .global
 *   - every body is a minimal `ret` (no LGDT / LIDT / IRET / far jump)
 *   - the stub sentinel dword is present in .rodata
 *   - no other .S or .c file calls any symbol defined here
 */

    .section .rodata
    .global vm86_switch_stub_sentinel
    .type   vm86_switch_stub_sentinel, @object
    .align  4
vm86_switch_stub_sentinel:
    .long   0x00000270              /* VM86_SWITCH_SENTINEL, OPENGEM-027 */
    .size   vm86_switch_stub_sentinel, . - vm86_switch_stub_sentinel

    .section .text

/*
 * vm86_switch_long_to_pe32
 *
 * STUB: returns immediately. Future OPENGEM-029 body will:
 *   - push callee-saved host long-mode state
 *   - load the 32-bit GDT via LGDT
 *   - far jump through a compat-mode code selector (L=0, D=1)
 *   - disable paging + EFER.LME + re-enable classic 32-bit paging
 */
    .global vm86_switch_long_to_pe32
    .type   vm86_switch_long_to_pe32, @function
vm86_switch_long_to_pe32:
    retq
    .size   vm86_switch_long_to_pe32, . - vm86_switch_long_to_pe32

/*
 * vm86_switch_pe32_to_long
 *
 * STUB: returns immediately. Future OPENGEM-029 body will reverse the
 * long_to_pe32 sequence.
 */
    .global vm86_switch_pe32_to_long
    .type   vm86_switch_pe32_to_long, @function
vm86_switch_pe32_to_long:
    retq
    .size   vm86_switch_pe32_to_long, . - vm86_switch_pe32_to_long

/*
 * vm86_switch_enter_v86_via_iret
 *
 * STUB: returns immediately. Future OPENGEM-029 body will consume the
 * 36-byte v86 IRET frame produced by vm86_iret_encode_frame() and
 * execute the IRET that enters virtual-8086 with EFLAGS.VM=1,IOPL=3.
 */
    .global vm86_switch_enter_v86_via_iret
    .type   vm86_switch_enter_v86_via_iret, @function
vm86_switch_enter_v86_via_iret:
    retq
    .size   vm86_switch_enter_v86_via_iret, . - vm86_switch_enter_v86_via_iret

/*
 * vm86_switch_gp_trampoline
 *
 * STUB: returns immediately. Future OPENGEM-029 body will:
 *   - snapshot the pushed trap frame
 *   - decode the offending INT N in guest memory
 *   - hand off to vm86_dispatch_int()
 *   - re-execute IRET back into the guest
 */
    .global vm86_switch_gp_trampoline
    .type   vm86_switch_gp_trampoline, @function
vm86_switch_gp_trampoline:
    retq
    .size   vm86_switch_gp_trampoline, . - vm86_switch_gp_trampoline
