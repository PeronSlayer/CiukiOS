# Desktop overlay composition

The desktop module has two separate drawing roles: `WIN_DESKTOP` supplies
wallpaper and icons beneath native windows, and `WIN_OVERLAY` draws menus
above them. The shell composes a damaged band in that order. DOSVM restores
its client from the retained VGA model during `EV_PAINT`, using the
`CVP_NO_DAMAGE` presenter path even when the guest has produced no new pixels.

## Sources checked before implementation

[Microsoft's window painting documentation](https://learn.microsoft.com/en-us/windows/win32/learnwin32/painting-the-window)
describes repainting newly exposed client regions after another window moves
away, with drawing clipped to the update region. The
[X Consortium Xlib specification](https://xorg.freedesktop.org/archive/current/doc/libX11/libX11/libX11.html)
defines window stacking, exposure after unmapping or restacking, and backing
store as optional. A compositor must restore exposed clients from retained
content or request them to repaint; wallpaper alone cannot restore a client.

The repository already follows this policy in `ui_comp_bands`,
`ui_draw_scene`, `ui_windows_draw` and DOSVM's `band_present`. However,
`desktop.c` calls WALLP.APP unconditionally before its `paint()` dispatcher.
The overlay pass invokes the same `EV_PAINT` handler after every window has
painted. Although `paint()` correctly limits `WIN_OVERLAY` to the popup, the
earlier wallpaper call writes into the same completed composition band and
overwrites window clients outside the popup. This is a paint-layer defect;
it does not depend on a new guest frame or native GPU acceleration.

## Decision

The expanded runtime gate also exposed incorrect taskbar command routing.
The shell gives native task buttons actions200..231 with owner zero, but
the active module previously consumed those IDs as its own controls before
the native task handler ran. Route taskbar actions from owner zero first;
retain module-owned controls with identical numeric IDs in their module.
This is CiukiOS's own namespace, not the Win32 ABI. The distinction follows
the source/owner separation documented for [WM_COMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-command)
and the window-manager actions in [WM_SYSCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand).

Call WALLP.APP only for `HOST.window == WIN_DESKTOP`. Keep overlay drawing
limited to the existing Volume and popup painters. Preserve the existing
background-to-window-to-overlay order, damage bounds and DOSVM exposure
presenter. The change belongs to DESKTOP.APP and uses no additional resident
SHELL payload bytes.

Validation must exercise an unchanged guest image: compare client pixels
outside an opened Volume/menu/native window and compare the entire client
after that overlay closes or moves away. An animated game can repaint over
the defect and conceal it. A host regression should execute the production
desktop event and paint definitions with a clipped composition surface;
the screenshot gate should use the current native DOSVM module, real input
and read-only RAM observation, keeping all fixture changes on a private
image. Physical T23 qualification remains separate from emulator results.

## Focused validation

`scripts/tests/test_desktop_overlay_host.py` compiles the exact production
`paint()` and `app_event()` definitions against a clipped host pixel buffer.
It passes Volume and menu composition over a static DOS client and a second
native window across eight separate bands, complete repaint after exposure,
an empty overlay, dialog painting and an unavailable wallpaper module.
Removing the new desktop-surface guard fails the pixel-retention check,
demonstrating that the regression test detects the original overwrite.
Both host test cases pass in a 512 MiB RAM / 128 MiB swap systemd scope.
OpenWatcom compiles the updated desktop module with the production warning
and error flags; Python syntax and gate CLI checks also pass.

The [pre-fix QEMU reproduction](../../build/full/t23-next/qemu-overlay-before-retry-20261008/report.json)
reaches the static native DOSVM client and opens Volume using real input.
Outside Volume's actual 220x60 surface, 281019 of 281028 compared client
pixels differ from the unchanged baseline. The
[captured Volume frame](../../build/full/t23-next/qemu-overlay-before-retry-20261008/volume-open.png)
therefore reproduces the wallpaper overwrite while the guest is idle.
The source image remains unchanged and no guest RAM writes are used. Its
SHA-256 is `9d265362702e25d3c220fac12cd84c5055ec82e13d19220fb8fccc7f00c17620`;
the report records the old DESKTOP.APP hash separately. The failing gate is
expected evidence of the old defect, not a successful corrected-build run.

The [first corrected-image run](../../build/full/t23-next/qemu-overlay-after-20261008/report.json)
has zero differences across 281028 exposed client pixels with Volume open,
all 281228 client pixels after Volume closes, and 148748 exposed pixels while
Files covers the DOS client. That run stops at the subsequent taskbar raise
assertion; it establishes the wallpaper-layer correction but does not yet
pass the complete interaction gate.

`scripts/qemu_test_desktop_overlay.py` uses the native DOSVM module with
`scripts/fixtures/vga_overlay_guest.asm`, an unchanged VGA image waiting for
keyboard input. Its planned runtime checks cover Volume and CiukiOS menus,
native Files-window opacity, preservation outside each overlay and complete
client recovery after exposure. It also raises DOS and Files in both orders
and minimizes/restores DOS, checking unchanged geometry, retained guest
session and exact client pixels. Volume's mask is its actual 220x60 surface;
menu masks include only the measured bevel and its three-pixel shadow,
independently of any client pixel differences. The corrected-build runtime
result passes the complete interaction gate, as recorded below. Physical T23
qualification remains pending.

`scripts/qemu_files_game_launch.py` provides a separate default-launch helper
for the installed DOOM.COM and WOLF3D.COM shortcuts. It navigates real Files
to TestGames, selects Details/Name ordering, reads the actual shortcut
catalog, and double-clicks the corresponding row without game arguments.
Files changes its working directory before launching a COM/EXE; a Run-field
launch with `-warp` does not exercise that same context. The helper verifies
both the Files execute and DOSVM open markers contain exactly the bare path.
Its button-down/up/down/up sequence follows the timing model described in
[Microsoft's mouse input documentation](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-mouse-input#double-click-messages);
CiukiOS Files implements its own threshold of fewer than nine BIOS ticks.
No paint-completion wait is inserted between the two clicks. Game runtime
qualification belongs to the integration owner's gate.

## Taskbar ownership and keyboard validation

The taskbar fix dispatches actions 200..231 with `ui_hit_target == 0` before
module controls 100..248. `ui_hit_target` is the hit record's click owner;
the mouse release path checks it against the original press owner.
`ui_hit_owner` describes the surface being painted and is reset after
window drawing, so it cannot distinguish these clicks. Module-owned
controls with the same numbers continue through `app_action`.
Generic Tab traversal selects the active window's owner and excludes IDs
above 199; Enter uses that selected focus. The change therefore preserves
the current keyboard focus namespace. Explicit module-generated shell
actions are a separate existing protocol, not a new keyboard ABI.

`scripts/test_taskbar_routing.py` executes the exact production dispatch
prefix and next/previous focus walkers in a 16-bit CPU fixture. Its
[200 passing scenarios](../../build/tests/taskbar-routing-20261008/report.json)
cover taskbar IDs 200,208,211,231 while Files, DOS and window 31 are active,
present/absent modules, unrelated paint owners, colliding module controls,
native boundaries and module quiet/redraw/command responses. Both Tab
directions skip taskbar owner 0 hits and module IDs above 199. Removing the
owner route reproduces the original collision: Files consumes action 211
and the DOS window is never raised. The CPU test runs in a 512 MiB RAM /
128 MiB swap scope; the separate screenshot gate verifies full-image runtime
interaction.

The complete [corrected-build screenshot gate](../../build/full/t23-next/qemu-overlay-verified-20261008/report.json)
passes all nine exact pixel comparisons: Volume open/close, Files over DOS,
DOS raised over Files, DOS restored after minimization, Files raised over
DOS, Files closed, and start menu open/close. Every compared pixel outside
the real occluder is unchanged. The guest is not relaunched by restore, its
window geometry is preserved, no guest RAM is written, and the source image
is unchanged. The pointer is moved away from the popup before measuring
its bevel so the cursor cannot interrupt that independent boundary check.

## Default Wolf shortcut and input decision

The installed TestGames `WOLF3D.COM` is GAME_KIND2 in
`src/com/testgame_launch.asm`; it changes to `\APPS\WOLF3D` and executes
the explicit `\APPS\WOLF3D\WOLF3D.EXE` with the caller's command tail.
This shortcut uses the original DOS engine. It does not select the separate
APPS Wolf4GW audio wrapper or the native WOLFWIN application. Qualifying a
no-argument launch must exercise this actual path.

The original [id Software DemoLoop](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_MAIN.C)
waits for input on its intro/title sequence, then enters the control panel;
its initial menu selection also depends on configuration and edition.
Use a dismissing key, observe the main menu, then N and Enter to select New
Game by its initial letter. The upstream
[menu implementation](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_MENU.C)
accepts an initial letter independently of the old selection, defaults to
Episode 1 and Normal difficulty, and accepts Enter at each stage. Observe
those transitions before proceeding. From gameplay, F10 opens the exit
confirmation and Y calls `Quit(NULL)`, which returns DOS exit status 0.
Evidence of rendering a title/menu alone does not establish gameplay:
the runtime gate should retain the played-scene screenshot and verify
movement changes the view before proving a clean game-initiated exit.

For Files navigation, Alt+V,N invokes `C_SORT_NAME`, which explicitly sets
`sort_desc=0` in `src/apps/files.c`; only clicking an already active Details
header toggles the direction. The helper's independently sorted shortcut
catalog and fixed Details row are therefore consistent with that menu
command without assuming the previous sort state.

The first Files launch attempt exposed an input race: short Alt+D pulses
and blind typing left the list focused or queued an incomplete path. The
helper now clicks the real Address field, observes its near-pointer
`struct field` in the loaded module, verifies Ctrl+A selection, and waits
for every exact typed prefix before sending the next character. The
existing failed run's read-only 16 MiB capture contains one valid address
field with the unexpected `C:\SBEMU` value, confirming what was navigated.
It uses no private production export and no RAM writes.
Discovery requires an initialized drive/path and fixes the field's structure,
pointer and segment. Subsequent edits accept partial prefixes such as `c`
and `c:` in that same validated field. The next failed capture showed a
correct first-character `c`; requiring a complete path during every edit
had incorrectly rejected it. Initialization and field-update waits remain
bounded, with the exact prefix checked after every real keyboard event.

`scripts/qemu_test_testgames_wolf.py` provides the separate original-engine
gate. Its recognizer decodes the installed assets according to the upstream
[Huffman/chunk format](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_CA.C)
and [picture alignment/planar layout](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_VH.C),
using the [WL6 chunk identifiers](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/GFXV_WL6.H).
It compares indexed spatial colour classes, with contrast checks rejecting
blank or faded frames, and embeds no commercial images. HUD masking follows
only the changing fields in
[WL_AGENT.C](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_AGENT.C).
The gate verifies intro/menu/episode/difficulty pixels, the actual HUD,
rotation changes exceeding three equally timed passive frame changes,
game-initiated quit and a fresh valid 448-byte CDVS exit snapshot. Failures
retain a bounded 16 MiB RAM capture, registers, logs and available snapshots
from the private image. The fresh assembled SHELL must exactly match the
image before RAM observation begins.

Six synthetic host tests pass for Huffman/plane decoding, dynamic HUD
masking, palette changes, rejection of titles/blanks/noise/shifted images,
address-field validation and partial-prefix editing of the bound field.
Separately, nine pictures from the installed
WL6 data decode and match synthetic RGB conversions; all nine blank negative
controls fail as intended. Both checks run in 512 MiB RAM / 128 MiB swap
scopes. The standalone Wolf gate's CLI and Python syntax checks pass;
its complete real QEMU gameplay result is recorded in
[the final validation document](../validation/2026-10-08-t23-desktop-fit-dos.md).

The no-argument Doom run subsequently reached Files successfully, but the
old generic red/nonblack HUD check accepted its Skill menu. Doom readiness
must instead compare the actual STBAR WAD patch against static statusbar
pixels and wait for all main/episode/skill menu cursors to disappear. The
decision follows id Software's
[statusbar and widget coordinates](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/st_stuff.c),
[patch drawing](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/v_video.c)
and [dynamic widget drawing](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/st_lib.c).
Keep the existing bounded readiness polling; a final Enter may still be
queued, so colour or a fixed extra sleep cannot establish that a level began.

The [15-case screenshot regression](../../build/tests/doom-gameplay-screen-20261008/report.json)
passes in a 512 MiB RAM / 128 MiB swap scope. It rejects the actual Skill
false-positive, title/main/episode menus, blank output and two incomplete
wipes. Three fully revealed captured HUDs match all 4653 compared static
pixels. It also rejects each menu over an otherwise valid HUD and accepts
changes confined to the masked widgets. The observed Skill frame matches
only 2.407% of STBAR pixels and retains its row2 cursor, while the old
colour-only condition accepted it. `qemu_test_t23_runtime.py` now polls this
WAD-based readiness and all three menu states before checking movement;
the polling timeout and sleeps are unchanged. Its syntax checks pass;
the next real Files/game run remains scheduled by the integration owner.

The subsequent no-argument Doom gate completed twice, but the first Wolf
gate stopped before `[FILES] execute`: its screenshot shows WOLF3D selected
and its serial log contains no DOSVM launch. The helper's fixed 80 ms button
pulses could therefore prove selection without proving a second delivered
press. QEMU's [monitor specification](https://www.qemu.org/docs/master/system/monitor.html)
defines `mouse_button` as changing the button state, and `pmemsave` as reading
physical memory; command completion does not acknowledge guest consumption.
In `shell_gui_input.inc`, a consumed press updates `ui_previous_button` and
sets `app_capture` to the application window plus one before calling
`app_mouse`; release clears capture. `shell_apps.inc` populates `HOST.ticks`
directly from the BIOS low-word clock, which Files compares against its first
press. The helper will acknowledge both button edges through those existing,
read-only fields, bound the first-to-second press interval conservatively
with that same clock, and retry an expired pair only after release. It will
retain the actual Files execute/DOSVM open checks and will not change the
production double-click threshold or write guest RAM. This is a harness
correction; the failed run did not reach the Wolf engine.

Seven bounded CPU cases pass for the new edge protocol: the former blind
pulses collapse to one press in an asynchronous poll model, acknowledged
edges launch correctly, an expired repaint retries with a fresh pair, the
BIOS low word can wrap, wrong owners and Shift/Ctrl presses fail, and
permanently slow polls fail within the limit while releasing the button.
The existing six Wolf asset/field recognizer cases also pass. The corrected
Wolf runtime gate remains for the integration owner to run sequentially.

That rerun verified the bare WOLF3D path and entered the original engine.
It stopped at the normal hardware/memory sign-on page, before the title.
[WL_MAIN.C SignonScreen/FinishSignon](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_MAIN.C)
renders the linked 64,000-byte `introscn` image and prints `Press a key`
before blocking in `IN_Ack`; this is separate from VGAGRAPH's title picture.
The original [SIGNON.OBJ](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/OBJ/SIGNON.OBJ)
contains its indexed bitmap. The gate will derive that bitmap from the
installed, unmodified executable and compare its unchanged image pixels, plus the
exact prompt rendered from installed font 0. The
[font renderer](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_VH.C)
uses glyph height, near offsets and per-character widths; the
[centering code](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_US_1.C)
places the 73-pixel prompt at x123,y190. Only after both matches will the
gate send a real acknowledgement key and wait for recognized rating/title
pictures or the main menu. No executable patch or command argument is added.

The installed image is at MZ module segment `2201:0000`, file offset 150544,
with SHA-256 `999d6f405bd45a2575aff5c83cfc9da183bbf28d811859a892ca5abc983ed5c7`.
Three relocation sites refer to that segment. Its artwork differs from the
upstream object's top rows, so the matcher reads the installed image rather
than assuming the source object is identical. Only fingerprints are stored
in the test code. [WL_MENU.C IntroScreen](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_MENU.C)
defines the changing memory bars and hardware check marks excluded from the
bitmap comparison; FinishSignon's bottom prompt strip is checked separately.
The captured page scores 1.0 on 42,025 static bitmap pixels and 730 prompt
pixels, including 184 foreground glyph pixels. Fourteen captured/synthetic
boundary cases pass in `build/tests/wolf-signon-screen-20261008/report.json`:
changed hardware indicators and palettes still match, while blank/noisy
images, missing/shifted prompts, `Working...`, headerless pages, a prompt
alone, title images and modified or non-MZ executables are rejected. The six
existing Wolf screen/field cases also pass. The next sequential Wolf runtime
run remains pending with the integration owner; gameplay is not yet proven.

The next runtime reached the sign-on, Options and New Game screens, then
stopped on a complete Episode 1 menu. Its installed `C_EPISODE1PIC` (chunk 30,
48 by 24 pixels) matches every one of the matcher's 955 sampled pixels with
ten colour classes, but its brightest sampled palette channel is only 65.
The generic 80-channel contrast guard had rejected an otherwise exact asset.
[DrawNewEpisode](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_MENU.C)
places that dark mountain thumbnail at x40,y23 after the menu palette fade;
the [WL6 chunk definitions](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/GFXV_WL6.H)
identify it as chunk 30. The matcher retains its default 80 contrast guard
and uses a measured 64 minimum only for this thumbnail, while also requiring
the unchanged exact Episode 1 gun cursor. Blank, faded and title images
must still fail; pixel/class checks are unchanged.

Eleven captured/negative cases pass in
`build/tests/wolf-episode-screen-20261008/report.json`: the captured complete
menu matches all 955 pixels and its exact cursor, the default 80 guard still
rejects that dark asset, and blank/noisy/title images, incomplete palette
fades, a missing cursor and shifted/corrupt thumbnails fail. All six existing
Wolf screen/field cases also pass. Runtime continuation remains scheduled by
the integration owner.

The next run stopped during a dark title fade after the sign-on key. Its
snapshot remains ready, with successful video/device queries, no unsupported
instructions or fatal video error, and two forwarded keys without drops.
The fixed ten-second menu loop had rejected an ordinary transitional frame.
The original [DemoLoop](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_MAIN.C)
loads/fades the title before checking `IN_UserInput`; the
[palette implementation](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_VL.C)
publishes intermediate palettes before its final palette. The gate will use
bounded intro/menu states: only an exact complete sign-on, rating, title or
credits page permits an acknowledgement; incomplete or fading frames wait
up to the configured game timeout, and an already acknowledged page cannot
receive duplicate keys before a new recognized page/menu. A limited number
of acknowledgements bounds the total wait. This changes test sequencing and
does not infer a production fault from an intermediate screenshot.

Spatial colour-class consistency also holds during a palette fade, so it
cannot alone establish a complete intro/menu. The linked
[GAMEPAL.OBJ](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/OBJ/GAMEPAL.OBJ)
palette matches the installed EXE's 768-byte block exactly (file offset
253128, SHA-256 `9c6d75cf32e883cc936374b718b0d628e9cf449cecaa32c1415f2877aefe1670`).
The gate extracts that block from the unchanged executable and compares
intro/Options picture pixels against the full palette as well as their
existing shape/class checks. It allows two RGB units for DAC expansion
rounding and requires 99.5% agreement, preventing an intermediate fade from
receiving a key before the original `IN_UserInput` call is ready. The stored
fingerprint and standard EGA prefix locate the palette without embedding it.

Seven CPU state-machine cases pass, covering fades longer than ten seconds,
one key per recognized page, a fresh configured timeout for the next page,
unknown-image rejection, repeated-page timeout and the acknowledgement cap.
Nine captured/palette cases pass in
`build/tests/wolf-intro-screen-20261008/report.json`: the captured dark title
waits, the previously captured complete Options menu matches the installed
palette exactly, and synthetic installed title pixels match only at the
complete palette. Half, 90% and 97% fades passed the former class-only
recognizer but now remain unready. Blank and unrelated images fail. The
configured 90-second timeout applies to each recognized transition, with
at most four acknowledgement keys. These harness changes are frozen pending
the integration owner's next runtime; no production file was changed.

The next runtime completed the intro and all three menus, but its first
`wolf-gameplay-hud-client.png` is the actual Get Psyched preload screen.
Its static HUD also exists during preloading, so HUD shape alone was an
incorrect gameplay boundary. The three passive frames still show that page;
the after-input frame is an ordinary level during its dark palette fade.
The snapshot remains ready with no unsupported/fatal video error or device
model error. The HUD masks match the original DrawFace/DrawHealth/DrawAmmo
coordinates, so they will remain unchanged.
[GameLoop](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_GAME.C)
calls [PreloadGraphics](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_INTER.C)
before PlayLoop; PreloadGraphics draws GETPSYCHEDPIC at x48,y56, fades in,
preloads, waits for input, then fades out. The
[WL6 chunk list](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/GFXV_WL6.H)
identifies that picture as chunk134. The harness will require the existing
exact masked HUD at the full installed palette and absence of that exact
loading picture before timing passive/input frames. After input it will
wait boundedly for the same settled gameplay condition, preserving contrast
and pixel guards instead of treating an intermediate fade as a lost screen.

Seventeen captured/asset cases pass in
`build/tests/wolf-gameplay-screen-20261008/report.json`. All four captured
preload/HUD frames now fail gameplay readiness despite matching the old HUD
recognizer; the complete preload frame matches the palette but is rejected
by its exact loading picture. The captured dark level waits for palette
completion. Menu, blank, unrelated, faded and corrupt-HUD frames fail;
asset-based full-palette HUD cases pass with the original dynamic regions
excluded. Those synthetic positive cases validate the matcher and do not
claim a completed runtime. Six existing Wolf screen/field cases also pass.
[PlayLoop and UpdatePaletteShifts](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_PLAY.C)
render before initial fade-in, suppress key handling while screenfaded, and
restore gamepal after damage/bonus palette shifts; the
[video macros](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_VH.H)
use thirty vertical retraces for normal fade-in/out. The gate preserves its
80 contrast guard and all 4,534 static HUD shape/class samples, additionally
checking 7,680 unmasked pixels against the full installed palette. Initial,
passive and post-input readiness use bounded waits. No production source
or image was changed; the integration owner schedules the final runtime.

The next runtime, `qemu-wolf-files-ready-20261008/report.json`, reached a
settled original level, measured zero changed pixels in all three passive
samples, then 20,059 viewport pixels after the commanded rotation. Its
fixed-delay F10 screenshot still showed gameplay, but the later failure
capture already showed the genuine three-line quit confirmation. This
establishes a harness readiness race rather than missing F10 delivery.
[CheckKeys](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_PLAY.C)
checks the original keyboard state before calling the F10 control panel;
[Message and Confirm](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_MENU.C)
draw a randomly selected entry from nine `endStrings`, use installed font 1,
center its measured lines in the 160-pixel gameplay window and poll Y/N/Escape.
The gate retains its separate real F10 press/release and now waits for all
exact text lines before sending Y. It does not accept generic frame changes.

The harness extracts the unchanged executable's nine 80-byte message records
at offset 147168, guarded by SHA-256
`92e4a9ecd5acb041a4efb63f637b2343641daf047e3024c88f98b81626701b78`.
It derives every line's glyphs from the installed WL6 font and uses Message's
exact coordinates. Confirm's reserved blinking cursor remains outside the
checked text rectangles. The actual failure dialog matches message 4 with
all 7,943 text-rectangle pixels consistent, score 1.0 and contrast 142.
Twenty captured/asset cases pass in
`build/tests/wolf-quit-screen-20261008/report.json`: the actual confirmation
passes; the early F10/gameplay frames, menus, blank, unrelated, missing-line
and dark-palette frames fail. Synthetic installed-font layouts test all
nine messages without claiming those dialogs occurred at runtime. Syntax
and diff checks pass. Production and image hashes remain unchanged; the
integration owner runs the final QEMU gate.

## Final integration result

The final no-argument original Wolf gate passes in
`build/full/t23-next/qemu-wolf-files-complete-20261008/report.json`: actual
level readiness, three unchanged passive views, 20,243 pixels changed by
Right, exact exit confirmation, game-initiated return code 0000, a fresh
validated exit snapshot and desktop return. This completes the runtime gates
previously described as pending above. The source image is unchanged and no
guest RAM is written. The
[final validation record](../validation/2026-10-08-t23-desktop-fit-dos.md)
also records the passing two-launch DOOM and complete overlay tests. Physical
T23 gameplay and latency remain separate qualification steps.
