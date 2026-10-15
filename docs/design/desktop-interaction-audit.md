# Desktop interaction audit

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
