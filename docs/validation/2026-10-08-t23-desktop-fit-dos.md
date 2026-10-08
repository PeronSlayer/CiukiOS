# T23 desktop composition, complete wallpaper and DOS evidence — 8 October 2026

## Physical evidence and repair

The owner confirmed that the preceding scanline fix restored full-colour
graphics on the ThinkPad T23, then reported residual stalls, black DOS windows
and wallpaper replacing a DOS client when opening Volume or another window.
The owner also requested the complete wallpaper without cropping.

The connected TS64GMSA230S, serial H551120730, disk sequence 59 was inspected
through its read-only mount. The bounded collector retains hashes, mount
options and decoded records in
`build/full/t23-next/physical-logs-20261008-diskseq59-recheck/manifest.json`.
Its video log confirms 1024×768×32, pitch 4096 and protected session row
transfers. Its native S3 backend remains unqualified: stage 1, zero commands,
zero timeouts. The older DOS log records two guest exits with code 0000 but
does not identify gameplay, a stall or a user-requested close. Neither that
return code nor a successful presenter call proves gameplay.

The desktop module invoked WALLP.APP during the overlay paint pass, after
window clients had already been painted. Restricting the wallpaper call to
the desktop pass fixes the observed overwrite and avoids the extra wallpaper
render/upload on popup repaint. A second defect sent owner-zero taskbar
actions to the active module first; Files could consume the DOS task button
as a Files control. Owner-zero taskbar actions now reach the shell before
module controls; module-owned colliding IDs preserve their original route.

Ciuk1 now defaults to Fit in fresh images and absent/legacy one-byte profiles.
The image is contained in the desktop work area, preserves its proportions
and uses desktop-colour bands. Explicit saved two-byte style choices remain
supported. Research and decisions are recorded in
[desktop composition](../design/desktop-overlay-composition.md) and
[T23/window/wallpaper research](../design/t23-window-composition-research.md).

Finished DOS windows show the session result instead of an empty black client.
New bounded DOS snapshots preserve fresh video/device state at most once per
second, write one 448-byte record at the live deadline or termination, and
retain generation and sample ages. Display Properties saves the existing
624-byte framebuffer cache/timing record once per process. Savage stage-1
diagnostics retain the failed format/geometry/BAR tuple. These changes expose
the missing physical evidence; they do not alter guest memory limits, timer
debt, IRQ delivery or physical framebuffer cache policy.

## Verification

All full builds and QEMU jobs ran sequentially in separate systemd user
scopes capped at 3 GiB memory, 1 GiB swap and one CPU. No standalone floppy
profile was used. Each QEMU gate uses a private image or a read-only CD and
checks fresh SHELL bytes/listings before observing guest memory. Observation
does not modify guest RAM.

| Check | Result and evidence |
| --- | --- |
| Original overlay defect | Reproduced on the previous image: 281,019 client pixels overwritten outside Volume; `qemu-overlay-before-retry-20261008/report.json`. |
| Corrected overlay, taskbar and window order | PASS, nine exact pixel comparisons, each zero unintended changes; geometry retained and guest not relaunched on restore; `qemu-overlay-verified-20261008/report.json`. |
| Taskbar routing CPU regression | PASS, 200 scenarios plus a mutation that reproduces Files consuming the DOS task button; `build/tests/taskbar-routing-20261008/report.json`. |
| Default Fit on HDD | PASS at 640×480, 800×600, 1024×768 and 1280×800, also retaining explicit Ciuk2/Ciuk3 Fill; `qemu-fit-properties-20261008/provenance.json` and each profile's `result.json`. |
| Full-CD default Fit | PASS on final CD-only boot, 36 exact image samples and uniform bands; `qemu-fit-cd-final-20261008/report.json`. |
| Invalid VBE 4F06 pitch regression | PASS, 1280×800×32, one-page fallback, exact Fit pixels and native-client return; `qemu-fit-hardware-20261008/report.json`. |
| Read-only cache API | PASS, real system caller, short/bad destination guards and VMFORK child rejection; `qemu-cache-api-20261008/report.json`. |
| Savage preflight | PASS, 45 host scenarios and ASan/UBSan; `build/tests/savage-preflight-20261008/report.json`. |
| Snapshot/parser/overlay/Wolf host checks | PASS, 36 tests; canonical warning-as-error DOSVM compile also passed. |
| DOOM readiness regressions | PASS, 15 captured/synthetic cases reject Skill/title/menus and incomplete wipes; `build/tests/doom-gameplay-screen-20261008/report.json`. |
| Files mouse delivery regression | PASS, seven consumed-edge/tick-wrap/ownership/bounded-retry scenarios; `scripts/tests/test_files_game_mouse.py`. |
| Wolf initial screen regressions | PASS, 14 cases compare the linked original signon bitmap and installed-font prompt; `build/tests/wolf-signon-screen-20261008/report.json`. |
| Wolf episode regressions | PASS, 11 cases compare all 955 sampled thumbnail pixels and the cursor, reject blank/faded/wrong-menu controls; `build/tests/wolf-episode-screen-20261008/report.json`. |
| Wolf intro regressions | PASS, seven bounded state-machine scenarios and nine captured full-palette cases; `build/tests/wolf-intro-screen-20261008/report.json`. |
| Wolf gameplay readiness | PASS, 17 captured/asset cases reject loading, incomplete fades and corrupted static HUD pixels while accepting documented changing fields; `build/tests/wolf-gameplay-screen-20261008/report.json`. |
| Wolf quit readiness | PASS, 20 captured/asset cases recognize the complete original confirmation and reject gameplay or incomplete text; `build/tests/wolf-quit-screen-20261008/report.json`. |
| DOOM from Files, no arguments | PASS, two launches, actual WAD statusbar scores 0.99914/1.0, 146,309/146,308 view pixels changed by input, normal menu quit and visible finished result; `qemu-doom-files-hud-20261008/report.json`. |
| Original Wolf from Files, no arguments | PASS, complete level palette, three passive comparisons, commanded rotation, exact F10 confirmation, game-initiated exit and desktop return; `qemu-wolf-files-complete-20261008/report.json`. |
| Properties styles | PASS through 27 checks: all five positions, CWP1 Tile, malformed CWP2 rejection, unsaved selection and theme cancellation; `qemu-properties-full-cache-20261008/800-std/result.json` then stops at the old drag harness's edge-clamped move. |
| Properties UI/mode/cache | PASS, 10 checks including actual bounded title drag, damaged-area pixels, malformed assets, cancellation, 640×480 preview rollback/Keep, restart and exact 624-byte persistence; `qemu-properties-ui-bounded-20261008/800-std/result.json`. |

