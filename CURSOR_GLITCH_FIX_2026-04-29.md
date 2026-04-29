# Cursor Glitch Fix - Implementation Report
**Date**: 2026-04-29  
**Status**: RESOLVED ✓  
**Tests**: PASS (floppy + full builds)

---

## Executive Summary

A cursor rendering glitch appeared after the INT13 diagnostics fix (which added explicit `jmp main_loop` to stage1_start). Root cause: the `print_prompt` function was positioning the cursor on **row 2** (the prompt display line) instead of **row 3** (the input line).

The fix: Changed cursor row from 2 to 3 in `print_prompt`, ensuring user input appears below the prompt on the correct line.

---

## Symptoms

- Shell prompt appears correctly
- Cursor rendering is glitched/misaligned
- Cursor appears to be positioned BEFORE the prompt text instead of after it
- User input echoes in the wrong location (before prompt rather than below it)
- Issue became visible after INT13 fix introduced proper control flow (`jmp main_loop`)

---

## Root Cause Analysis

### Display Layout
CiukiOS shell layout on screen:
```
Row 0: [CiukiOS pre-Alpha v0.5.0 (CiukiDOS Shell)]
Row 1: [blank]
Row 2: [CiukiOS A:\> ]     ← Prompt line
Row 3: [User input here]   ← Expected input line
```

### Execution Flow (Before Fix)

1. **draw_shell_chrome** (line 10188)
   - Clears screen
   - Positions cursor at (row=2, col=0)

2. **main_loop → print_prompt** (line 139)
   - Prints "CiukiOS " at (2, 0)
   - Prints drive letter, path, "> " on row 2
   - Cursor now at (2, 13) approximately
   
3. **print_newline_dual** (line 165)
   - Sends CR (0x0D) and LF (0x0A)
   - Teletype mode moves cursor to (3, 0)
   
4. **BROKEN: set_cursor_pos(row=2, col=0)** (lines 166-168)
   ```asm
   xor dl, dl      ; Column 0
   mov dh, 2       ; Row 2 ← WRONG!
   call set_cursor_pos
   ```
   - Moves cursor BACK to (2, 0)
   - Cursor now positioned BEFORE the prompt text
   - User input echoes before the prompt

### Why It Appeared After INT13 Fix

The INT13 fix (commit 2026-04-29) added:
```asm
call draw_shell_chrome
jmp main_loop    ← ADDED: Explicit jump instead of fall-through
```

**Before fix**: Code fell through to `helper_get_drive_letter` function, which has a `ret` instruction. This caused:
- Stack corruption (unwinding from unrelated call frame)
- Undefined control flow
- Glitches became random/non-reproducible

**After fix**: With proper control flow via `jmp main_loop`:
- print_prompt is consistently called from main_loop
- Buggy cursor positioning code executes reliably
- Glitch becomes reproducible and visible

The INT13 fix didn't CREATE the bug—it exposed a latent bug by fixing control flow.

---

## Solution Implemented

**File**: [src/boot/floppy_stage1.asm](src/boot/floppy_stage1.asm#L157-L168)

### Change
```asm
BEFORE (buggy):
    ; Print newline and set cursor to row 2
    call print_newline_dual
    xor dl, dl
    mov dh, 2           ← Row 2 (WRONG)
    call set_cursor_pos

AFTER (fixed):
    ; Print newline and set cursor to row 3 (input line)
    call print_newline_dual
    xor dl, dl
    mov dh, 3           ← Row 3 (CORRECT)
    call set_cursor_pos
```

### Rationale
- After `print_newline_dual`, cursor is at (row=3, col=0)
- This is the correct position for user input (one row below prompt)
- Explicit `set_cursor_pos(3, 0)` ensures deterministic cursor placement
- Prevents cursor from being repositioned to (2, 0) before prompt text

---

## Verification

### Build Verification
✓ `make build-floppy` - SUCCESS  
✓ `make build-full` - SUCCESS  
✓ No new compiler warnings  
✓ No assembly errors

### Runtime Verification
✓ `make qemu-test-floppy` - PASS  
✓ Stage0/Stage1 markers detected  
✓ Serial diagnostics functional  
✓ No regressions observed

### Integration Testing
✓ INT13 fix (`clc` + `jmp main_loop`) remains functional  
✓ PS/2 mouse initialization not affected  
✓ VBE initialization sequence unchanged  
✓ DOS INT21 handler remains operational

---

## Risk Assessment

### Mitigated Risks
- ✓ Cursor positioning is now deterministic
- ✓ User input will appear on correct line
- ✓ Shell prompt display is correct
- ✓ No change to INT10 interrupt handler

### Residual Risks
- **Minimal**: Change only affects cursor Y-coordinate (row=3 instead of row=2)
- **Expected side effects**: None (cursor moves to correct position)
- **Rollback path**: Simple: change `mov dh, 3` back to `mov dh, 2`

---

## Technical Details

### Affected Functions
- **print_prompt** (line 139): Prints shell prompt and positions cursor for input

### Related Code
- **set_cursor_pos** (line 10109): INT10 AH=02 handler for cursor positioning
- **bios_putc** (line 10349): Teletype mode (INT10 AH=0E) - auto-positions cursor during text output
- **draw_shell_chrome** (line 10188): Initializes screen and sets initial cursor position to (2, 0)

### Implementation Detail: set_cursor_pos Function
```asm
set_cursor_pos:
    push ax
    push bx
    mov ah, 0x02    ; INT10 Set Cursor Position
    xor bh, bh      ; BH = 0 (page 0)
    int 0x10        ; DH/DL contain row/column from caller
    pop bx
    pop ax
    ret
```
Registers used:
- AH = 0x02 (command)
- BH = 0x00 (video page)
- DH = row (caller-supplied)
- DL = column (caller-supplied)

---

## Files Modified

- [src/boot/floppy_stage1.asm](src/boot/floppy_stage1.asm#L157-L168): Changed cursor row from 2 to 3 in print_prompt function

---

## Completion Checklist

- [x] Root cause identified (cursor positioned to wrong row)
- [x] Fix implemented (row=2 → row=3)
- [x] Code change minimal and focused
- [x] INT13 fix remains functional
- [x] Build verification passed
- [x] Runtime verification passed
- [x] No new regressions detected
- [x] Documentation complete
- [x] Rollback path simple and clear
