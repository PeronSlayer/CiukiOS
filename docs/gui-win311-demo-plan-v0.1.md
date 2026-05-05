# CiukiOS Minimal GUI Demo Plan v0.1

## Goal
Prototype a tiny Windows 3.11-inspired graphical shell without changing the core runtime path.

The demo is intentionally packaged as a normal full-profile DOS `.COM` application so runtime work can continue independently.

This document remains exploratory only. It does not change the mainline engineering priority order, which is runtime split, broad DOS program compatibility, legacy audio, and only later Windows pre-NT readiness.

## Branch Scope
Branch: `feat/gui-win311-demo`

Included in this branch:
1. `CIUKWIN.COM`, a real-mode VGA mode 13h desktop mockup
2. full-profile image packaging for `\APPS\CIUKWIN.COM`
3. documentation for the future GUI direction

Excluded from this branch:
1. Stage1 shell integration
2. window manager state in the kernel/runtime
3. mouse driver dependency
4. protected-mode graphics
5. changes to `main` or the active runtime-cleanup branch
6. any claim that GUI work is ahead of DOS compatibility, audio, or runtime-split milestones

## Demo Behavior
Run from the full-profile shell:

```text
CIUKWIN
```

The demo:
1. switches to VGA mode 13h
2. draws a Program Manager-style desktop
3. shows a title bar, menu row, windows, icons, and pointer mockup
4. waits for any key
5. restores text mode and returns to the CiukiDOS shell

## Future Architecture Direction
The next real GUI should remain DOS-runtime friendly:

1. `GFX` layer: mode set, palette, primitive draw calls, blit hooks
2. `WDM` layer: windows, rectangles, z-order, invalidation
3. `CTL` layer: buttons, menus, list boxes, text labels
4. `DESK` shell: launcher, file manager, settings, shutdown/return
5. optional `MOUSE` input through INT 33h once driver loading is stable

## Acceptance Criteria
1. `make build-full` succeeds.
2. `CIUKWIN.COM` is present under `\APPS` in the full FAT16 image.
3. Running `CIUKWIN` opens the graphical demo and returns cleanly to the shell after a key press.