QEMU report paths above are beneath `build/full/t23-next/` unless an explicit
`build/tests/` prefix is shown. Emulator evidence does not qualify the T23's
S3 engine or prove that its physical lag/DOS symptoms have disappeared.

The game gate no longer uses a fixed delay after `ST_Init` as a gameplay
baseline. One previous passing control had a black baseline and another
failed during loading. Original-engine tests instead require the installed
assets' actual HUD, response to input and normal game-initiated quit; the DOOM
gate also requires a second launch. Files navigation verifies the real address field and each typed
prefix, then double-clicks the installed shortcut with no arguments. Invalid
test navigation and premature menu/HUD classification are retained as failed
harness evidence, not reported as operating-system defects.

The passing DOOM run retains a new 448-byte exit snapshot for generation 2,
state FREE, code 0000, both packets validated. It reports zero fatal or
unsupported protected VGA instructions and zero forwarded-key drops/IRQ
failures. Its final video sample describes the game's restored text screen;
the gameplay screenshots and input comparisons establish the preceding play.
The device packet is an older retained sample with its age and failed final
query annotated, not a claim that those devices remain live after exit.

The Properties drag harness now waits for the actual held-button drag state
and chooses a movement inside the work area. Previously it always requested
right/down movement even when the large dialog was already against both
limits; the window manager correctly kept it there. This fixture correction
does not relax the UI's bounds or its pixel-damage comparison.

The original Wolf engine first blocks on `FinishSignon`'s “Press a key” page.
The gate recognizes its actual installed bitmap/font before acknowledging it.
The Episode 1 thumbnail's full-colour contrast is 65; a generic minimum of 80
incorrectly rejected the exact screen. Only that thumbnail uses minimum 64,
with all spatial/class comparisons and the separate cursor retained. The
following attempt captured a title fade and zero fatal/unsupported counters,
but the test stopped at its fixed ten-second phase wait. Intro progression
therefore requires a bounded state machine instead of that fixed delay;
those incomplete attempts do not establish a guest-engine failure.

The following attempt reached the level but sampled its baseline during
Get Psyched: the loading page already has a valid status bar. The upstream
[PreloadGraphics implementation](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_INTER.C)
draws that page before loading and fading out; its picture is
[GETPSYCHEDPIC 134](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/GFXV_WL6.H).
The corrected gate therefore requires the complete installed palette on
7,680 static HUD pixels and absence of the exact loading asset before
measuring input. It retains the existing 4,534 spatial/class samples,
contrast threshold and dynamic-field masks. Bounded settling after input
also accounts for the palette transitions in the original
[PlayLoop and UpdatePaletteShifts](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_PLAY.C).
The failed premature sample had zero fatal/unsupported VGA or device model
errors and is retained as harness evidence rather than an engine diagnosis.

The exit gate also waits for the actual confirmation instead of comparing a
single screenshot immediately after F10. Its first screenshot preceded the
dialogue, while the subsequent failure capture contained the complete
original confirmation. The matcher uses the installed engine's nine original
messages and Font1 at the coordinates documented by
[Message and Confirm](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_MENU.C).
It recognizes 7,943 pixels in that captured message with score 1.0 and rejects
the earlier gameplay frame before sending Y.

The final original-engine Wolf run passes from the bare Files shortcut with
no arguments. Three passive comparisons change zero view pixels; Right changes
20,243 pixels. The game accepts its own exit confirmation, returns code 0000
and leaves a fresh 448-byte generation-1 snapshot: state FREE, both packets
validated, zero fatal/unsupported VGA instructions. Closing the finished DOS
window unloads its module and returns to the desktop. The source image is
unchanged and the test writes no guest RAM.

Final build/package hashes, validated component comparisons, archive CRCs,
private-data exclusions and the physical write/readback are retained beneath
`build/full/t23-next/`. The Windows launcher is not runtime-tested on Windows.
