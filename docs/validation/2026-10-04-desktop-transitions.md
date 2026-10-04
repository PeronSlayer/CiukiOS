# Graphical display transitions (0.8.3)

## Sources and decision, before implementation

The [VESA VBE 3.0 specification](https://www.cs.utexas.edu/~dahlin/Classes/UGOS/reading/vbe3.pdf)
defines mode selection through function 4F02h. Neither a DOS command nor an
intermediate text mode is required. The previous Display module invoked
`VGASETUP DESKTOP`, which suspended applications and selected text mode on
the way out of the shell. The browser had the same problem with HTGET.

Display changes are deferred until the module callback returns, handled
on the shell stack, and rebuild only the graphical surfaces. Applications
remain loaded. Resource teardown is separated from selection of text mode.
A graphical confirmation has a 12-second deadline; only acceptance saves the
four-byte Mxxx profile, using a temporary file and rollback rename. Rejection,
closure or expiry restores the previous mode. Existing DOS-window checks stay.
The browser uses its own resident network API, documented separately.

The remaining trusted console helpers (DHCP, hardware inventory and time-zone
lookup) use ordinary synchronous DOS EXEC while the graphical surface stays
owned by the desktop. A small transient wrapper redirects DOS standard-output
calls, BIOS teletype and INT 29h output to COM1 and restores its vectors on return.
It suppresses BIOS console mode, cursor, page, scroll, text and palette writes
while the helper executes. This follows the EXEC and vector lifetime
contract in Microsoft's [MS-DOS Encyclopedia, System Calls](https://www.pcjs.org/documents/books/mspl13/msdos/encyclopedia/section5/).
These utility calls may pause their owning UI until they return; browser
network requests instead advance cooperatively in its event loop.
The fast-console interface is documented in Microsoft's
[DOS 2.0 device-driver source documentation](https://github.com/microsoft/MS-DOS/blob/main/v2.0/source/DEVDRIV.txt);
the suppressed BIOS operations follow the
[Phoenix IBM PC/XT/AT BIOS reference](https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/bios/System_BIOS_for_IBM_PC_XT_AT_Computers_and_Compatibles_198908.pdf).

The [VirtIO 1.2 GPU specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
and [QEMU GPU implementation](https://raw.githubusercontent.com/qemu/qemu/master/hw/display/virtio-gpu.c)
distinguish transfer/flush completion from physical display presentation.
Frame-cadence measurements must preserve that distinction; command totals
or average game-loop rates alone do not establish absence of stuttering.

## Presentation audit

The bounded pre-change new-game run measured 34.78 game-loop starts/s, but
about 68 GPU transfer/flush batches/s, with single observed completion gaps
up to 103.9 ms. HMP sampling has approximately 3 ms resolution and can miss
updates; these are software pipeline timestamps, not physical monitor FPS.
Evidence: `build/tests/release-0.8.3-2026-10-04/baseline`.

The previous driver uploaded the entire viewport whenever damage is reported or the
compositor changes page. Even an identical page or a small pointer change
therefore transfers several MiB. QEMU's `virtio_gpu_transfer_to_host_2d`
supports a bounded rectangle with a backing offset and full-resource stride.
The implementation compares the completed RAM viewport with its retained
staging image, skip identical submissions, and upload only the bounding box
of actual pixel changes. Staging is never modified while a fence is pending.
First presentation and recovery remain full transfers. This is independent
of the scheduler quantum and preserves in-place VGA drawing.

The first integrated run isolated another periodic source: during rotation,
capture/submission gaps recur near the 18-BIOS-tick interval used by the desktop
meters. `desktop.c:poll` queues a top-bar damage rectangle but returns `1`;
`app_poll` expands that into damage for its owner, the entire desktop. Return
`3`, the existing precise-damage contract already used by DOSVM, and compute
the meter rectangle with the same width rule as `top_paint`. This removes a
full-scene repaint every approximately 989 ms without lowering update frequency
or changing scheduling. The retained-source/rectangle transfer semantics above
remain the presentation contract.

## Final integrated validation

The [full runtime gate](2026-10-04-release-runtime/README.md) passes: Doom new game,
rotation and normal exit; HTTP 200 and reload; Files wheel down/back; graphical
32-bit preview timeout/rollback, 16-bit banked preview/rollback, accepted mode and
About startup preference across reboot. The 16-bit gate exposed a real overrun:
the compositor read a WORD scratch capacity as DWORD, inflating its row count.
The [fix and captured fault](2026-10-04-vbe-bank-boundary.md) are recorded separately.
The mode profile transaction also now tests DOS errors as negative returns;
successful close/rename calls need not return zero in AX.

At matched 800×600, submission gaps over 42 ms fell from six to one and two in the
corrected 9.9-second samples. The final 95th percentile was 30.87 ms, maximum
67.73 ms; game-loop rate was 34.78/s. There was no host CPU-quota throttling.
Residual spikes remain. GPU completion and software timestamps are not monitor
presentation, and the earlier 1280×768 baseline is not a matched comparison.
The first wheel test used QEMU HMP's upward direction while already at the top;
the corrected sign passes a rendered down/back comparison. These earlier failures
are retained as failed attempts, not counted as passing gates.
