# Desktop interaction audit

Milestone A, 2026-10-15, worktree baseline `309007c6`, directive f2-67.
The initial inventory below records the behaviour before implementation.
`ok` means the host replay or widget harness exercises that behaviour;
`wrong` means implemented behaviour conflicts with the directive; `missing`
means the required interaction has no implementation. Host renders establish
pixel output and state transitions, not physical display timing.

The baseline command is `nice -n 19 python3 -B -m unittest discover -s
tests/host/desktop -v`. Its complete output is retained in
`build/codex/f2-67-before-tests.log`. Review frames are in
`build/host/f2-67/before/`. The production application replay covers Files,
CiukNote, Task Manager, the desktop shell and slow input delivery. The
rendering replay compares incremental presentation with full composition.
Existing goldens include explicitly unpinned frames; an unpinned digest is
not a visual approval. The lead reviews the paired PNGs.

Baseline discovery ran 98 tests in 273.964 seconds: the executed desktop,
widget, application and rendering replays passed, five image-dependent tests
were skipped, and two modules failed to import because this discovery form
does not give their relative imports a package. Their separate package-form
run is recorded in `build/codex/f2-67-before-package-tests.log`. Subsequent
full runs use `-s tests/host/desktop -t tests/host`.

## Windows and shell

| Interaction or control | Initial behaviour and evidence | Status |
| --- | --- | --- |
| Click activation and stacking | Left click focuses and raises, releases captured input; `desktop_test.c` input/lifecycle and application replay | ok |
| Secondary click activation | Secondary clicks do not activate the target application | wrong |
| Launch activation | CREATE_WINDOW focuses the new client; a held menu defers focus until it closes | ok |
| Close and peer exit | Cooperative close, deadline, surface cleanup; selects highest remaining order, without preferring another window of the same process | wrong |
| Active and inactive captions | Shared frame painter and task buttons follow focus; three-theme scene goldens | ok |
| Title drag | Retained client surface moves; screen clamp keeps the full window in the work area | ok |
| Title double click | Rolls up the window instead of maximizing; `input.c` title-click branch | wrong |
| Maximize button | Uses resize and saved restore rectangle, but draws a roll-up glyph and allows resize while maximized | wrong |
| Resize edges and corners | Eight hit regions and bottom-right grip; v2.1 limits, steps and backing swaps exercised in `desktop_test.c` | ok |
| Resize visual feedback | New frame exposes face-colour strips until its replacement surface arrives; Files six-frame replay | wrong |
| Resize opposite edge | Left/top anchoring exists, but size limits are applied after screen clipping, which can push geometry out of bounds | wrong |
| Live application layout | Replacement surfaces call layout/anchors; Files replay checks exact pixels, model, focus and bounded surface lifetime | ok |
| Keyboard move and size | No system-menu Move or Size operations | missing |
| Minimize and restore | Taskbar click toggles, task context menu restores/closes; highest visible order becomes active | ok |
| Transition outlines | No minimize, restore or maximize animation | missing |
| New-window cascade | Position depends on reused channel slot, so successive launches can overlap exactly | wrong |
| Alt+Tab and reverse | Ordered list, application icons, minimized-window restore, Alt-release commit and Esc cancel; `desktop_test.c` shell checks | ok |
| Alt+F4 | Close active client; desktop-only shortcut opens shutdown; input release is handled | ok |
| Alt+Space | No window system-menu shortcut | missing |
| Alt+Esc | No immediate window rotation | missing |
| Modal application dialogs | In-surface panels centre on the client and intercept input; dialog harnesses exercise default, cancel and focus return | ok |
| Modal resize | Existing panel is clamped into view rather than recentered on its resized owner | wrong |
| Shell message owner | Independent shell message has no owner or input block | missing |
| Start button and cascading menus | Pointer/keyboard traversal, separators, disabled actions, submenu placement; shell replay and look goldens | ok |
| Desktop icons | Selection, double click, keyboard navigation, drag, inline rename, file commands and context menus; folder/shell harnesses | ok |
| Taskbar | Activation, minimize/restore, context actions, clipped titles and hover tips; tray/layout harnesses | ok |
| Tray volume | Slider, mute, keyboard steps and media keys, drag capture; model and transient harnesses | ok |
| Tray network and media | State popups, safe remove, scroll, notifications and tooltips; network/media harnesses | ok |
| Clock and notifications | Measured clock width, expiry and slide repaint, click actions; transient/clock harnesses | ok |
| Run, About, shutdown, AutoPlay | Built-in edit, tabs, buttons, choices and cancellation; shell, shutdown and AutoPlay harnesses | ok |

