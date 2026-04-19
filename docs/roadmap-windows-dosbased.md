# Roadmap: Windows DOS-based on CiukiOS

**Status:** Design-stage. Implementation gated on approval of `docs/opengem-016-design.md`.
**Baseline:** Alpha v0.8.7
**Scope:** Windows releases that run on top of — or boot from — MS-DOS / FreeDOS-compatible runtimes.
**Out of scope (permanent):** Windows NT, 2000, XP, Vista, 7, 8, 10, 11. These are unrelated OS kernels and will never be supported by this roadmap.

---

## 1. Position in the CiukiOS project

This roadmap is a **sibling** of `docs/roadmap-ciukios-doom.md`, not a replacement. Both roadmaps share the same foundation — the 16-bit execution layer designed in `docs/opengem-016-design.md` — and advance in parallel once that foundation exists.

| Roadmap | Primary target |
|---|---|
| `roadmap-ciukios-doom.md` | DOS DOOM binary (`DOOM.EXE`) |
| `roadmap-windows-dosbased.md` *(this file)* | Windows 1.x → Windows ME |
| `roadmap-dos62-compat.md` | MS-DOS 6.2 compatibility baseline |

---

## 2. Compatibility tiers

### T0 — v8086 foundation (not a Windows tier, but the prerequisite)
- Target: `gem.exe`.
- Status: planned under OPENGEM-017..024.

### T1 — Windows 1.x / 2.x
- Pure 16-bit real-mode shell over DOS.
- No additions required beyond T0; useful as a Windows-shape canary.
- Expected surprises: niente di rilevante.

### T2 — Windows 3.0 real mode
- Full INT 21h coverage (short filenames only).
- INT 2Fh multiplex interface (minimal — Windows presence detection).
- TSR-style interrupt hooks must survive the Win/DOS boundary.

### T3 — Windows 3.1 Standard mode
- **DPMI 0.9 host** (INT 31h) — first appearance of protected-mode services.
- 286-style protected-mode transitions emulated on 32-bit PE.
- LDT management.
- KRNL286.EXE is the canonical smoke test.

### T4 — Windows 3.1 / WfW 3.11 Enhanced 386 mode
- Stable 8259 PIC timing (master+slave, IRQ2 cascade, edge/level correctness).
- Stable 8253 PIT timing (channel 0 tick, channel 2 speaker).
- VGA register fidelity across CRTC, Sequencer, GC, Attribute.
- VxD negotiation handshake with graceful fallback (no VxD execution).
- KRNL386.EXE is the canonical smoke test.

### T5 — Windows 95 / 98
- **FAT32 + LFN** mandatory.
- INT 13h extensions (AH=41h, 42h, 43h).
- VESA VBE 2.0 linear framebuffer.
- PS/2 8042 controller full behavior (commands, scancodes, mouse aux channel).
- IDE/ATA real (PIO + basic DMA).
- Real-mode → 32-bit protected-mode bootstrap preserved through Win9x setup.
- `WIN.COM` → `VMM32.VXD` transition is the canonical smoke test.

### T6 — Windows ME
- Minimal ACPI surface (RSDP + FADT + FACS minimums).
- FAT32 boot without real-mode driver dependency.
- T6 is a logical extension of T5; no separate architecture.

---

## 3. Hard compatibility requirements

These requirements are non-negotiable for T5/T6. They should be introduced earlier when feasible:

1. **Deterministic PIT tick** at 18.2065 Hz (or the reprogrammed rate requested by the guest).
2. **PIC edge/level parity** with reference hardware.
3. **RTC / CMOS** read accuracy, including BCD / binary mode flag and century byte on T5+.
4. **A20 gate** controllable through 8042 port 0x64 and fast-A20 via port 0x92.
5. **BIOS data area** (segment 0x0040) readable and writable by guests.
6. **Video mode 13h** pixel-accurate writes.
7. **VESA VBE 2.0** mode list and LFB for T5.
8. **FAT32 + LFN** with correct checksum on 8.3 companions.
9. **INT 2Fh multiplex** including Windows version reporting and Win9x "are you there" probe.
10. **DPMI 0.9 host** responsive with correct error flags.

Each requirement becomes a gate in the OPENGEM-017+ sequence once its consuming tier is approached.

---

## 4. Non-goals (permanent)

- Windows NT family: NTLDR, BOOTMGR, NTFS, WDM, HAL, kernel-mode PE loader — none of these will ever be implemented here.
- ReactOS compatibility — out of scope.
- WoW64 — out of scope.
- Win32 on non-Windows hosts (Wine-style) — out of scope.

Any request to support Windows NT or newer on this roadmap must be rejected with a pointer to this section.

---

## 5. Validation doctrine

Each tier must ship with:

1. A dedicated static gate that checks binary presence, INT dispatch coverage, and marker emission patterns.
2. A runtime probe opt-in env variable (`CIUKIOS_VM86_BOOT_LOG`) equivalent to `CIUKIOS_OPENGEM_BOOT_LOG`.
3. A screenshot / serial log artifact for documentation.
4. A handoff entry per the CiukiOS session workflow.
5. No regression on prior tiers — the full static gate stack stays green.

---

## 6. Estimated milestones

Estimates are intentionally omitted. Each milestone lands when it lands, under the CiukiOS single-session discipline. The ordering is:

```
T0 → T1 → T2 → T3 → T4 → T5 → T6
```

No tier is skipped.

---

## 7. Related documents

- `docs/opengem-016-design.md` — architectural foundation.
- `docs/roadmap-ciukios-doom.md` — parallel DOOM roadmap.
- `docs/roadmap-dos62-compat.md` — DOS 6.2 baseline.
- `docs/int21-priority-a.md` — INT 21h compatibility priorities.
- `CLAUDE.md` — Project North Star.
