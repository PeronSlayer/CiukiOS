# Parallel implementation prompts: original DOS windows

These assignments continue the [qualified foundation](vm-session-foundation-2026-09-26.md).
They are remaining work, not features already present in the desktop. Each can
run independently in its own branch. Agree on a versioned host/device interface
before integrating; keep edits to shared kernel, shell and ABI files coordinated.

## 1. VGA memory and native presentation

> Work in CiukiOS. Read AGENTS.md, docs/vm-session-foundation-2026-09-26.md and
> docs/vm-dpmi-contract-2026-09-26.md before changing code. Extend the actual
> src/vm/virtual_vga model and session monitor so original real/protected-mode
> DOS programs' video memory cycles observe planar latches, read/write modes,
> chain-4, odd/even addressing and DAC/register state. Do not replace originals
> with source ports or make plain RAM aliases stand in for planar hardware.
> Integrate a bounded protected framebuffer presenter with the native compositor
> through a separate adapter; never call the existing real-mode CR0 transition
> from V86. Keep kernel and SHELL.COM size limits. Own new video/presenter files;
> coordinate shared JLM and ABI changes. Verify original mode-13h and planar
> workloads, focus/resize/redraw, exact cleanup and measured frame times on
> Pentium III/128 MiB QEMU. Preserve failing evidence; do not infer physical
> T23/E500 compatibility or hardware acceleration from emulator screenshots.

## 2. DPMI lifetime and host scheduling

> Work in CiukiOS. Read AGENTS.md and the VM foundation/contract documents.
> Extend the real Jemm/HDPMI integration beyond its qualified fixture to original
> extender clients. HDPMI has separate page tables and I/O permissions; preserve
> shadow ownership across entry, callbacks, faults, mode transitions and exit.
> The qualified sequence creates the host after BEGIN and removes it before END.
> Keep the shipped HDPMI 3.24 callback ABI; do not use the older fork's ABI.
> Implement a host scheduling mechanism that remains responsive during guest
> execution, including guest CLI and blocked BIOS/DOS waits. Own new scheduling
> and DPMI-adapter files; coordinate shared module changes. Validate actual CPU
> instructions, memory accounting, repeated launches, failure unwind, owned
> callback removal and complete page-table restoration. Do not treat source-port
> callbacks or a single foreground V86 context as independent background VMs.

## 3. Focused input and guest audio devices

> Work in CiukiOS. Read AGENTS.md and the VM foundation/contract documents.
> Implement guest input and sound around explicit per-session device ownership:
> keyboard make/break events, Ctrl/Space and modifiers, focus-loss key release,
> mouse state, timer/PIC interrupts and the DMA/Sound Blaster/OPL behavior needed
> by original DOS applications. Use distributable source dependencies with pinned
> provenance and preserved licenses. Do not pass guest physical-device I/O through
> while the host owns the same device or assume JLM traps intercept DPMI clients.
> Own new peripheral/device files and propose the required scheduler interface
> without editing another track's files. Validate original binaries and real
> CPU I/O, objective PCM captures, simultaneous native UI operation, repeated
> focus changes, missing-device messages and cleanup after normal/error exits.
> Keep unsupported operations explicit. QEMU acceptance and physical hardware
> acceptance must remain separate, and no 30 fps guarantee may be fabricated.
