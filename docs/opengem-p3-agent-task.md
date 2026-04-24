# OG-P3 Agent Task — OpenGEM Full Graphical Desktop

Date: 2026-04-25
Assigned to: parallel agent (NOT the DOOM/P4 agent)
Priority: Phase 3 completion — must close before Phase 3.5 installer begins

---

## Mission

Make the OpenGEM graphical desktop render fully and be interactable on the CiukiOS full
profile.  The current state is: GEM.EXE launches and reaches its event loop, but the
graphical desktop either fails to render or remains in a degraded partial state.

The agent must identify the exact root cause(s), implement the minimum fixes in Stage1
and/or Stage2 assembly, and close the milestone with reproducible gate evidence.

---

## Current Codebase State (as of v0.5.9-final)

| Component | Location | State |
|-----------|----------|-------|
| Stage1 DOS runtime | `src/boot/floppy_stage1.asm` | Complete for Phase 2; minor gaps for Phase 3 |
| Stage2 launcher | `src/boot/full_stage2.asm` | OPENGEM_TRY_EXEC=0 (stub) by default |
| VDI primitives | Stage1 (`vdi_enter_graphics` etc.) | Present, stand-alone, NOT wired into GEM runtime path |
| INT21h handler | Stage1 | 50+ functions; 3 user file handle slots |
| INT33h | Stage1 (`install_int33_vector`, `int33_handler`) | Installed; stateful; does NOT render cursor in graphics mode |
| INT10h | BIOS passthrough | No CiukiOS intercept — GEM's VGA driver calls BIOS directly |
| OpenGEM payload | `assets/full/opengem/` | Full: GEM.RSC, DESKTOP.RSC, SDPSC9.VGA, DESKHI.ICN, DESKLO.ICN, DESKTOP.INF, DESKTOP.FMT, etc. |
| Full profile image | `build/full/` | FAT16 HDD image; GEMSYS/ and GEMAPPS/ directories present |

---

## Root-Cause Hypothesis (Confirmed Gaps)

### GAP-1: File Handle Exhaustion (HIGH CONFIDENCE — primary blocker)

Stage1 provides only 3 user file handle slots (BX 5, 6, 7).  GEM.EXE opens the
following files during desktop init in rapid succession, often with more than one open
simultaneously:

- SDPSC9.VGA (video driver)
- GEM.RSC (resource file)
- DESKTOP.RSC
- DESKTOP.INF
- DESKHI.ICN
- DESKLO.ICN
- DESKTOP.FMT / DESKTOP.DFN

With 3 slots this exhausts immediately.  INT 21h AH=3Dh returns error 0x0004 (too many
open files), GEM aborts or enters a degraded mode.

**Fix**: Expand file handle slots to at least 8 in Stage1.

### GAP-2: OPENGEM_TRY_EXEC Default is 0

The full profile build passes `-DOPENGEM_TRY_EXEC=0` or omits the flag, so Stage2
always takes the blocked/stub path.  This is by design as a safety valve, but must be
enabled for Phase 3.

**Fix**: Update the `full` build profile (`build-profiles/full.md`, `scripts/build_full.sh`,
and `Makefile`) to pass `-DOPENGEM_TRY_EXEC=1` for Phase 3 closure.

### GAP-3: INT 33h Mouse Cursor Not Visible in GEM Graphics Mode

The Stage1 INT 33h handler (line ~6893) handles reset/status/position/range/version but
does not draw a hardware or software sprite cursor in VGA graphics mode.  GEM renders
the desktop but mouse movement produces no visible cursor, making it appear the desktop
is non-interactive.

GEM calls INT 33h AX=0001h (show cursor) and AX=000Bh (set motion ratio) during init.
The current handler accepts but ignores the draw-cursor request in graphics mode.

**Fix**: Implement a minimal software sprite cursor for INT 33h AX=0001h in VGA mode
(mode 12h or 13h): save behind-cursor pixels to a scratch buffer, XOR-plot an 8×8 or
16×16 cursor bitmap at the current position.  On move (AX=0004h), erase previous sprite
and redraw at new position.

