# OG-P4 Agent Task — DOOM Milestone

Date: 2026-04-25
Assigned to: THIS agent (DOOM/P4 agent)
Priority: Phase 4 — begins after Phase 3 graphical desktop is stable (OG-P3 closed)

---

## Mission

Make DOOM (1993, vanilla DOS binary) boot and be playable on CiukiOS.  DOOM requires:

1. A DPMI host exposing INT 31h services for protected-mode transition
2. A flat 32-bit address space (at minimum ~8 MB accessible after DPMI switch)
3. Mode 13h VGA (320×200, 256-color) already present in Stage1
4. Timer interrupt delivery (INT 08h/1Ch at ~70 Hz) — already present via BIOS
5. Keyboard (INT 09h/16h) — present
6. Sound: at minimum a null/stub driver so DOOM doesn't hang on sound init
7. Mouse: optional for keyboard-only play

DOOM uses a DOS extender (typically DOS/4GW or CWSDPMI).  The CiukiOS DPMI host must
satisfy the extender's minimal DPMI 0.9 surface so DOS/4GW can initialize and pass
control to DOOM's 32-bit code.

---

## Current Codebase State (as of OG-P3 close baseline)

| Component | Location | State |
|-----------|----------|-------|
| Stage1 DOS runtime | `src/boot/floppy_stage1.asm` | Complete Phase 3; INT 21h 50+ fns |
| INT 10h mode 13h | Stage1 `vdi_enter_graphics` | Present; BIOS passthrough otherwise |
| INT 31h DPMI | Stage1 | **NOT PRESENT** |
| INT 15h memory map | Stage1 | **NOT PRESENT** (BIOS passthrough only) |
| A20 gate | Stage1 | Not handled (BIOS may leave it open on QEMU) |
| Protected-mode GDT/IDT setup | Stage1 | None |
| DOOM binary | assets/ | NOT YET INCLUDED |

---

## Phase 4 Scope

### In Scope

- Minimal DPMI 0.9 host (INT 31h) sufficient for DOS/4GW initialization
- Flat protected-mode 32-bit execution environment (single client)
- INT 15h AX=E820h memory map (8 MB conventional + extended to ~16 MB)
- A20 gate enable via BIOS INT 15h AX=2401h or Port 0x92
- Null sound driver stub (so DOOM falls through to "nosound" mode)
- DOOM binary included in `assets/full/` and `build/full/` image
- Playability gate: DOOM title screen reached, game enters playable state
- Performance gate: stable ~35 fps in mode 13h (QEMU benchmarked)

### Out of Scope (P4)

- Sound Blaster emulation (real sound is Phase 5+)
- VESA/VBE 320×200 alternatives (mode 13h is sufficient)
- Multiplayer / IPX networking
- Windows 3.x compatibility (Phase 5)
- Memory beyond 16 MB (HIMEM/XMS extended)
- Phase 3.5 installer — tracked separately under `setup/`

---

## Architecture: DPMI Host in Stage1

DOOM's DOS extender (DOS/4GW) calls INT 31h to:
1. Detect DPMI (via INT 2Fh AX=1687h — already intercepted by `int2f_handler`)
2. Enter protected mode (INT 31h AH=00h, switch to PM and hand back a protected-mode
   entry point)
