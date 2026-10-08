# T23 DOS-window repaint and Savage latency research

The new physical report separates two problems. A DOS guest that will not
start needs diagnosis in the DOSVM execution path. A guest that starts but is
replaced by wallpaper when a menu or another window repaints points to damage
recomposition, expose handling, or window order. The graphics engine cannot
repair a missing window repaint.

## Repaint and occlusion

The X.Org sample-server specification is a useful primary reference for the
same underlying rule: when a window region becomes exposed, the server sends
an expose notification for the minimal region that needs repainting; the
server can retain hidden pixels in backing store, but that is an optional
optimization ([Xserver specification, “Windows”](https://www.x.org/releases/X11R7.6/doc/xorg-server/Xserver-spec.pdf)).
CiukiOS has its own compositor, so this is a design analogy, not an X11
compatibility requirement.

In the current path, `DOSVM.APP` presents the live guest framebuffer in
`band_present()` using `CVP_NO_DAMAGE`, so an exposed DOS client should be
re-rendered even if the guest has no new dirty rows. Its poll reports guest
damage in 64-row bands and requests queued damage only. The compositor paints
windows in `ui_window_order`; while composing it may skip a lower module only
when a later window fully covers that band. Therefore the reported wallpaper
replacement is evidence to trace the exposed band and z-order decisions, not
evidence that the guest lost its screen contents.

For the next physical reproduction, record the affected client rectangle,
`ui_window_order`, `ui_comp_dirty`/damage bounds, the covered-window decision,
the DOSVM damage mask, and whether `band_present()` succeeds. Compare pixels
after opening and dismissing the menu, and after covering then raising the DOS
window with another window. The repair should invalidate the newly exposed
client region and repaint the DOS source there while preserving normal
top-to-bottom stacking. Do not let a desktop wallpaper repaint substitute for
that client paint. Existing QEMU window tests already exercise cover/raise;
add the same scenario for repaint caused by the desktop menu if that trigger
matches the physical failure.

## BCI waits and host stalls

The primary X.Org Savage 2.4.1 driver documentation identifies a chipset bug:
reading the engine status register under heavy load, including scrolling or
window dragging, can bus-lock some Savage systems. It provides `ShadowStatus`
as an alternate, slightly more expensive status-read method and enables it by
default for DRI ([Savage driver manual](https://manpages.debian.org/unstable/xserver-xorg-video-savage/savage.4.en.html#ShadowStatus)).
That makes status polling a concrete candidate for the physical lag spikes.

The fresh diskseq59 physical log rules out that candidate for this boot:
the native backend is zero, initialization stopped at stage1, and command,
fill/copy/triangle and timeout counters are all zero. The desktop therefore
uses CPU uploads. New preflight diagnostics preserve the failed input/BAR
tuple so a later physical boot can identify that rejection without relaxing
the hardware admission checks. Removing the duplicate wallpaper paint from
the overlay pass also removes unnecessary CPU upload work from popup repaint.

The current native backend polls alternate status at MMIO `0x48c60` in
`wait_idle()` and `fifo()`, each for up to 65,536 tight reads. Initialization
sets PIO mode and disables the shadow-status update path; successful fill,
copy and triangle operations synchronously wait for engine idle before and
after submission. CPU framebuffer reads and writes also call
`gpu_savage_cpu_begin()` and wait for idle first. These waits are required for
ordering: Linux's upstream Savage framebuffer driver likewise defines
chip-specific FIFO/idle waits and calls them before changing modes
([Linux v6.2 Savage BCI](https://github.com/torvalds/linux/blob/v6.2/drivers/gpu/drm/savage/savage_bci.c),
[Linux Savage framebuffer driver](https://github.com/torvalds/linux/blob/v6.2/drivers/video/fbdev/savage/savagefb_driver.c)).
Removing them or allowing CPU fallback after an uncertain timeout would risk
corrupting scanout and private scratch surfaces.

Measure these costs before changing policy: add per-operation elapsed/max
wait telemetry, split status-poll time from packet submission and CPU row
copy time, and correlate spikes with idle/FIFO timeout and user-operation
counters in DISPLAY_INFO. Test sustained DOS animation with no window changes,
then menu exposure/repaint with the engine both idle and active. The upstream
`ShadowStatus` note justifies a focused shadow-status experiment on the T23,
but its CRTC/MMIO setup must first be checked against the pinned X.Org source
and saved-state restore path; do not simply turn on the DMA/status register.
Any batching or reduced polling must retain a bounded wait and the existing
CPU/GPU serialization contract.

## Evidence boundary

The DOS launch failure and repaint symptom are not yet tied to one cause.
Capture DOSVM fork/exit status separately from paint/compositor state. A
working VBE mode and full-colour desktop prove scanout selection, not correct
guest-window damage handling or acceptable BCI latency.

## Game render readiness

The original id Software startup source prints `ST_Init` before starting the
level or title sequence and entering `D_DoomLoop`; `I_InitGraphics` is inside
that loop ([upstream d_main.c](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/d_main.c)).
This Linux source release establishes phase order, not the implementation of
the separate DOS audio library. The old runtime gate took its baseline only
two seconds after that marker. A 128 MiB control which ultimately passed had
a black baseline, while a 512 MiB music-enabled control failed with a black
baseline, active timers/audio and 401 pending PIT expiries. Music alone and
512 MiB alone pass. The gate now waits a bounded interval for visible status
bar graphics before measuring response to input; movement, menu navigation,
normal quit and a second launch remain required. Failure captures are capped
at16 MiB to include the observed12 MiB client page tables/device counters.
The512 MiB music-enabled retry completes two launches, movement, menu quit
and desktop return; its first HUD appears2.684 seconds after ST_Init and
the second3.272 seconds after it. A stronger status-bar occupancy threshold
also excludes a partly revealed screen wipe from the gameplay baseline.
No production timer or memory-limit change follows from a premature sample.

The final gate also exercises the physical launch context: Files changes to
`C:\\DESKTOP\\TestGames` and double-clicks the bare DOOM.COM shortcut, without
Run or warp/sound arguments. It waits for title graphics, observes the main,
episode and skill selections from the shipped WAD's skull sprites, then starts
the game through its real menus. Their positions and initial choices are
checked against [id Software's m_menu.c](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/m_menu.c).

## Finished DOS windows

The physical log records guests exiting with code0000, but the native
DOSVM paint function replaced every non-live guest with an empty black
rectangle. That hides the difference between a running black VGA frame and
a terminated program. The module will display a finished/error message and
the session return code after termination, preserving the existing manual
window close. The DOS wrapper still propagates the child's return value;
[Microsoft's MS-DOS Encyclopedia, function4Dh](https://www.pcjs.org/documents/books/mspl13/msdos/encyclopedia/section5/)
documents the child's return code separately from its termination method.
CVSESSION status is a separate local API, so its state/generation values
are logged separately and a failed status query cannot read uninitialized
stack outputs or claim code0000.

## Default wallpaper framing

Ciuk1 is selected by default with `WP_FIT = 1`: preserve the whole image and
its aspect ratio, using solid desktop-color bars when the screen work area has
a different aspect ratio. This follows the documented Fit behavior in
[Microsoft's desktop background guidance](https://support.microsoft.com/en-us/windows/experience/personalization/change-the-desktop-background-in-windows): Fit keeps the image's aspect ratio and may show side or top/bottom bars; Fill covers the screen by cropping. Since the owner asked to see the entire photograph, Fit is the appropriate default while Fill remains a supported explicit choice. Default-photo gates independently check a contained draw rectangle, source pixels across the image, and uniform samples in the bars.

The same default applies when `WALL.CFG` is absent or contains only its
legacy one-byte wallpaper index. A valid two-byte profile still honors its
explicit style, including style 0 (Fill); malformed profiles do not turn a
missing style into an implicit crop.