### GAP-4: INT 21h AH=44h IOCTL Subfunctions Missing

GEM's VDI driver probes devices via IOCTL subfunctions not currently handled by Stage1:

- Sub 04h: Read from character device control channel
- Sub 05h: Write to character device control channel
- Sub 0Dh: Generic IOCTL for block devices (disk geometry query)
- Sub 08h: Check if block device is removable

Currently `int21_ioctl` only handles sub 00h (get device info), 06h, 07h (status).
Unknown subfunctions return success (AX=0, CF=0) with no output, which may cause GEM
to misinterpret device capabilities.

**Fix**: Add stub handlers for sub 04h, 05h, 08h, 0Dh in `int21_ioctl`.  Sub 08h for
the boot drive should return AL=1 (not removable).  Sub 0Dh should return CF=1 with
AX=001Fh (unknown request) — GEM handles this gracefully.

### GAP-5: INT 21h AH=52h List-of-Lists Pointer Format

Stage1 already implements AH=52h returning ES:BX pointing to SYSVARS, but the MCB chain
anchor at ES:[BX-2] may not satisfy GEM's DOS version probe order.  GEM uses AH=30h
(version) to decide code paths; Stage1 returns AL=05 BH=00 (DOS 5.0), which is correct.
Verify no issue here; document after tracing.

---

## Deliverables

1. **`src/boot/floppy_stage1.asm`** — patched with:
   - File handle slots expanded from 3 to 8 (handles BX=5..12; all swap helpers updated)
   - INT 33h AX=0001h: software sprite cursor in VGA modes; AX=0002h: hide cursor
   - `int21_ioctl` sub 04h, 05h, 08h, 0Dh stubs

2. **`scripts/build_full.sh`** and **`Makefile`** — `OPENGEM_TRY_EXEC=1` active in full
   build.

3. **`scripts/qemu_test_phase3_desktop.sh`** — acceptance gate:
   - Launches QEMU with full image
   - Asserts serial markers: `[OPENGEM] try GEMVDI`, `[OPENGEM] try GEM.EXE`, `[OPENGEM]
     return` (no `fail`)
   - Reports PASS/FAIL

4. **`docs/diario-bordo-v2.md`** — entry #66+ recording the fix and gate evidence.

5. **`CHANGELOG.md`** — v0.5.10 entry with Phase 3 desktop closure.

---

## Acceptance Criteria

| # | Criterion | Method |
|---|-----------|--------|
| 1 | Full profile boots to GEM graphical desktop without crash or fallback to shell | QEMU serial trace: no `fail`, no `4B!`, no `48!` |
| 2 | `qemu_test_phase3_desktop.sh` returns exit 0 over 10 consecutive runs | Automated gate |
| 3 | Mouse cursor is visible and tracked in GEM VGA mode | QEMU display visual or pixel-assert |
| 4 | Shell is reachable after GEM exit (`CLS` command works) | Serial trace: `[CiukiOS]` prompt after GEM return |
| 5 | All existing gates remain green (floppy, full, regression-lock, perf-budget) | `scripts/qemu_test_all.sh` exit 0 |

---

## Execution Plan

### Step 1 — Enable OPENGEM_TRY_EXEC=1 and trace GEM init

1. Edit `scripts/build_full.sh`: add `-DOPENGEM_TRY_EXEC=1` to the `nasm` invocation
   for `full_stage2.asm`.
2. Rebuild and run: `make full && scripts/qemu_run_full.sh`.
3. Capture serial output; identify the exact INT 21h call that first fails (look for
   `4B!`, `48!`, `3D?` error markers).

### Step 2 — Expand file handle slots (GAP-1)

The handle data is stored in a parallel-array structure.  Expanding from 3 to 8 slots
requires:
- Add 5 more sets of variables: `file_handle4_open` through `file_handle8_open` and all
  associated fields (pos, mode, start_cluster, root_lba, root_off, cluster_count,
  size_lo, size_hi).