3. Allocate descriptors (AH=00h sub-function group: 0000h, 0001h, 0007h)
4. Set descriptor base/limit (AH=07h, 06h)
5. Map memory (allocate DOS memory for DOOM's WAD load buffers)
6. Simulate real-mode interrupts from PM (AH=03h: simulate RM interrupt)
7. Set exception handlers (AH=02h: set exception handler)
8. Allocate/free real-mode callback (not critical for DOOM keyboard path)

The DPMI host must live in Stage1 as a new section, installed at boot alongside
INT 21h/20h/2Fh.  It is only activated when a process calls the DPMI entry point.

### INT 2Fh AX=1687h Detection (existing gap)

Stage1 already has `int2f_handler` but it may not respond to AX=1687h (DPMI detect).
This call must return:

- AX=0000h (DPMI host present)
- BX=0001h (32-bit programs supported)
- CL=05h (DPMI 0.9 spec, processor type = 386+)
- DX=0090h (DPMI version 0.90)
- SI=0000h (no private data required)
- ES:DI = real-mode segment:offset of DPMI entry point (a far CALL target)

The entry point is a Stage1 label `dpmi_entry_16` that performs the real-to-protected
mode switch and installs GDT/IDT.

---

## Deliverables

1. **`src/boot/floppy_stage1.asm`** — new DPMI host section:
   - `dpmi_entry_16`: real-to-protected mode switch trampoline
   - `dpmi_pm_handler`: INT 31h handler dispatching DPMI 0.9 function groups
   - `int15_handler`: INT 15h AX=E820h memory map (3 entries: conventional 0–640K, hole
     640K–1M, extended 1M–16M)
   - A20 enable via Port 0x92 at Stage1 init (before any exec)
   - Null SB driver: INT 21h AH=44h device-write to a "SBLASTER" device name returns
     success without output (stops DOOM's SB detect from hanging)
   - Protected-mode IDT with passthrough entries for RM interrupt reflection

2. **`assets/full/doom/`** — DOOM 1 shareware binary (`DOOM1.WAD`, `DOOM.EXE`) or
   placeholder with build instructions.

3. **`scripts/qemu_test_doom.sh`** — DOOM acceptance gate:
   - Boots full image, executes `DOOM.EXE`
   - Asserts serial marker: `[DOOM] title` (Stage1 emits this when INT 10h mode 13h
     is set by DOOM's startup)
   - Times from exec to marker; reports fps estimate from timer tick delta
   - Returns exit 0 on PASS

4. **`docs/diario-bordo-v2.md`** — entry #67+ for Phase 4 progress

5. **`CHANGELOG.md`** — v0.5.11 entry for Phase 4 milestone

---

## Acceptance Criteria

| # | Criterion | Method |
|---|-----------|--------|
| 1 | DOS/4GW initializes without error (no `DOS/4GW fatal error` message) | Serial trace |
| 2 | DOOM title screen is reached and rendered in mode 13h | QEMU display / serial INT 10h mode marker |
| 3 | DOOM enters game state with keyboard input (arrow keys + CTRL) | Manual QEMU test |
| 4 | No hang or crash during DOOM startup over 5 runs | Automated gate |
| 5 | All previous gates still pass (floppy, full, phase3-desktop, regression-lock) | `scripts/qemu_test_all.sh` |

---

## Execution Plan

### Step 1 — Research DOOM's exact INT 31h call sequence

Before writing any DPMI code:
1. Run DOOM in a standard DOS emulator (DOSBox) with INT 31h tracing enabled.
2. Capture the exact sequence of AH values and parameters DOS/4GW uses.
3. Build a minimal DPMI 0.9 function list (probably: 0000h, 0001h, 0006h, 0007h, 0200h,
   0201h, 0202h, 0203h, 0300h, 0301h).
4. Document in `docs/doom-dpmi-trace.md`.

### Step 2 — INT 2Fh AX=1687h DPMI detection

Extend `int2f_handler` in Stage1 to handle AX=1687h:
```asm
int2f_handler:
    cmp ax, 0x1687
    je .dpmi_detect
    ; ... existing cases ...
.dpmi_detect:
    xor ax, ax          ; DPMI present
    mov bx, 0x0001      ; 32-bit support
    mov cl, 0x05        ; 386+ processor
    mov dx, 0x0090      ; DPMI 0.90
    xor si, si          ; no private data needed
    mov es, cs
    mov di, dpmi_entry_16
    iret
```

### Step 3 — GDT and protected-mode trampoline

Add a GDT in Stage1 data section:
- Null descriptor (0)
- CS32: base=0, limit=4GB, code/exec, 32-bit (DPL 0)
- DS32: base=0, limit=4GB, data/rw, 32-bit (DPL 0)
- CS16: alias for real-mode code (base=CS<<4, limit=0xFFFF, 16-bit) — needed for RM
  interrupt reflection back to Stage1

`dpmi_entry_16` (called via far CALL from client):
1. Save client ES:BX (initial PM stack segment:size) from stack
2. Enable A20 (Port 0x92 bit 1)
3. Load GDT (`lgdt [cs:gdtr]`)
4. Load IDT stub (`lidt [cs:idtr_pm]`)
5. Set CR0 bit 0 (PE)
6. Far jump to `dpmi_entry_32` using CS32 selector
7. In PM: set DS/ES/SS to DS32; set client stack from ES:BX
8. Far return to client PM entry point (from stack)

### Step 4 — INT 31h handler in protected mode

A 32-bit interrupt gate for INT 31h pointing to `dpmi_pm_handler`:
- AX=0000h (allocate descriptor): bump a descriptor counter; return selector in AX
- AX=0001h (free descriptor): mark as free; return AX=0
- AX=0006h (get segment base): return base in CX:DX
- AX=0007h (set segment base): write base to GDT entry
- AX=0008h (set segment limit): write limit to GDT entry
- AX=0009h (set descriptor access rights): write access byte to GDT entry
- AX=0200h (get RM interrupt vector): read IVT[BL]*4 → CX:DX
- AX=0201h (set RM interrupt vector): write IVT[BL]*4
- AX=0202h (get exception handler): return current PM IDT entry
- AX=0203h (set exception handler): install 32-bit IDT gate
- AX=0300h (simulate RM interrupt): push flags/CS/IP on RM stack, call INT BL

For DOOM's keyboard: INT 09h is delivered by BIOS normally (hardware IRQ); Stage1's BIOS
passthrough handles it.  DOOM polls the keyboard port (0x60) directly — no DPMI required.

### Step 5 — INT 15h E820h memory map

Install INT 15h handler in Stage1:
```
; AX=E820h, EBX=continuation (0 for first call)
; ES:DI → buffer for 20-byte ACPI memory descriptor
; Returns: EAX='SMAP', EBX=next continuation (0=done), ECX=20
```
Return 3 entries:
- Type 1 (usable): 0x00000000–0x0009FFFF (640 KB conventional)
- Type 2 (reserved): 0x000A0000–0x000FFFFF (BIOS/VGA area)
- Type 1 (usable): 0x00100000–0x00FFFFFF (extended, 15 MB, capped)

After entry 3, return EBX=0 (end of map).

### Step 6 — Null sound stub

DOOM auto-detects SB by probing the SB DSP via I/O port 0x226 (reset) and reading
0x22A.  Since QEMU doesn't emulate SB by default, DOOM falls through to null sound if
the reset doesn't ACK.  Verify this is already the case; if DOOM hangs at sound init,
add a Port 0x22A read hook in Stage1's I/O path or configure QEMU with `-soundhw sb16`.

### Step 7 — Include DOOM shareware binary

1. Download DOOM1.WAD + DOOM.EXE from id Software's official shareware release (legal
   free distribution).
2. Place in `assets/full/doom/` and add to the full image build script.
3. The build script must copy `DOOM.EXE` and `DOOM1.WAD` to the root of the FAT16 image.

### Step 8 — Write acceptance gate and validate

1. Create `scripts/qemu_test_doom.sh`:
   - Boot full image in QEMU (no GUI) with serial output captured
   - Type `DOOM` at the shell prompt via QEMU monitor `sendkey`
   - Wait up to 30 seconds for `[DOOM]` marker (emitted by Stage1 when INT 10h AH=00
     AL=13h is called — add a serial emit there)
   - Assert marker present; report PASS/FAIL
2. Run 5 times; all must PASS.
3. Run `scripts/qemu_test_all.sh`; all existing gates must remain green.
4. Commit: `feat(doom): Phase 4 DOOM milestone closure (OG-P4)`.
5. Update `CHANGELOG.md` v0.5.11, `docs/diario-bordo-v2.md` entry #67, `Roadmap.md`
   Phase 4 → done.

---

## Constraints

- **No CPU emulation** in the final execution path.  The DPMI host switches the real CPU
  to protected mode.
- **Single-client DPMI**: only one protected-mode client at a time; no multi-tasking,
  virtual 8086 mode, or page-table management required.
- **16-bit Stage1 context must remain reachable**: Stage1's INT 21h / DOS services must
  be callable from PM via DPMI AX=0300h (simulate RM interrupt).  DOOM's DOS/4GW uses
  this for file I/O.
- **No new build profiles**: extend the existing `full` profile; do not create a
  `doom`-specific profile unless absolutely required.
- **Regression safety**: The DPMI host code must be conditional (`%if ENABLE_DPMI`) so
  floppy profile is not affected.
- **Phase 3 must be closed first** (OG-P3 gate green) before beginning PM code.  The
  expanded file handles and IOCTL fixes from P3 are prerequisites for DOOM's file I/O.

---

## Reference: DPMI 0.9 Function Priority for DOOM/DOS4GW

| AX | Function | Priority |
|----|----------|----------|
| 0000h | Allocate LDT descriptor | Required |
| 0001h | Free LDT descriptor | Required |
| 0006h | Get segment base address | Required |
| 0007h | Set segment base address | Required |
| 0008h | Set segment limit | Required |
| 0009h | Set descriptor access rights | Required |
| 000Ah | Create alias descriptor | Nice-to-have |
| 0200h | Get RM interrupt vector | Required |
| 0201h | Set RM interrupt vector | Required |
| 0202h | Get PM exception handler vector | Required |
| 0203h | Set PM exception handler vector | Required |
| 0204h | Get PM interrupt vector | Required |
| 0205h | Set PM interrupt vector | Required |
| 0300h | Simulate RM interrupt | Required (for INT 21h from PM) |
| 0500h | Get free memory information | Nice-to-have |
| 0501h | Allocate memory block | Required (DOOM heap) |
| 0502h | Free memory block | Required |
| 0503h | Resize memory block | Nice-to-have |
| 0800h | Physical address mapping | Nice-to-have (mode 13h already mapped) |

Implement "Required" items first; run DOS/4GW to see which additional functions it hits.
