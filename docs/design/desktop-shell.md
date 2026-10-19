# Desktop shell bars and protruding Start orb (f2-82)

## Desktop transfers, close acknowledgement and Files refresh (f2-85)

Primary sources rechecked on 2026-10-19 before code changes:
[POSIX read](https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html),
[write](https://pubs.opengroup.org/onlinepubs/9799919799/functions/write.html),
[condition waits](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_clockwait.html),
[Microsoft progress guidance](https://learn.microsoft.com/en-us/windows/win32/uxguide/progress-bars),
[EWMH close requests](https://specifications.freedesktop.org/wm/latest-single/),
and [QEMU throttling](https://www.qemu.org/docs/master/system/invocation.html).
The Open Group direct URLs returned HTTP 403; its indexed specification text
was available. Preserve short-I/O positions, 1 ms EINTR back-off, predicate
waits, real byte progress and cancellation. QEMU evidence remains with the lead.

Repository comparison found a second, unconditional full refresh in
`apps/files/files.c:idle`, in addition to the media timer. It synchronously
re-listed, rebuilt the tree and invalidated the full client every second,
even without a media change. Remove that path; one worker listing per second
covers external writes. Apply metadata-only updates in place, preserving
selection, anchor, scrolling, entries and tree allocations; invalidate only
visible changed rows and the selected-size status when needed. Structural
changes still replace the listing. Idle browser polling must not paint.

The desktop uses the same transfer mailbox and dialog formatter as Files.
Clipboard reads, trash preparation, 64 KiB copy steps, cancellation cleanup,
clipboard consumption and the final folder refresh all run on the worker.
Desktop Delete keeps the existing move-to-Trash semantics and collision-safe
names, displaying the shared Deleting dialog. The shell hosts the toolkit
body in an internal window with its ordinary title frame and input routing.

Protocol: v2 client-to-shell `DESK_CLOSE_ACK` has exactly eight bytes, no
attachments, with a uint32 disposition: 1 = closing later, 0 = close declined.
Only the authenticated sender's closing window can acknowledge. Its first
later acknowledgement must arrive before the original two-second deadline and
grants a total of 30 seconds from the original request; duplicates never extend it. A declined
request received before the effective deadline clears it and permits a fresh
close request. Shutdown retains its own deadline. Files acknowledges while its transfer/confirmation
is pending and declines when the user keeps the operation open. Unacknowledged
and expired requests retain the existing forced termination behavior.

Host acceptance (2026-10-19): a separate process grows one file every 100 ms
among 1,024 entries. Four worker listings in four seconds, zero UI listings,
2.870% UI CPU and 1.297 ms maximum input turn; the former idle path measured
5.390% and re-listed twice per second. The harness checks stable entries/rows,
selection/anchor, clipped/full render equivalence, no paint for unchanged
polls, external clipboard updates and structural add/remove. These are host
measurements, not physical T23 qualification. Eight shell dialog frames are
pinned in `tests/host/desktop/desktop-transfers-goldens.json`.

## Files worker transfers (f2-84)

Research checked before implementation on 2026-10-19:

- [POSIX read](https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html)
  and [write](https://pubs.opengroup.org/onlinepubs/9799919799/functions/write.html)
  permit short transfers and EINTR before transferring data. Keep short-I/O
  positions, and sleep at least 1 ms before retrying interrupted transfer I/O.
- [POSIX condition waits](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_wait.html)
  atomically release the mutex while sleeping and require a predicate loop.
  The transfer worker owns its job; the UI only reads a locked snapshot and
  sends answers/cancellation. No VFS operation holds the mailbox mutex.
- [Microsoft progress guidance](https://learn.microsoft.com/en-us/windows/win32/uxguide/progress-bars)
  recommends real determinate progress, meaningful file details, cancellation,
  and delaying remaining-time estimates until enough samples exist. Count
  bytes/items on the worker before mutation, poll at 100 ms, show elapsed time,
  and estimate rate/remaining time after two seconds. Delete uses item counts.
- [QEMU request-rate throttling](https://www.qemu.org/docs/master/system/invocation.html)
  supplies a host-side slow-device mechanism. The existing runner exposes
  `throttle_iops` on a USB fixture; qualification remains the lead's QEMU work.

Repository comparison: Files ran 24 synchronous 16 KiB steps per 2 ms UI
turn, reported only selected roots, and redrew its whole client on every
progress update. Use a dedicated persistent pthread, 64 KiB chunks (128
512-byte sectors, matching the directive's f1-81 bulk-command number), a
cheap metadata listing pass without ELF sniffing, and clipped dialog-only
repaints. Preserve the temporary-file/fsync/atomic-rename publication path.
The f2-85 amendment above supersedes the former desktop-only 16 KiB
default: both hosts now use the extracted worker and 64 KiB steps.

The f2-84 implementation could not suspend the shell's two-second close
deadline. The f2-85 acknowledgement above addresses that limitation; runtime
qualification remains the lead's responsibility.

## Bar surface quality (f2-83)

Primary-source research checked on 2026-10-19 before implementation:

- Microsoft's Windows 7 [taskbar guidance](https://learn.microsoft.com/en-us/windows/win32/uxguide/winenv-taskbar)
  separates window identification, activation and notification controls;
  its [colour guidance](https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-color)
  favours restrained colour and checking reduced-colour displays. The
  [theme format documentation](https://learn.microsoft.com/en-us/windows/win32/controls/themesfileformat-overview)
  distinguishes Aero and Windows 7 Basic colour roles (face, light, shadow
  and dark shadow). Use these as inspiration for opaque depth and distinct
  button states, without borrowed art.
- Apple's [toolbar HIG](https://developer.apple.com/design/human-interface-guidelines/toolbars)
  recommends deliberate controls and consistent grouping. Keep the existing
  top status group, bottom window group and every geometry/hit-test box.
- GNOME's [UI styling HIG](https://developer.gnome.org/hig/guidelines/ui-styling.html)
  calls for theme variables, accessibility testing and state cues beyond
  colour. The [WCAG contrast definition](https://www.w3.org/WAI/WCAG21/Understanding/contrast-minimum)
  supplies the measurable 4.5:1 label target; test actual shaded faces in
  both themes and after RGB565 conversion.
- [FreeType glyph retrieval](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html)
  documents normal hinting and 8-bit coverage. The existing pinned Inter
  12/13 px atlases already use them (`sdk/freetype/ui_raster.c`); retain the
  atlases and blend a soft one-pixel text shadow on the dark top bar.
- [OpenVG 1.1, section 6](https://registry.khronos.org/OpenVG/specs/openvg-1.1.pdf)
  describes coverage antialiasing. Use deterministic integer 4x4 sample
  coverage for circles, their inset rims and three-pixel rounded tasks.
  Its image-copy discussion also recommends dithering when reducing colour
  depth, with stable results for successive animation frames (pp. 148–149).

Repository comparison: bars were flat fills, task labels were hard-clipped,
and circle edges had only four samples. The portrait already has a pinned
source and deterministic area scaler; native Tango tray sprites are 16 px.
Decision: reuse those assets and metrics. Both bars use a two-stop vertical
surface with a one-pixel light top edge, dark bottom edge and subtle inner
shadow; steps in light/shade use roughly 8–12 percent mixes. Cool slate-blue
and neutral platinum retain their existing theme roles. Menus and notification
faces use the same surface treatment. Focused tasks have a darker face and
two-pixel accent, hover has a brighter bevel, minimized tasks a lighter face
with upright text. Overlong labels end with the existing U+2026 glyph.
The normal-boot observer's default taskbar colour literal follows the new
Ciuki Blue base, with samples spanning each bar's new gradient (as already
done for the orb). Its geometry, tolerance and occupied-fraction thresholds
are unchanged. The companion host source guard checks the shared surface
painter rather than the former flat fills.

RGB565 surfaces use a screen-anchored ordered 4x4 Bayer threshold before the
existing scanout truncation (`fbdev.c`, `pack_565`); XRGB8888 keeps its smooth full-colour ramp.
The threshold chooses neighbouring channel levels, with no random state or
frame-dependent phase. Tests check adjacent body rows (excluding the explicitly
contrasting edge/inner-shadow lines): at most one 8-bit step in 32 bpp and
one native channel step between adjacent 4x4 band means in RGB565. They also
check that dithering actually mixes neighbouring levels within a row, and
that partial repaint matches a whole repaint. All painting remains integer-only.

F2-83 deliberately re-records 122 existing shell/compositor, bar-preview,
interaction, notification and boot-frame references, and adds eight bar
references (both themes, both sizes, XRGB8888/RGB565). App-only controls and
dialog-only references remain unchanged apart from the three theme bar
previews. Review PNGs and 2x inspection crops live in
`build/codex/f2-83-frames/`; host captures are not guest evidence.

Research checked before implementation, 2026-10-18:

- The [EWMH specification](https://specifications.freedesktop.org/wm/latest-single/)
  distinguishes the work area reserved by panels, maximization within that
  area, full-screen geometry, and task buttons that activate or minimize
  windows (sections 2.1.1, 2.7, 3.9 and 5.7). CiukiOS already reserves
  `DESK_BAR` at both edges. Keep those dimensions and existing DOS/full-screen
  transitions; move shell controls without changing the window protocol.
- Microsoft's [taskbar documentation](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar)
  describes the launcher, window buttons and notification area as distinct
  shell functions. The owner's layout places launcher and system status above
  the work area and window buttons alone below it. Both retained themes use
  left-aligned tasks. Window captions are never repeated in the top bar.
- The [Desktop Notifications specification](https://specifications.freedesktop.org/notification/latest/protocol.html)
  separates replacement, expiry, dismissal and activation. Preserve CiukiOS's
  bounded notification slot, coalescing and actions while moving its anchor
  below the top bar. This is a behavior reference, not D-Bus implementation.

The implementation previously drew tray icons and time in the bottom bar,
date and the focused title in the top bar, and limited tasks by the tray's
width. Shared geometry now keeps tray/time/date at the top right, an explicit
notification anchor between tray icons and clock, and tasks across the bottom.
Tray popups drop down; task tooltips rise from their bottom buttons. Damage
and overlay exclusion must cover both old and new popup positions.

F2-82 research rechecked the three primary sources above on 2026-10-18.
EWMH sections 2.1.1, 3.9 and 5.7 distinguish reserved work area from window
stacking; Microsoft documents the launcher separately from application
windows. Decision: retain the 32-pixel work-area boundary and paint the orb
as shell chrome above windows. Notifications keep their existing expiry,
replacement and action semantics. These references inform behavior; the
owner's Vista-like geometry is drawn locally without Microsoft artwork.

Amendment 2 research rechecked the [EWMH specification](https://specifications.freedesktop.org/wm/latest-single/)
and [PNG specification](https://www.w3.org/TR/png/) on 2026-10-18 against
the existing shared geometry and pinned converter. Decision: resize through
the generated geometry and existing deterministic portrait scaler; preserve
the reserved work area, stacking and approved source pixels.

The signature control is a 38-pixel orb at (6,0), extending 6 pixels below
the 32-pixel bar. A concentric 42-pixel concave well is clipped to the bar.
Integer radial shading, a soft elliptical highlight, a thin dark outline
and a one-pixel shadow surround the unchanged, deterministic 24-pixel area
average of the approved portrait. Hover brightens the gloss; menu-open
inverts the gradient and shifts the portrait one pixel. The existing scaler
already supports 24 pixels; no additional bitmap is required.

`convert_portrait.py --geometry` generates `start-orb.h` and `start-orb.json`
from one geometry definition. The build validates both; the desktop uses
the header and runner observers use the JSON for the orb and portrait boxes.
The observer compares every fully opaque portrait cell against the pinned
converter, independent of theme/gradient. Circular hit-testing includes the
protrusion; the menu still anchors at (6,32). Orb damage includes its shadow,
and client-only, scroll and window-copy fast paths exclude that area just
as they exclude notification chrome. DOS and full-screen geometry stays
unchanged.

Only Ciuki Blue (default) and Platinum remain selectable. The removed saved
theme maps explicitly to Ciuki Blue in both settings readers and logs once
per process, including when it follows another valid theme line.

Intentional golden changes: shell bars, orb, task widths, tray popup placement,
notification placement and Control Panel's two-theme list. Classic theme
frames are removed. Frames of application controls without shell chrome
remain unchanged except for theme-list content. Host captures cover 1024x768
and 800x600; runtime QEMU evidence belongs to lead integration.

F2-82 intentionally re-records shell reference, interaction, compositor,
transient/notification and boot-frame goldens for the larger orb. App-only
and dialog-only frames retain their existing hashes. Twenty additional orb
goldens cover idle, hover, pressed, maximized-window and notification states
in both themes at both resolutions. The dedicated replay checks circular
hits above and below the bar, menu/keyboard activation, notification input
priority on a narrow screen, shadow damage, copy exclusions and exact
incremental/full-frame parity. Host qualification also retains the
no-allocation-after-first-frame and DOS transition checks.

## F5-09 desktop protocol v3 (2026-10-19)

F5-09B lead amendment (2026-10-19): GRANT is the atomic mode transaction
under the desktop's call-74 authority, including composition suspension,
cursor hide, mode confirmation and rollback. A separate call 74 before GRANT
would leave `awaiting_frame` and is not issued. REVOKE uses the exact returned
PID/token/framebuffer-generation tuple synchronously before repaint; the same
mode is selected again on reactivation. A pending restored desktop frame is
completed before regrant; a failed repaint defers regrant to heartbeat recovery.
The independent desktop watchdog
observes restoration through the first complete desktop repaint, reports a
2 s deadline violation, and never enables composition over a live lease.
Kernel timeout/death revocation is reconciled only after a successful display
grant query proves desktop ownership has resumed; the client receives a loss
event (ETIMEDOUT after the 10 s first-frame deadline, EIO for earlier external
loss). Failed requests retain the ordinary signed v3 RESULT.

LEASE_EVENT now sends a 72-byte payload: the original state, serial, source
width/height/depth; then token, framebuffer generation, native mode id, scale,
source width/height/pitch/size/format, native width/height/depth, signed errno.
The bridge reads legacy 20-byte events as well; v1/v2.1 messages are unchanged.
The tuple grants only FRAME/PALETTE/GAMMA, never a display descriptor or mapping.

The explicit fw_cfg selector `f2:crash-isolation run=<8hex> server=desktop-v3`
runs the existing isolation battery and then GATE_V3. The authored raw-ABI PE
peer is `/System/Win32/Fixtures/desktop-v3.exe`; the native desktop monitor
reports eight ordered observations through call 3. Only the controller-selected
desktop with a live Win32 child may update the kernel observer, which emits
the trusted CIUKI_TEST records. The two Alt+Tab batches revoke and regrant;
crash/restart phases inspect and cancel their dialogs (f5-07B owns restart
execution). The shortcut launches the native Control Panel Add/Remove page;
the monitor seeds/lists/removes its Uninstall key via w32srv. The case remains
not_run until lead validation. Host raw-ABI replay is host evidence only.


Research before implementation: Microsoft's [Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links)
requires retaining target, arguments, working directory, icon/index and show
state; [Uninstall registry key](https://learn.microsoft.com/en-us/windows/win32/msi/uninstall-registry-key)
identifies DisplayName and UninstallString. [SetCapture](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcapture)
limits capture to a foreground window, while id's original
[Quake II input](https://raw.githubusercontent.com/id-Software/Quake-2/master/win32/in_win.c)
uses capture, clipping and repeated recentering. Preserve raw input deltas before
pointer acceleration, clipping or coordinate conversion. Microsoft's
[SetCooperativeLevel](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdraw7-setcooperativelevel)
requires foreground exclusive ownership; [ExitWindowsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-exitwindowsex)
and [EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record)
supply restart flags and exception code/address semantics. These modern API pages
are behavioral references, not evidence of executing Windows 98 binaries.

Repository comparison: v2.1 has one window per channel, XRGB8888 surfaces and
ASCII captions. The kernel already supplies raw motion counts. The checked-out
ABI has neither f5-08 delegated presenter calls nor f5-07A Win32 registry RPC.
V3 keeps native clients unchanged, uses the existing XRGB8888 surface contract,
and exposes explicit failing integration callbacks for those pending services.
It does not invent syscall numbers or read another service's hive files.

V3 is opt-in (header major 3, minor/reserved zero). The 16-byte envelope is
`u8 major, opcode, 0, 0; u32 request, window, generation`, little endian.
The transport has a PID authenticated by channel sender metadata. Its zero
window/generation addresses HELLO only. Every visible window has its own
nonzero client id and strictly increasing generation. Owners belong to that
same transport; their lifetime and activation never confer authority over a
native PID. Surface replacement is atomic and acknowledged before releasing
its old mapping. Backpressure never blocks the shell; exhausting the v3 reply
queue disconnects the transport and all its windows rather than losing a
lifecycle acknowledgement. See protocol.h for payloads and bounds.

Shell links use the documented CiukiOS binary `CIUKLNK3` format, with counted
UTF-8 fields, bounded at 4096 bytes. It retains all six ShellLink properties;
f5-15's IShellLink writer must use this format. Native shell launch records
carry separate target, arguments and working directory: never interpolate them
into a shell command or truncate them to v2's single 160-byte argument.
A stale/invalid record fails closed. Registry enumeration and launch use an
RPC adapter, not a second hive writer. Runtime qualification and connecting
the parallel f5-07A/f5-08 adapters remain lead integration work.

The f2-84/f2-85 worker and close-deferral reports were also checked against
this implementation. Shortcut discovery, stale-link reads and launch RPCs run
on a persistent sleeping worker. The UI uses try-lock snapshots and a bounded
single launch mailbox; it never waits for that worker's disk/RPC operations.
There are at most 64 cached links (32 initial Programs slots, remaining slots
for Desktop), 128 scanned directory entries per root and three directory levels
under Programs. The Start menu shows eight shortcuts plus the native Programs
folder view, which provides keyboard scrolling for the complete cached list.
Labels are sorted; activation reads the link again on the worker. No temporary
file, command interpreter or v2 single-argument conversion is used.

`apps/libdeskui/v3.h` is the f5-10/f5-19 user-mode handoff. `du3_request` validates
outgoing payloads and returns a request token; `du3_next` validates server events.
A PING requires an explicit `du3_ui_progress` after the guest UI makes progress;
a transport thread must not reply for a frozen guest task. Native toolkit
windows still use their existing implementation and wire versions.

V3 wire rules, relative to the 16-byte envelope above:

| Message | Payload | Direction and attachment |
| --- | --- | --- |
| HELLO | empty; request/window/generation all zero | client; no fd |
| CREATE_WINDOW | style, owner id/generation, physical x/y, logical w/h, scale, title byte count + UTF-8 | client; one XRGB8888 surface fd |
| DAMAGE | logical source x/y/w/h | client; no fd |
| RESIZE (64) | logical w/h, configure serial | client; one replacement surface fd |
| PLACE (65) | physical x/y, logical w/h, scale, show (normal/minimized/hidden), style | client; no fd |
| SET_TITLE | count + UTF-8 | client; no fd |
| SET_CURSOR | shape, custom id (0 builtin, 1–4 cached), signed ShowCursor count | client; no fd |
| CURSOR_BITMAP (66) | id, hotspot x/y | client; one 32×32 surface, ARGB in the source pixels |
| POINTER (67) | capture/clip/relative/hidden flags, logical client clip x/y/w/h | client; no fd |
| WARP (68) | signed logical client x/y | focused process only; no synthetic raw motion |
| LEASE (69) | request/release, logical w/h, depth | focused window; no fd |
| RESULT (70) | request opcode, signed negative errno or zero | server; envelope request matches client token |
| ACTIVATE (71) | keyboard focus, application activation | server event, or client activation request |
| LEASE_EVENT (72) | state, lease generation, logical w/h/depth | server; 0 toggle request, 1 active, 3 revoked |
| RELATIVE_EVENT (73) | original 40-byte input record | server; raw kernel counts before acceleration/clipping |
| CRASH (74) | exception code, address, module byte count + UTF-8 | client; authenticated sender PID only |
| RESTART (75) | ExitWindowsEx flags | client; confirmation before service callback |
| POINTER_STATE (76) | current pointer flags (zero releases capture/clip) | server |
| CONFIGURE | logical w/h, scale, serial, physical client x/y, show, style | server |
| KEY/MOTION/BUTTON | 40-byte input record | server; ordinary motion uses logical client coordinates |
| PING/PONG | UI-progress token | either direction |
| CLOSE / CLOSE_REQUEST / CLOSED | empty | client / server / server |

All scalars are 32-bit little-endian, including signed two's-complement fields;
input timestamps keep their existing low/high halves. Reserved header bytes,
trailing bytes, descriptor slots and message reserved fields must be zero.
Dimensions are 1–2048 logical pixels; scale is 1–3; position is bounded to
±8192 physical pixels. Native surface allocation/stride limits remain in force.
Window ids are client-local 1–16, with nonzero generations increasing without
wrap or reuse within a transport. The existing 16 desktop slots include transport
anchors, so a single v3 transport can own at most 15 simultaneous windows.
This bounded capacity is not a claim of unrestricted Win32 window counts.

Creation queues CONFIGURE, RESULT and activation in that order. PLACE queues a
new CONFIGURE before its RESULT. RESIZE accepts only the latest nonzero serial,
queues RESULT before releasing the old server mapping/fd, and rejects duplicate
or superseded commits with ENOENT. The client keeps its old backing until RESULT.
An owner must already exist on the same transport; the immutable relationship
cannot form a cycle. Topmost windows stay above ordinary windows; owned windows
stay above their owner, and internal system dialogs stay above both groups.
Only modal owned windows redirect activation. Tool/owned/hidden windows do not
get their own task button or Alt+Tab entry. Overlapped windows use the existing
interactive resize path; popups have no frame, shadow or caption hit targets.

The lease callback is a transaction boundary. Mode choice prefers an exact
listed geometry/depth, then an integer multiple at 16/32 bpp, then the smallest
suitable listed mode. Failed acquisition leaves desktop presentation enabled.
Revocation precedes focus loss, menus, modal dialogs, hot keys or disconnect;
failed revocation keeps composition paused and is retried. Reacquisition uses a
new generation and the retained logical mode. The kernel adapter must guarantee
synchronous cancellation and mode restoration, including holder death. The
checked-out ABI cannot supply that adapter yet: no working exclusive scanout,
mode switch or two-second hardware recovery is claimed here.

The native adapter symbols are `ciuki_win32_presenter_lease`,
`ciuki_win32_shell_launch`, `ciuki_win32_shell_restart`,
`ciuki_win32_uninstall_list` and `ciuki_win32_uninstall_run`.
Their weak default implementations return ENOSYS, rather than successful fake
results. f5-07A supplies authorized HKLM/HKCU enumeration and selected-key launch;
f5-07B/f5-15 supply session shutdown, hive commit, WININIT and RunOnce restart
processing. The Control Panel never edits hives or expands UninstallString into
a shell command. The bounded command decoder preserves the argument tail and
requires an explicit absolute executable path; ambiguous unquoted paths fail.

Final compatibility checks used Microsoft's optional `lpWindowName` in
[CreateWindowEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createwindowexw)
and the [surface damage coordinate distinction](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_surface).
V3 accepts empty captions as well as valid UTF-8 captions up to 63 bytes; native
caption constraints remain unchanged. During an outstanding resize, source
DAMAGE is mapped outwards through the retained backing's actual stretch ratio.
The host test compares the incremental result with a complete redraw.

V3 CLOSE_ACK carries one disposition word (0 decline, 1 defer), scoped to its
window. A frame-close request does not start the native process-wide timeout:
closing one top-level window cannot terminate its siblings. Explicit End Task
uses the authenticated host PID and the existing termination authority, and
works independently of guest UI progress. Legacy Task Manager window lists
retain their v2 ASCII schema; only non-ASCII code points are replaced by `?`
in that legacy view, while v3 captions remain intact. Empty captions use
`Windows program` there. Lease release is idempotent, regrant notification
precedes activation, and applying a clip immediately confines the current
pointer without manufacturing a raw motion event.

Native adapter signatures (strong definitions replace the failing defaults):

```c
int ciuki_win32_presenter_lease(uint32_t pid, int acquire, int hardware_mode,
    uint32_t logical_w, uint32_t logical_h, uint32_t bpp, uint32_t generation);
int ciuki_win32_shell_launch(const struct desk_launch_record *record);
int ciuki_win32_shell_restart(uint32_t requesting_pid, uint32_t flags);
int ciuki_win32_uninstall_list(struct desk_uninstall_entry *out, unsigned capacity);
int ciuki_win32_uninstall_run(const char *key);
```

Presenter success is zero; failure is negative errno and must leave the previous
lease/mode intact. For release, hardware_mode is zero and the generation/owner
still identify the revoked lease, including after a channel disconnect.
Launch returns zero or a positive accepted PID on success, negative errno on
failure. Restart and uninstall_run return zero on success, negative errno on
failure. The listing returns a count up to capacity, or negative errno;
keys identify HKLM/HKCU entries, not an executable string supplied by the UI.
The service must re-resolve and authorize the key before starting its command.
Shell launches run on the sleeping worker; the UI sees only queued acceptance
and a later failure dialog. Restart confirmation is UI-only; its adapter owns
the separate service transaction and must not perform a machine reboot.