## Cursor inventory

| Shape or routing | Initial behaviour | Status |
| --- | --- | --- |
| Arrow | Fixed 11 by 17 drawing, duplicated for hardware upload and software painting | wrong |
| Busy and arrow with busy | No desktop shapes or automatic launch/unresponsive indication | missing |
| Text I-beam | Edit and text area hit tests do not request a cursor | missing |
| Not allowed | No shape for blocked modal input or disabled controls | missing |
| Horizontal, vertical and both diagonal resize | Resize hit tests never change the arrow | missing |
| Move | Title drag and splitter capture retain the arrow | missing |
| Link hand, crosshair, precision select | No shapes or application request API | missing |
| Hot spots and shape switching | Hardware service accepts 32-row masks and negative origins, but desktop uploads only the arrow once | missing |
| Cursor restoration | Software save-under and hardware-fault recovery exist; acceleration/performance harnesses compare full and incremental frames | ok |

## Toolkit widgets

These rows cover all `du_kind` values. Applications share their input and
drawing implementations in libdeskui; the individual application rows below
identify their additional controls and model tests.

| Widgets | Behaviour and host coverage | Status |
| --- | --- | --- |
| Label, separator, toolbar, group, header, status | Clipped themed drawing, mnemonic labels, status panels; toolkit and application goldens | ok |
| Button, tool button | Hover, press, release inside, disabled state, focus, default action, split arrow, mnemonics; toolkit harness | ok |
| Checkbox, radio | Pointer and keyboard toggle, grouped radio exclusion; theme/toolkit harnesses | ok |
| Single-line edit | Caret, keyboard editing, selection, clipboard, horizontal scroll; toolkit harness; pointer text selection is f2-68 | ok |
| Text area | Caret, selection, wrap, scroll, undo, clipboard; text-area and CiukNote harnesses; selection refinements are f2-68 | ok |
| List view | Details/icons/list, selection, sort, keyboard open, context menu, drag and scroll; Files/toolkit harnesses | ok |
| Tree | Expand/collapse, keyboard traversal, scroll, selection; Files/Registry Editor harnesses | ok |
| Scrollbar | Arrow/page repeat, thumb capture, vertical/horizontal limits; toolkit/application harnesses | ok |
| Splitter | Captured pointer motion updates application layout; Files/IDE harnesses; matching cursor is missing | wrong |
| Progress, graph | Bounded values and timer repaint; toolkit/Task Manager harnesses | ok |
| Tabs | Click, keyboard cycling, focus and active page; Task Manager/Control Panel harnesses | ok |
| Canvas | Pointer capture/cancel, application drawing and keyboard delivery; Paint/Terminal/Graphics Demo harnesses; cursor request is missing | wrong |
| Menu bar and popup | Pointer/keyboard traversal, accelerators, mnemonics, disabled items and dismissal; toolkit/application harnesses | ok |
| Message/input/open/save/progress dialogs | Modal input, centring, file navigation, default/cancel, progress and optional validation; transient and application harnesses | ok |

## Applications