- Extend `int21_open` to try slots 4–8 after slot 3.
- Extend `int21_close`, `int21_read`, `int21_write`, `int21_seek` to dispatch BX=8..12
  similarly to BX=7 (using existing swap-helper pattern).
- Extend `int21_ioctl` sub 00h device-info dispatch for BX=8..12.

Ensure BX values returned to caller match the slot (slot 4 → BX=8, ..., slot 8 → BX=12).

### Step 3 — Software sprite mouse cursor (GAP-3)

Add the following to the INT 33h handler section of Stage1:

- A 64-byte scratch buffer `mouse_save_buf` to hold behind-cursor pixels (8×8 in
  mode 13h, or 16×16 in mode 12h stored as byte array).
- `mouse_cursor_visible` flag (0=hidden, 1=shown).
- On INT 33h AX=0001h (show cursor): if in graphics mode (`int 0x10 AH=0Fh` to read
  current mode), save pixels under cursor position, draw XOR sprite at (mouse_x, mouse_y).
- On INT 33h AX=0002h (hide cursor): restore saved pixels.
- On INT 33h AX=0004h (set position): if visible, hide then show at new position.
- On INT 33h AX=000Bh (set motion ratio): store mickeys-per-8-pixels values (stub ok).
- Use a minimal 8×8 arrow glyph stored in Stage1 data section (same style as gfx_font8).

### Step 4 — IOCTL stubs (GAP-4)

In `int21_ioctl`, add cases for sub 04h, 05h, 08h, 0Dh:
- Sub 04h: store BX (device handle) in a scratch; CX bytes of control data; return CX=0,
  CF=0.
- Sub 05h: same pattern; CF=0.
- Sub 08h: check BL (drive number, 0=default); for valid drives return AL=1 (non-removable),
  CF=0; for unknown drives AX=0x000F, CF=1.
- Sub 0Dh: return CF=1, AX=001Fh (not supported) — GEM handles gracefully.

### Step 5 — Validate and close

1. Run `scripts/qemu_test_phase3_desktop.sh` ten consecutive times.
2. Run `scripts/qemu_test_all.sh` for regression check.
3. Commit with message `feat(opengem): Phase 3 graphical desktop closure (OG-P3)`.
4. Update `CHANGELOG.md` (v0.5.10), `docs/diario-bordo-v2.md` (entry #66), `Roadmap.md`
   Phase 3 status → all green.

---

## Constraints

- **No CPU emulation**: all code runs in native 16-bit real-mode; no protected-mode
  transitions are permitted in Stage1.
- **No new source files** without explicit need; prefer modifying `floppy_stage1.asm`
  and `full_stage2.asm` in-place.
- **No regression on floppy profile**: floppy profile does not include OpenGEM and must
  keep all existing gates green.
- **Architecture**: x86 NASM, `bits 16`, `org 0x7C00` / CS-relative addressing; all
  data in `[cs:label]` form; no 32-bit instructions unless already established in the
  area being modified.
- **Makefile discipline**: do not add new Makefile targets without documenting them; use
  existing `build_full.sh`/`build_floppy.sh` pattern.
- **P3 does NOT include DOOM or extender stubs** — those belong to P4.

---

## Reference Symbols in Stage1 (key locations)

| Symbol | Approx line | Purpose |
|--------|-------------|---------|
| `install_int33_vector` | ~6878 | Hooks IVT vector 0x33; INT 33h stub installed here |
| `int33_handler` | ~6893 | Dispatch for AX=0000..000Bh |
| `int21_handler` | dispatch ~7050 | Master INT 21h handler |
| `int21_open` | after `int21_handler` | AH=3Dh; 3-slot logic to expand |
| `int21_ioctl` | after `int21_open` | AH=44h; gaps at sub 04/05/08/0Dh |
| `int21_swap_file_handles` | after `int21_ioctl` | Slot-swap helper; extend for slots 4–8 |
| `file_handle_open` | BSS/data | Slot-1 state; replicate for slots 4–8 |
| `vdi_enter_graphics` | near gfx section | `int 0x10` AH=00 AL=13h — OK for P3 if GEM sets own mode |
