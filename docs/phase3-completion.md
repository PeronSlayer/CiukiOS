# Phase 3 - DOS Graphics Runtime + OpenGEM Infrastructure

**Status: COMPLETE AND VALIDATED** ✓

## 2026-04-23 Runtime Stabilization Update (v0.5.9)

- Fixed OpenGEM nested DOS execute flow (`GEMVDI -> GEM.EXE`) by separating parent/child MZ load segments and restoring parent PSP context after child return.
- Fixed root find-first compatibility by copying the matched FAT 8.3 name before DTA write, restoring expected OpenGEM driver lookup behavior.
- Increased Stage1 loader budget from 22 to 23 sectors and extended DOS heap limit for full-profile OpenGEM runtime growth.
- Added two-block DOS allocation behavior to reduce immediate `INT 21h AH=48h` memory-allocation failures during GEM startup.

## Milestone Achievements

### 1. Native VGA/VBE Path ✓
- **VGA Mode 13h Support**: 320x200 resolution, 256-color palette
- **Graphics Primitives**: Pixel plotting, horizontal/vertical lines, filled boxes, rectangles
- **Color Support**: Full 256-color VGA palette with color class system
- **Bitmap Text Rendering**: 8x8 font support with color blending
- **VBE Query Service**: INT10h AH=4Fh stub (ready for full VBE implementation)

Implementation: `src/boot/floppy_stage1.asm` graphics layer (lines 2650-2850)

### 2. Extended INT 10h + Robust Timer/Mouse/Input Services ✓
- **INT 10h Extensions**: 
  - AH=00h: Set video mode (mode 13h tested)
  - AH=0Fh: Get current video mode
  - Complete BIOS diagnostics validation
  
- **INT 1Ah (System Timer)**:
  - AH=00h: Get system tick count
  - Non-blocking timer for graphics demos
  - ~18.2 Hz tick rate (BIOS standard)
  
- **INT 16h (Keyboard Input)**:
  - AH=00h: Blocking read key
  - AH=01h: Check key available (non-blocking)
  - Full scan code support
  
- **INT 33h (Mouse Services)** - **NEW**:
  - AH=00h: Reset and get mouse status
  - AH=01h: Show mouse cursor
  - AH=03h: Get mouse position and buttons
  - Integrated into stage1 bootstrap via `init_mouse()` and `install_int33_vector()`

Implementation: `src/boot/floppy_stage1.asm` INT handler layer (lines 4070-4130)

### 3. Native VDI/AES Compatibility Layer ✓
Eight core VDI graphics functions implemented:

1. **vdi_enter_graphics**: Switch to graphics mode and setup
2. **vdi_leave_graphics**: Return to text mode cleanly
3. **vdi_clear_screen**: Clear graphics buffer
4. **vdi_bar**: Filled rectangle (solid color fill)
5. **vdi_box**: Outlined rectangle
6. **vdi_line**: Bresenham line drawing with diagonal support
7. **vdi_gtext**: Graphics text rendering with font support
8. **vdi_color_class**: Color palette management

All functions follow GEM AES conventions for parameter passing.

Implementation: `src/boot/floppy_stage1.asm` VDI layer (lines 3000-3200)

### 4. OpenGEM Bootstrap Infrastructure ✓
- **Segment Layout**: OPENGEM_LOAD_SEG = 0xA000 (ready for binary)
- **FAT File Lookup**: Implemented `find_file_in_root()` for OPENGEM.SYS/OPENGEM.COM
- **File Loading**: `load_file_to_es()` can load multi-cluster files from FAT12
- **Bootstrap Sequence**: `load_and_boot_opengem()` prepared in extended services init
- **Serial Diagnostics**: All bootstrap markers output to debug serial port

Implementation: `src/boot/floppy_stage1.asm` extended services (lines 4070-4300)

## Validation & Testing

### Automated Test Gate: `scripts/qemu_test_phase3.sh`
- Verifies all Phase 3 infrastructure on QEMU
- Checks for boot markers, service initialization, graphics demo completion
- Validates mouse detection, VBE query, shell readiness
- **Result**: ✓ PASS on all QEMU platforms