| Application and controls beyond shared widgets | Initial behaviour and coverage | Status |
| --- | --- | --- |
| Files: navigation toolbar, address, places/folders trees, splitter, list, rename, status, transfer dialogs | Model/UI replay, file commands and six resize captures preserve path, selection and focus; cursor feedback absent | wrong |
| CiukNote: editor, find/replace bars, menus, status and file prompts | Application/key-script replay and text-area tests; layout callback supports resizing; I-beam absent | wrong |
| CiukIDE: project tree, editor, build/output panes and job controls | IDE model/UI harness and resize callback; text/splitter cursor feedback absent | wrong |
| Control Panel: applet launcher, display, mouse, keyboard, sounds, date/time, fonts, system/device pages | Control/display/device/font/sound harnesses and captures; minimum is the fixed page size, growth uses anchors | ok |
| Task Manager: task/process lists, tabs, history graphs, action buttons | Process model, view and production replay; bounded resize anchors | ok |
| Terminal: cell canvas, scrollbar and power prompt | Input/command harness, resize layout, modal confirmation; cursor API absent | wrong |
| Registry Editor: key tree, values list and edit dialogs | Registry/UI harness with layout callback; edit cursor absent | wrong |
| Image Viewer: image canvas, zoom, navigation, scrollbars and status | Viewer harness, image decoding and resize callback | ok |
| Help: topics tree, content pane, navigation toolbar | Help harness and resize callback; link cursor absent | wrong |
| Calculator: display, keypad, memory and arithmetic actions | Calculator model/UI harness; deliberately fixed size | ok |
| Clock and Calendar: clock face, month navigation, calendar cells | Clock model/UI harness; deliberately fixed size | ok |
| Character Map: font/symbol grid, character field, copy action | Character Map harness; deliberately fixed size | ok |
| Volume Control: master/device sliders, mute and status | Mixer harness; deliberately fixed size | ok |
| Music Player: transport, seek, volume, playlist, menus and status | Player/playlist/view/native harnesses; resize callback | ok |
| Graphics Demo: scene canvas, diagnostics, benchmark and mode controls | Frame/performance/application harnesses; completion-paced resize | ok |
| CiukPaint: toolbox, options, palette, canvas, scrollbars, text tool and dialogs | Paint model/UI harness and resize callback; tool cursors and selection belong to f2-68 | wrong |
| Demo: checker surface, typed text, input counters and heartbeat | Native demo and gate harnesses; fixed surface is intentional | ok |
| DOS session: window, task menu and full-screen action | Host autorun/loading and desktop input-resync harnesses; no guest run in this task | ok |

## Research and implementation decision

Checked before implementation against the repository:

