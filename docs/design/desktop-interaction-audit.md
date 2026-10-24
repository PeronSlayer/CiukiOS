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

## Window management and shell

| Interaction or control | Initial behaviour and evidence | Status |
| --- | --- | --- |
| Click activation and stacking | Left click focuses and raises, releases captured input; `desktop_test.c` input/lifecycle and application replay | ok |
| Secondary click activation | Secondary clicks do not activate the target application | wrong |
| Launch activation | CREATE_WINDOW focuses the new client; a held menu defers focus until it closes | ok |
| Close and peer exit | Cooperative close, deadline, surface cleanup; selects highest remaining order, without preferring another window of the same process | wrong |
| Active and inactive captions | Shared frame painter and task buttons follow focus; Ciuki Blue/Platinum scene goldens | ok |
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

## F2-68 milestone A: text selection and paint cursors, 2026-10-15

Baseline: branch `wt/f2-68`, before implementation. The toolkit and Files
host replay passed all five tests. Its frames are saved in
`build/host/f2-68/before/`; the complete output is in
`build/host/f2-68/baseline-deskui.log`. The physical logs for image
`a738204a` were read without modification. They establish the running
applications and input activity, but contain no selection or cursor-shape
measurements. The statuses below describe the source and host behaviour.

| Control | State | Observed behaviour |
| --- | --- | --- |
| Notepad document (`apps/ciuknote`, `DU_TEXTAREA`) | wrong | Drag, double-click word, Shift movement, replace-on-type and application edit commands exist. A third click starts a new character selection. The caret remains visible with a selection. |
| Notepad Find and Replace fields (`DU_EDIT`) | wrong | Drag and keyboard selection exist. Double-click selects the entire field instead of a word; click distance is ignored. The caret is painted across a selection. |
| Input dialogs, file dialog folder and filename fields | wrong | Reuse `DU_EDIT`, with the same click and caret defects. |
| Custom dialog text fields, including Paint dimensions and font scale | wrong | Reuse `DU_EDIT`, with the same defects. |
| Control Panel text fields | ok | This version has no inline editable text fields. Open/save and custom dialogs use the common controls above. Static labels and font previews are display content. |
| Control Panel lists: applets, themes, wallpapers, resolutions, depth, keyboard, sounds, fonts, drivers, leases and autoplay | wrong | Active rows and selected flags are maintained by controllers. Highlighting depends on application flags; a generic list can move its active row without displaying a selected row. |
| Toolkit lists and trees, file dialog list | wrong | Selection events are dispatched to the controller. The toolkit itself does not maintain a selection highlight for lists without a selection controller. File dialog and Files explicitly update item flags. |
| Files address and inline rename fields | wrong | Reuse `DU_EDIT`; the rename field selects the initial name, but double-click and selected caret have the common defects. |
| Terminal output | wrong | Drag and copy exist. Word/line clicks and keyboard selection are absent. Selection is restricted to output rows. |
| Terminal command line | missing | Manual character editing and paste exist; there is no highlighted command selection, cut, select-all or replace-selection editing. |
| Toolkit text clipboard | wrong | Fixed process-local 1,039-byte buffer. Notepad keeps a separate full-document copy and publishes only its first-line mirror. Generic text areas have no clipboard shortcuts. |
| Paint text overlay | wrong | Uses the text-area model and draws selected text. Pointer handling is separate, has no triple click, and always paints a caret. Generic clipboard shortcuts are absent from the model. |
| Character Map copy field | wrong | Uses `DU_EDIT`; inherits the field click and caret defects. Its Copy button already calls the toolkit clipboard. |
| Help topic body and development-tool output | wrong | Read-only `DU_TEXTAREA` controls; inherit the pointer/caret defects and lack generic copy/select-all shortcuts. |
| Development-tool source editor | wrong | Editable `DU_TEXTAREA`; inherits the same defects. Its source-colour callback already excludes selected text. |
| Registry and project dialogs | wrong | Use toolkit input and file dialogs, with the common field behaviour above. |