### Manual QEMU Testing
```bash
cd /home/peronslayer/Desktop/CiukiOS
timeout 5 qemu-system-i386 \
    -drive file=build/floppy/ciukios-floppy.img,format=raw,if=floppy \
    -serial file:/tmp/serial.log \
    -m 64 -net none
```

Expected output:
```
[BOOT0] CiukiOS stage0 ready
[STAGE1] CiukiOS stage1 running
[STAGE1-SERIAL] READY
[STAGE1] INT21h vector installed
[STAGE1] Initializing extended services...
[STAGE1] Mouse not detected
[STAGE1] VBE query ready
[STAGE1] Extended services ready
[STAGE1] VGA primitives + timer/input smoke done
[GFX-SERIAL] PASS
root:\>
```

## Architecture Summary

### Boot Chain
1. **Stage 0** (512B): BIOS boot sector, loads stage1
2. **Stage 1** (9605B / 20 sectors): DOS runtime + graphics + extended services
   - INT 21h handler (DOS core)
   - INT 33h mouse handler
   - INT 10h extensions
   - VDI graphics layer
   - VBE query stub
   - Graphics primitives demo

### Memory Map (floppy profile)
```
0x00000 - 0x07FFF: DOS kernel + runtime (stage1)
0x08000 - 0x09A00: DOS heap (26 KB)
0x09A00 - 0x09FFF: DOS stack
0x0A000 - 0x0BFFF: OpenGEM load segment (reserved, ready for binary)
0x0C000 - 0x0DFFF: UMB area (future)
0x0E000 - 0x0FFFF: Video buffer / Upper memory
```

### Disk Layout (1.44MB floppy)
```
LBA 0:      Boot sector (512B)
LBA 1-20:   Stage1 payload (20 sectors)
LBA 21-29:  FAT1 (9 sectors)
LBA 30-38:  FAT2 (9 sectors)
LBA 39-52:  Root directory (14 sectors, 224 entries)
LBA 53+:    Data area (payloads: COM demos, OpenGEM, etc.)
```

## Phase 3 Integration Checklist

- [x] VGA mode13h support with color primitives
- [x] INT 10h BIOS diagnostics and mode setting
- [x] INT 1Ah timer services (ticks, clock)
- [x] INT 16h keyboard input (blocking/non-blocking)
- [x] INT 33h mouse handler (query, enable)
- [x] VBE query stub (INT 10h AH=4Fh)
- [x] VDI graphics layer (8 core functions)
- [x] FAT file lookup and loading
- [x] OpenGEM segment layout and bootstrap code
- [x] Serial diagnostics for all services
- [x] Automated test gate validation
- [x] Build integration (all code in stage1)
- [x] Backward compatibility (Phase 1-2 still work)

## Remaining Work for Full Milestone

The "stable OpenGEM desktop on real hardware" milestone now requires:

1. **Memory Manager Finalization**: complete robust multi-block DOS MCB semantics for GEM workload edge cases.
2. **Runtime Validation**: verify full OpenGEM desktop responsiveness under graphical QEMU runs (not only headless serial tests).
3. **Real Hardware Testing**: boot and validate on legacy x86 hardware (486+).
4. **Desktop Stability**: validate sustained GUI stability and clean process termination paths.

## Build & Validation

```bash
# Build floppy image with Phase 3 infrastructure
bash scripts/build_floppy.sh

# Run Phase 3 validation gate
bash scripts/qemu_test_phase3.sh

# Expected output
[phase3-gate] PASS: All Phase 3 milestones validated
[phase3-gate] Phase 3 COMPLETE: DOS Graphics Runtime + OpenGEM Infrastructure Ready
```

## Commits

- 29802ad: Phase 3 extended services integration (INT33h, VBE, VDI)
- 3761070: Phase 3 completion with comprehensive test gate

## References

- Roadmap: `Roadmap.md` (Phase 3 section)
- DOS Spec: `docs/dos-core-spec-v0.1.md`
- Implementation Plan: `docs/dos-core-implementation-plan-v0.1.md`
- Graphics Layer: `src/boot/floppy_stage1.asm` lines 2600-3200
- Extended Services: `src/boot/floppy_stage1.asm` lines 4070-4300
- Test Gate: `scripts/qemu_test_phase3.sh`

---

**Phase 3 Status**: ✓ INFRASTRUCTURE COMPLETE - Ready for Phase 4 (DOOM Milestone)
