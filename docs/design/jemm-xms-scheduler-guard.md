# Jemm XMS critical sections and VM scheduling

Status: guard patch builds and runs, but is not a confirmed cause or fix.

## Evidence and decision

CiukiOS schedules its private DOS VMs from a physical IRQ0 callback. Jemm's pinned source (`e96bb6bb80cbdc3e6585544db79a6b133a5566dd`, recorded in `third_party/jemm/UPSTREAM.json`) dispatches the XMS V86 breakpoint to `xms_handler`. The handler first calls `Simulate_Far_Ret`, then processes the service. For XMS block moves, `xms_move_emb` calls `MoveMemoryPhys`; its conventional (<1 MiB) operands remain linear addresses, while extended-memory addresses are mapped as physical pages. `MoveMemoryPhys` calls `MoveMemory`, which may enable hardware interrupts around its copy when the guest IF bit is set (`JEMM32.ASM`). This means IRQ0 may reach the host scheduler while XMS processing is still active, even though the client far-return frame has already been prepared.

The local scheduler switches per-VM low-memory PTEs and reloads CR3 in `src/vm/session_vmm.inc:vmm_switch`. The source does not establish that every monitor helper or temporary mapping is safe to abandon mid-XMS call. The conservative decision is to expose a monitor-wide XMS-active depth bit through the existing versioned `Host_Scheduler_Profile` query and defer all VM switches while that depth is nonzero. The depth wraps the whole `xms_handler`, including `Simulate_Far_Ret`, register-result writes, and every handler exit. A counter rather than a boolean preserves correctness if XMS handling is nested.

The guard is preventive, not a confirmed root-cause fix. In the actual full-image run `build/full/t23-vbe-fix/music-xms-guard-runtime`, WAV and MP3 reached EOF, then advancing to Ogg failed with `#06 CS:IP=3146:1197 SS:ESP=1180:F554`. The captured Jemm guard signature `CVXMSGD1` is at physical `0x17EC08`; its XMS-depth field is zero at `0x17EC10` in both the pre-worker and failure dumps. This run therefore shows the failure with no active XMS handler at the captured fault; it does not implicate or exonerate a prior scheduling event. The previous `3146:1195` report and this `3146:1197` report remain unexplained.

## Next diagnostic: memory-only far-return trace

Do not use serial output, breakpoints, or debugger stops: previous debugger attachment changed the failure timing. The optional `--farret-trace` build uses the `CVFRET01` magic and a fixed 32-entry ring in Jemm's shared monitor data. Each 40-byte record captures sequence, operation kind (`0=Simulate_Far_Ret`, `1=Simulate_Far_Call`), pre-operation guest `SS:SP`, `CS:EIP`, `EAX`, target `CS:IP`, the protected-mode caller return address, `Client_Int`, and `Client_EFlags`. For `Simulate_Far_Ret`, the target is read from the same wrapped guest stack words as the upstream routine; for `Simulate_Far_Call`, it is the incoming `CX:DX` target. A bounded count/index identifies the newest records. On the first V86 invalid-opcode exception, the trace freezes and stores the fault `CS`, `IP`, `SS`, `SP`, `EAX`, flags, and exception number before the normal formatter can issue more calls. The recorder wraps its work in `PUSHFD/PUSHAD` and restores both before the original routine, preserving its register, flag, and stack behavior. It performs no I/O. The failure RAM capture can then show whether the bad guest frame was prepared by Jemm's simulated far-call/return path or appeared later through another write. Enable it only with this explicit diagnostic build option; ordinary builds remain unchanged.

Source-level check: pinned `JEMM32.ASM` implements `Simulate_Far_Ret` by reading `IP` at `SS:SP`, then `CS` at wrapped `SS:(SP+2)`, and advances `SP` by four. `Simulate_Far_Call` subtracts four from guest `SP`, then stores the incoming source `IP` and `CS`. Recording both operations distinguishes a malformed frame at its creation from later corruption. The instrumentation patch applies after the other optional Jemm source adaptations and is included by hash in the build manifest.

## Sources

- [Pinned Jemm XMS handler and address translation](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/src/XMS.ASM) (`xms_handler`, `xms_get_move_addr`, `xms_move_emb`).
- [Pinned Jemm physical-memory move and interrupt window](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/src/JEMM32.ASM) (`MoveMemoryPhys`, `MoveMemory`, `EnableInts`).
- [CiukiOS per-VM mapping and switch](../../src/vm/session_vmm.inc) (`vmm_create`, `vmm_switch`).
- [CiukiOS Jemm scheduler/profile patch](../../patches/jemm-ciukios-vm-scheduler.patch) (versioned query flags and negotiated host callback).
