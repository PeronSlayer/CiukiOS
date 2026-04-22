/*
 * stage2/include/vm86_switch.h
 *
 * OPENGEM-027: symbolic trampolines for the v8086 live mode-switch.
 *
 * All entry points are stubs at this phase:
 *   - defined in stage2/src/vm86_switch.S
 *   - compiled into the kernel so their symbols are resolvable
 *   - never invoked from any live boot path (shell.c / stage2.c)
 *
 * Later phases in the 027..030 plan will fill in the bodies:
 *   - OPENGEM-028: the live-switch plan (descriptor staging, not execution)
 *   - OPENGEM-029: long-mode save/restore + armed-but-gated cross-over
 *   - OPENGEM-030: gem.exe wire-up consulting the gating flag
 *
 * This header is append-only. No existing prototype may be removed.
 */

#ifndef STAGE2_VM86_SWITCH_H
#define STAGE2_VM86_SWITCH_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The sentinel is the stable token that gate scripts grep for. It must
 * change together with the ABI. OPENGEM-027 is the first revision.
 */
#define VM86_SWITCH_SENTINEL 0x0270u

/*
 * vm86_switch_long_to_pe32
 *
 * Long-mode (ring 0) → 32-bit protected-mode compatibility task entry.
 *
 * At OPENGEM-027 this is a no-op stub: it returns immediately. It exists
 * to freeze the symbol so the call-site wired in OPENGEM-029/030 has a
 * stable target.
 */
extern void vm86_switch_long_to_pe32(void);

/*
 * vm86_switch_pe32_to_long
 *
 * Reverse of vm86_switch_long_to_pe32. Stub at OPENGEM-027.
 */
extern void vm86_switch_pe32_to_long(void);

/*
 * vm86_switch_enter_v86_via_iret
 *
 * Stub at OPENGEM-027. Future OPENGEM-029 will implement the IRET that
 * consumes a 36-byte v86 frame staged by vm86_iret_encode_frame().
 */
extern void vm86_switch_enter_v86_via_iret(void);

/*
 * vm86_switch_gp_trampoline
 *
 * Stub #GP(0x0D) entry used by the IDT gate built in OPENGEM-026. At
 * OPENGEM-027 this is a no-op; the real decoder lands in OPENGEM-029.
 */
extern void vm86_switch_gp_trampoline(void);

/*
 * vm86_switch_stub_sentinel
 *
 * A literal integer value defined inside vm86_switch.S that the gate
 * can observe via nm / objdump symbol dumps when needed. Kept as a data
 * symbol to guarantee the assembly file is linked in.
 */
extern const unsigned int vm86_switch_stub_sentinel;

#ifdef __cplusplus
}
#endif

#endif /* STAGE2_VM86_SWITCH_H */