Every Paint tool currently retains the desktop arrow over the canvas.
The tool cursor and brush-size preview are missing in all cases below.

| Paint tool | State | Required pointer |
| --- | --- | --- |
| Rectangular select | missing | Crosshair |
| Free-form select | missing | No free-form tool exists in this baseline; selection cursor uses the same crosshair contract. |
| Eraser | missing | Square, with an exact footprint preview |
| Fill | missing | Bucket, active point at the pour tip |
| Colour picker | missing | Picker, active point at the sampling tip |
| Zoom | missing | Magnifier |
| Pencil | missing | Pencil, active point at the drawing tip |
| Brush | missing | Brush and current shape/size preview |
| Airbrush | missing | Precision crosshair and spray footprint |
| Text | missing | I-beam |
| Line, curve, rectangle, polygon, ellipse, rounded rectangle | missing | Crosshair |
| Canvas exterior, scrollbars, palette and tool options | ok | Arrow is appropriate. |

### Research and implementation decisions

The [GTK selection extension interface](https://docs.gtk.org/gtk4/signal.TextView.extend-selection.html)
separates selection granularity from hit testing. Its
[granularity enumeration](https://docs.gtk.org/gtk4/enum.TextExtendSelection.html)
identifies words and lines. Use a shared byte-boundary selection helper for
single-line, multi-line, terminal and Paint text, with successive nearby
clicks selecting a character position, a word and a logical line. A line
includes its terminating newline. Dragging after a word/line click extends
in the same units. Shift preserves the initial anchor.

The [W3C cursor definition](https://www.w3.org/TR/css-ui-3/#cursor)
defines selection cursors, zoom cursors and an image-relative hotspot. Apply
the tool cursor only to the image area; keep scrollbars and other controls
on their own cursor. Brush and eraser footprints follow the same image-to-view
mapping and even-size centring as the actual stamp. Preview pixels never
enter the saved image or undo history.

The repository already exposes `du_clipboard_set/get`, and Files publishes
its separate file clipboard by atomic replacement in `/system/settings`.
Keep the text clipboard API, extend it to bounded complete text, and publish
text through an independent atomic file so different native processes can
exchange it. Single-line paste consumes only the first line. Read-only
output supports selection/copy; cut, delete and typing affect only editable
input. Preserve Terminal's command history and interrupt keys.

The [C library rename documentation](https://sourceware.org/glibc/manual/2.24/html_node/Renaming-Files.html)
describes atomic replacement of the destination name. Compare this with
`apps/files/transfer.c`: use a temporary file in the same directory, complete
and flush it, close it, then rename it over the text clipboard. Reject failed
writes and malformed or oversized reads without discarding the cached copy.

Use the F2-67 `DESK_SET_CURSOR` request (v2, eight bytes: header then shape),
retaining its generic shape values. Add Paint shapes after the generic set.
Both cursor paths must use the same raster and hotspot. No image, guest or
physical qualification is claimed by this audit.

Free-form selection uses the existing Select tool with a Free-form option,
so both selection modes have a selectable crosshair cursor. A bounded path
is closed on release and converted to a pixel mask using the
[even-odd fill rule](https://www.w3.org/TR/SVG2/painting.html#FillRuleProperty),
as the existing polygon renderer already does. Masked pixels remain transparent
when copied, moved or transformed. The mask never becomes a canvas colour.

### Implemented state

| Area | State | Host-verified result |
| --- | --- | --- |
| Common fields, input/file dialogs and Files rename/address | ok | Nearby clicks select a position, word and line; word/line dragging keeps that granularity. Shift movement, Home/End, select-all, replacement, clipboard and deletion retain byte boundaries. |
| Editable text areas, including Notepad | ok | Shared word/line selection, keyboard extension and full-text clipboard. A selected caret is hidden. A triple-click in a one-row viewport keeps the selected line visible. |
| Read-only text areas | ok | Selection and copy work; cut and delete preserve content. Their keyboard clipboard commands take precedence over unrelated application accelerators. |
| Lists and trees | ok | Generic controls highlight the active selected item. Files explicitly retains ownership of its multi-selection and tree flags. Control Panel uses the same renderer. |
| Terminal command line | ok | Drag, word/line clicks, Shift movement, select-all, cut/copy/paste, delete and replacement share the field editing rules. |
| Terminal output | ok | Word and displayed-line clicks, drag and keyboard extension highlight output. A selected whole row copies its line ending. Cut copies output without deleting it. Busy Ctrl+C still interrupts. |
| Text clipboard | ok | Complete text up to 1 MiB, independent per-process caches and atomic shared-file publication. The host test clears the child cache before reading and publishes a replacement back to the parent. Single-line fields paste the first line. Missing storage retains the local clipboard. |
| Paint text | ok | Shared clipboard, word/line clicks, dragging, keyboard extension, replacement and selected-caret suppression. |
| Every Paint tool | ok | Selected-tool requests over the image, arrow over the image exterior and other controls, including captured motion. Both selection modes request the crosshair. Dialogs override the canvas cursor. |
| Brush, eraser and airbrush previews | ok | Image-coordinate footprints are drawn only into the view. For all twelve brush options and four eraser sizes, a host oracle compares preview boundaries against actual stamped image pixels at 1/4/8 zoom. |
| Cursor presentation | ok | Software and simulated hardware visible frames agree across shape changes, clipping and accelerator fallback. The old arrow retains its original damage bounds. |
| Free-form selection | ok | Closed bounded path, mask, copy/clear/move/undo and transparent paste/rotation tested without changing pixels outside the mask. |

Re-recorded frames, raw-frame digests and PNG review copies are under
`build/host/f2-68/`. The retained pins are in the desktop host suites and the
selection/Paint JSON manifests. The implementation report records the exact
test commands and their output. These are host results; the lead still runs
the integrated image on QEMU and the owner evaluates its behaviour on the T23.

## F2-67 and F2-68 merge: shared cursor contract

The window and text-control inventories above retain their original audit
scope. This integration preserves the thirteen generic cursor values and
rasters, appends the six Paint values after `DESK_CURSOR_PRECISION`, and
keeps one `DESK_SET_CURSOR` opcode with the eight-byte v2 wire format above.
The accepted shape range now ends at `DESK_CURSOR_ERASER`; the sentinel
`DESK_CURSOR_COUNT` remains invalid.

The [cursor bitmap and hot-spot specification](https://www.x.org/releases/X11R7.5/doc/libX11/libX11.html)
was checked again against `compositor.c`, `assets/cursors/cursors.h` and
`assets/cursors/paint.h` before resolving the merge: the mask controls
visibility, the image chooses foreground/background, and the hot spot is
relative to the image origin. Preserve the generic masks and the Paint tool
masks and hot spots unchanged. Software presentation, hardware upload and
the Paint capture helper use that same combined inventory. The generic
arrow retains its eleven-by-seventeen damage bounds.

The toolkit keeps `du_cursor`, `du_cursor_at` and `widget->pointer_cursor`.
The optional `widget->cursor_box` limits a tool cursor to its image region,
including during captured motion. Text-area capture, scrollbar/gutter hit
tests, modal override, clipboard and selection handling remain intact.
Build provenance includes both cursor headers and both format/licence notes.

Power-dialog and notification cursor overrides from F2-68 remain in the
combined router. F2-67 continues to own resize/move and captured client
feedback. The protocol/request harness checks that Power and notification
states suppress a stored Paint cursor and restore it after dismissal.

## F2-75: settings inventory and category tree

The audit precedes implementation. `apps/control/app.c` exposes thirteen
ungrouped launchers. Display combines theme, wallpaper, placement, background
colour, icon visibility, resolution and depth. Adapter shows display details
separately. Fonts previews installed faces in a separate launcher. Mouse
controls pointer and double-click speed; Keyboard selects the layout. Sounds
controls master volume, mute, event scheme, event volume, preview and an audio
hardware test. Date and Time shows UTC and controls clock formatting. System
contains version, processor, memory, uptime, a hardware count and serial boot
logging. Device Manager inspects devices/resources; Drivers inspects drivers
and leases. Network is an unavailable-settings notice; Power explains shutdown
and unavailable standby. AutoPlay manages remembered media choices. The
separate Adapter, Fonts, Device Manager and Drivers launchers scatter settings
which belong to Display or System. Network and Power must explain the actual
available actions and limitations without presenting inactive settings.

The desktop background menu initially contains New (Folder, Text Document,
Bitmap Image), Paste, a separator, Arrange Icons, Refresh, a separator,
Display Settings and Properties. New and Paste route to desktop file commands;
Arrange resets positions; Refresh reloads the folder. Both display entries
launch `control --panel=display`. Line up is missing. Start Settings contains
only six applet shortcuts and Task Manager. Tray secondary clicks have no
settings menu. These entry points do not share a category model.

The following tree assigns each user setting to exactly one applet. Internal
tabs and inspectors are parts of that applet, never additional launchers.
Status shown in the tray is a shortcut to its owning applet.

| Category | Applet | Owned settings and information |
| --- | --- | --- |
| Appearance | Display | Background: picture, placement, desktop colour, icon visibility. Appearance: theme palette and font roles, installed font preview. Settings: resolution, depth, adapter and read-only Savage engine state. No screen saver exists. |
| Sound | Sounds and Audio | Master volume/mute, system event scheme and volume, preview, audio hardware test. |
| Input | Keyboard | Keyboard layout. |
| Input | Mouse | Pointer speed and double-click interval. |
| Time | Date and Time | UTC status, 12/24-hour clock and seconds. Clock adjustment is unavailable. |
| Connections | Network | Adapter/link status; address configuration remains unavailable. |
| Power and devices | Power | Safe shutdown/restart entry point and current power-saving limitations. |
| Power and devices | Storage and removable devices | Removable-media choices and storage inventory. |
| System | System | Version, CPU, memory, uptime, Hardware check/device resources, drivers/leases, logs and next-boot serial logging. |

Keep this order, names and icons in a shared catalog used by Control Panel,
Start Settings and tray context menus. The panel uses a category tree beside
the selected category's applets. The existing theme supplies colour, type,
spacing, focus and button rendering. Display uses one tab sheet and uniform
OK, Cancel and Apply actions from every entry point. Apply previews; Cancel,
Escape, navigation away and caption close restore the entry values. OK keeps
the changes. Mode confirmation retains the bounded timer and safe rollback.

Before implementation, checked the upstream [tab container](https://docs.gtk.org/gtk4/class.Notebook.html),
[tree navigation](https://docs.gtk.org/gtk4/class.TreeView.html) and
[dialog response model](https://docs.gtk.org/gtk4/class.Dialog.html). The local
toolkit already provides focusable trees, lists, tabs and dialog responses;
reuse these primitives and keep the controller's draft, applied preview and
accepted values distinct. Merge only Display-owned keys when previewing or
reverting, preserving other settings and unknown keys. Remember panel size
in a separate small file so closing the panel cannot overwrite display edits.

Read-only physical logs for image `a395ffa8` report wallpaper/settings worker
stalls. They establish existing latency, not validation of these changes.
Retain deferred image decode and asynchronous settings writes. Host renders
and tests are the implementation evidence; the lead validates the integrated
image and the owner evaluates the T23.


The implemented home view contains seven categories and nine applets. Adapter
and Fonts selectors remain accepted as compatibility aliases for Display's
Settings and Appearance tabs. Device and driver inspectors share System's
General, Hardware check, Logs and Drivers tab sheet. They have no separate
home or Start launchers. The Power action opens the shell's existing shutdown
confirmation. Logs reads `/system/logs/BOOT.LOG` in bounded 8 KiB pages, with Previous, Next
and Refresh controls and a read-only text area for scrolling and copying.

Display retains an entry snapshot through every Apply, including successive
confirmed mode previews. Cancel restores the entry display keys and mode while
preserving independently edited settings. A pending save defers close or
navigation. A failed restore leaves the dialog open for retry. OK closes after
durable completion. The appearance selector changes the theme's existing
palette and UI font roles; the installed-font list remains a read-only preview.
The shell supplies its existing engine snapshot to Control Panel through the
read-only graphics query. Benchmark authority remains unchanged. The Power
confirmation is request 7 of the existing settings message, with all value
fields zero, declared in the shared private settings catalog.

Panel size uses `/system/settings/control.size`; bounds are checked before
opening, and successful size writes replace a small temporary file. Panel,
Start and tray labels/icons come from `apps/desktop/settings_catalog.h`.
Line up snaps icons into distinct available grid cells, while Arrange resets
saved positions. The background menu contains Arrange icons, Line up, Refresh,
New (Folder, Text file), Paste, a separator and Properties, in that order.

Host review copies for every applet, category and Display tab are under
`build/host/f2-75/`. Their raw-frame SHA-256 pins are in
`tests/host/desktop/control-goldens.json`. Separate render names distinguish
fixtures with different engine snapshots. The implementation report records
commands, results and the remaining integrated-image and physical checks.


The kernel's `bootlog.h` fixes the current log at `C:/SYSTEM/LOGS/BOOT.LOG`
and limits each file to 320 KiB. This exceeds CiukNote's 61,440-byte document
limit, so the System applet reads the file itself rather than launching an
editor. The [positioned-read specification](https://sourceware.org/glibc/manual/latest/html_node/I_002fO-Primitives.html)
was checked against the SDK's `pread`: keep one read-only descriptor for each
bounded snapshot, retry interruptions and handle short reads, truncation and
missing files. Pages follow file byte order, including the kernel's protected
prefix and circular suffix. The view never writes log data.


A completed Control Panel preview must not race a second settings writer.
Private settings request 8 uses the existing bounded mode-id payload and
Control Panel authorization, but classifies its completed frame as a preview.
It leaves the desktop's saved mode preference unchanged and skips the ordinary
automatic mode save. Request 3 retains its existing durable-frame behaviour.
Control Panel persists only after confirmation. Cancel first completes its
settings rollback and queues reload, then requests the original mode through
the same preview path. Host checks distinguish both frame classes and verify
that a pending durable rollback cannot start a mode restore early.


The display regression harness also exposed a maximized-window relayout using
its old origin when applying the new screen limits. Reset that origin to the
new work area before sizing; the host test checks the 1024 by 768 to 640 by
480 and return path, alongside preview and durable frame classification.

## Control Panel Win98 layout (2026-10-24, visual audit group B)

Research: the Windows 98 shell shows Control Panel as a folder window of
large-icon applets with an "N object(s)" status bar, value sliders are
trackbar controls (common-controls `TRACKBAR_CLASS`: channel, thumb, optional
tick marks; Microsoft Learn "About Trackbar Controls"), and property sheets
carry their tabs directly on the dialog (`PROPSHEETPAGE`, Microsoft Learn
"About Property Sheets") with OK, Cancel and Apply bottom-right.
Decisions, all in `apps/control/app.c`:

- Every horizontal value slider (pointer speed, double-click, trails, wheel,
  keyboard repeat and blink, master and UI volume, CD volume, power alarms,
  accessibility, RGB, Appearance size and points, font DPI) is a `DU_TRACKBAR`
  built by `slider()` (range `0..total-1`, ticks on small ranges, PgUp/PgDn page
  and drag from the toolkit). Sliders are now Tab stops.
- The home is a `DU_ICONS` list of 32 px Tango applet icons beside the category
  tree, a `DU_STATUS` bar "N object(s)" and no Close button (the window has its
  close box).
- A single-page applet no longer wears a one-tab strip repeating the window
  title (`flatten_page()` hides it and lifts the body). Taskbar & Start Menu uses
  the page tab control for its two tabs instead of a second one.
- `CP_HEIGHT` is 436: the 356 px tab body ends 10 px above a hint strip and the
  OK/Cancel/Apply row keeps a 10 px bottom margin, so the Display hint no longer
  sits on the buttons.
- The keyboard layout note that was drawn over the Multimedia page moved to
  the Keyboard page.