- [Cursor creation](https://wiki.libsdl.org/SDL2/SDL_CreateCursor)
  defines bitmap masks and hot spots. Keep one original 32 by 32 mask set,
  use its hot spot for both software painting and the existing hardware
  cursor service, and validate application requests at the desktop boundary.
- [Client minimum sizes](https://wiki.libsdl.org/SDL2/SDL_SetWindowMinimumSize)
  apply to client dimensions. Keep the v2.1 client-area limits and increments;
  fit the resulting decorated frame to the work area with its opposite edge
  held fixed during a resize.
- [Pointer grabs](https://www.x.org/releases/X11R7.5/doc/libX11/libX11.html)
  retain a cursor during capture. The desktop owns the resize/move cursor
  until release or resynchronization; an application cannot override chrome.
- [Application window states](https://specifications.freedesktop.org/wm/latest/ar01s05.html)
  distinguish modal ownership, maximization, shading, minimization and focus.
  Keep those states separate, preserve restore geometry and enforce owner
  blocking at input dispatch. In-surface dialogs remain part of their owner.

Use live content feedback: scale the retained backing until a fully rendered
replacement arrives, continue sending coalesced layout proposals during the
drag, and damage old and new frame extents. Reuse the capped presenter and
surface lifetime handshake. Transition outlines advance by monotonic time
without sleeping or allocating. Keyboard Move/Size uses the same geometry
limits as pointer capture, with Enter accepting and Esc restoring the start.

The read-only T23 disk log for image `a738204a` records a failed engine probe
and quarantine at lines 355–358 (`caps=0`, `init_result=-5`). Consequently
that acquisition used software presentation. Its damaged serial output does
not identify individual resize input sequences. This task must qualify
software cursor and resize feedback on the host; the lead checks physical
timing and both presenter paths separately.

## Implemented behaviour and handoff

The baseline inventory above is retained as the record of the audit.
The f2-67 interaction replay now covers all eight resize directions, stepped
work-area limits, opposite-edge anchoring, title maximize/restore, keyboard
Move/Size accept/cancel/resync, modal owner blocking, cascade, activation,
same-application close fallback and all thirteen cursor shapes. Existing
Files replacement-surface tests continue checking live layout, exact final
pixels, focus/model retention and descriptor lifetime. The presenter tests
compare software and hardware cursor output at all four screen corners and
exercise shape-upload failure recovery.

Application grouping uses the program name bound by the launcher, with a
same-PID fallback for admitted clients without launch metadata. Window titles
do not establish identity. This implements the directive's same-application
fallback across separate instances as well as windows of one process. The
[inter-client conventions](https://www.x.org/releases/X11R7.7/doc/xorg-docs/icccm/icccm.pdf)
distinguish application identity and window groups from display titles;
Ciuki's launcher already supplies the trusted program name.

The system menu opens through Alt+Space, title-bar secondary click or the
taskbar context menu. Move and Size use arrow keys, Enter accepts, Esc cancels;
pointer release after choosing an item leaves keyboard sizing active. Size
respects increments larger than the default eight-pixel keyboard step.
Title drag retains the established hit region. Title double click uses the
configured double-click interval and restores the exact saved rectangle.
Outline transitions take 150 ms in six monotonic steps, damage only their
old/new edge strips and do not block input. A held resize scales its retained
surface until a complete replacement layout arrives. Physical scanout timing
still requires the lead's hardware check.

Cursor source masks are in `assets/cursors/cursors.h`, with their licence and
format in its README. The desktop build provenance includes both files.
Software painting and the existing hardware cursor service share those masks
and hot spots; no driver ABI change is needed. Resize/move capture owns its
cursor until release. Run and desktop rename fields use their shell geometry;
libdeskui detects edits, text areas, embedded scrollbars, splitters, disabled
widgets and modal panels. Terminal requests text selection over its grid.

For f2-68, `DESK_SET_CURSOR` is appended to the opcode enumeration. It is a
client-to-desktop v2 message of exactly eight bytes: the normal four-byte
header, then one little-endian shape word at offset four, with no attached
descriptors. Valid values are `DESK_CURSOR_ARROW` through
`DESK_CURSOR_PRECISION`; `DESK_CURSOR_COUNT` is excluded. The desktop stores
the request only on the sending client's existing window and applies it
inside that client area. Invalid requests follow the existing peer-isolation
path. Chrome and modal ownership take priority. No additional authority is
granted.

Applications can call `du_cursor(ui, shape)` for a default or set
`widget->pointer_cursor` for a canvas/link widget. `du_cursor_at()` supplies
the automatic control hit test, and `du_window_turn()` sends a changed shape
through its normal bounded FIFO, retaining it across backpressure. Paint tool
drawings, size previews and text-selection changes remain in f2-68.

The new replay pins every refinement frame in
`tests/host/desktop/interaction-goldens.json`. Existing affected caption and
shell-dialog goldens are re-recorded. Paired captures, a cursor gallery and
changed hash records are under `build/host/f2-67/`; these are host review
artifacts. The lead still reviews the PNGs and qualifies the integrated image
on QEMU and the T23.

Final host validation: the desktop suite completed 100 tests, OK with five
payload-dependent skips; the desktop gate passed ten tests. The refinement
replay was also verified against its recorded goldens after capturing the
Alt+Tab list while held open. Commands and verbatim output are in
`build/codex/f2-67-report.md` and its referenced logs. No guest image was built
or booted by the implementer.
