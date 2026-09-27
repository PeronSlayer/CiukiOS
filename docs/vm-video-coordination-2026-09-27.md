# Video track: shared-interface changes for the other VM tracks — 27 September 2026

Written for the scheduler/DPMI and input/audio tracks that edit
`src/vm/session_jlm.asm`, `src/vm/session_abi.inc` and HDPMI in the same tree.
It lists what the video track changed in shared files and what the other
tracks must accept, and records the bounded scheduler/DPMI integration. Final
scheduler reentry and guest-IF fixes remain under qualification; consult the
[integration audit](vm-integration-audit-2026-09-27.md) for exact evidence scope.

## What changed in shared files

| File | Change | Why |
| --- | --- | --- |
| `session_jlm.asm` | `include session_video.inc`; `video_begin` before the aperture map loop; `video_end` in `rollback` right after the PTE restore; `video_can_end` in END after `vm_scheduler_can_end`; video ops `20h`–`2Eh` in `v86_dispatch`; `READBACK` uses the model; video capability bits ORed after `vm_scheduler_capabilities`; `video_owned` in DllMain detach | Model lifetime is tied to the session transaction; no new allocation path in core code |
| `session_jlm.asm` `map_pages` | PTE flags `7` → `3` (present, writable, **supervisor**) | Every guest access to A0000–BFFFF must fault into the VGA model. A user-accessible RAM alias cannot implement latches, map mask, write modes, chain-4 or odd/even |
| `session_jlm.asm` INT 10h hook | Body calls the virtual VGA BIOS (`session_video.c`); the scheduler's `CVSCHED_MODE_TRANSITIONS` increment is kept for AH=00h | One BIOS over the same device state |
| `session_jlm.asm` ports | `read_port`/`write_port` forward 3B0h–3DFh bytes to the model; `port_handler` sends string I/O (`INS`/`OUTS`, REP) to `video_port_string` first and only falls back to the existing sticky fatal when the transfer cannot complete | Original programs load the DAC with `REP OUTSB` (Wolfenstein 3-D does) |
| `scripts/build_vm_session.sh` | Compiles five freestanding OpenWatcom objects (`-ox -ot`, no default libraries) and links them into `CVSESSION.DLL` | C model, emulator, BIOS, presenter, monitor |

New numbers are disjoint from the scheduler range: operations `20h`–`2Eh`,
errors `20h`–`24h`, capability bits `0100h`–`0800h`
(`src/vm/session_video_abi.inc`). Operation `2Dh` is the owned INT 10h
mode-transition bridge used by official HDPMI 3.24. Operation `2Eh` DAMAGE
returns a per-band dirty mask for the desktop DOS window.

## Changes outside `src/vm/`

| File | Change |
| --- | --- |
| `src/com/dos_window_vga.inc` (new) | DOS-window runtime VGA session mode: JLM discovery, configuration, host-mode enter/leave around compositor callbacks, BAND drawing, exact vector/state restore |
| `src/com/dos_window_runtime.asm` | Under V86 (`SMSW` PE=1), install the VGA session mode; otherwise the existing path is unchanged |
| `src/com/dos_window_abi.inc` | `DW_V86_HOST` (209) and `DW_TSC_KHZ` (140) header fields |
| `src/com/shell_dos_window_host.inc` | Host prepare/tick accept PE=1 only with host mode and the banked path active, and PE=0 only with an LFB (reviewed with the UI/LFB track); +48 bytes |

The kernel is unchanged.

## Recorded scheduler/DPMI integration

1. **Separate page tables.** Jemm and every HDPMI client retain independent
   snapshots of all 32 A0000–BFFFF PTEs. The session shadow is supervisor-only
   (`P/W`, not `U/S`) in both CR3s. Detach unmaps the global shared-video alias,
   removes the official callback handle and then restores and verifies every
   client PTE before HDPMI frees client-specific state.
2. **Protected VGA instructions and ports.** HDPMI's ring-0 #PF path executes
   the actual ring-3 faulting instruction with `cvx_execute` on a private
   base-zero stack. Official API-6 port callbacks route scalar VGA IN/OUT to
   the same `cvga_state`; API 7 removes the exact returned handle. The shipped
   HDPMI 3.24 `TRAPPROCS` layout is unchanged.
3. **Mode transitions.** Official DPMI 0300h INT 10h and 0302h calls whose
   explicit target exactly equals the current INT 10h IVT entry use the owned
   JLM virtual-BIOS operation. Arbitrary 0301h/0302h procedures are not
   redirected. This prevents a protected client from bypassing the JLM hook
   and entering the unavailable physical video ROM.
4. **Host scheduling and END ordering.** A Jemm physical-IRQ0 callback owns a
   private stack and services the one foreground session even during virtual
   CLI or blocked BIOS/DOS waits. The HDPMI adapter preserves pre-client PIC
   masks, keeps IRQ0 available while owned and restores both masks exactly.
   END refuses while the host, client, callback or video share is owned; the
   qualified order is host creation after BEGIN and removal before END.

The client qualification runs the unchanged packaged doom-vanille DOS engine
`APPS/DOOMVAN/PCDMCORE.EXE`, SHA-256
`efbe64359fb1dfe569cde2428f41ef15a40b8975b8eb36a1cfc5a2731b894980`.
It is neither the proprietary original Doom executable nor the cooperative
CiukiOS window port. It observes separate Jemm/HDPMI CR3s, real protected
instructions, VGA reads/writes and I/O, a deliberate #UD unwind, repeated
launches, five exact callback install/remove pairs, zero PTE repairs and exact
before/after PTE and physical-aperture bytes. Callbacks and the single current
V86 execution context remain services of one foreground session, not
independent background VMs.

The host-progress observations during CLI and BIOS waits do not by themselves
qualify virtual IF preservation or every V86 IOPL/POPF path. The newer scheduler
guard and guest-IF corrections require their own final runtime result.

## For the input/audio track

Peripheral exports now use `cvgp_` / `CVGP_`; `cvp_` / `CVP_` remains reserved
for the presenter. The combined host/sanitizer and freestanding target link
passes, but the peripheral model is still not attached to Jemm/HDPMI traps.

The video track does not trap or claim any non-VGA port, keyboard/mouse
interrupt or timer beyond an INT 08h *observer* hook (Jemm V86 hook chain,
returns CF=1, so later hooks and the guest IVT still receive every tick). The
timer presenter is rate-limited by TSC and bounded per tick (`budget_pixels`);
it runs with interrupts disabled for the duration of one bounded present,
measured at ≤ 7.1 ms for 64 000 pixels on QEMU/KVM (uncached LFB stores).
