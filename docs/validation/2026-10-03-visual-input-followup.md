# Visual input follow-up, 2026-10-03

## Owner confirmation and remaining gameplay complaint

The owner confirms the cache/cursor build substantially improves pointer
responsiveness during Doom and restores a responsive desktop after exit.
Doom itself still feels slow on the same Linux QEMU profile. The subsequent
[engine timing investigation](2026-10-03-doom-frame-cadence.md) measures
33.3 simulation/render-loop iterations per second during rotation, with
irregular intervals, and distinguishes this from desktop input latency and
from stale VGA diagnostic snapshots. Gameplay smoothness remains open.

## Cache/cursor correction and final verification

The follow-up now includes an implemented correction and a completed runtime
check on the rebuilt canonical image. Native framebuffer writes used an
uncached mapping under the firmware's broad UC MTRR, making even successful
native copies occupy long interrupt-disabled intervals. The validated
single-CPU QEMU/Bochs path now owns a WC VRAM range, with synchronized cache
transitions and original-policy restoration on unbind. Cursor backgrounds
belong to their individual display pages; the next cursor is composed before
the flip and only the now-hidden old cursor is erased. Cursor rows are
transferred together. Scheduler quanta were not changed.

The controlled comparison with the same host CPU cap changed gameplay input
consumption from 300–444 ms to 3–71 ms; a canonical-image repetition measured
3–76 ms. Idle and confirmed post-Doom-exit motion stayed near 3 ms. The final
run, allowing a second host CPU for QEMU/test overhead, measured 2.6–4.9 ms
during a real E1M1 new game. All 12 sampled frames contain one intact cursor.
Console DOS and return to the desktop also pass, including framebuffer cache
release and rebind. That check additionally exposed and fixed `app_key`
discarding the original BIOS key when an app declined it, breaking global
shortcut fallback after application focus.

These timings measure monitor-injected PS/2 motion consumed by the guest,
not physical SDL-to-screen latency. See [cache investigation and evidence](2026-10-03-framebuffer-cache.md)
and `build/tests/lag-final-validated-2026-10-03/`. Full image and sanitized
Windows ZIP are current and verified; Windows runtime was not tested.

## Owner retest after the 15:11 build, before the cache/cursor correction

The owner reports gameplay lag, idle-desktop lag and cursor blinking. The
15:11 session manifest matched the then-current session/VMM/presenter sources;
the freshly assembled shell was byte-identical to the image's build artifact.
The earlier compilation checks do not establish a runtime fix.

A bounded KVM run (256 MiB guest, 768 MiB host scope, zero swap, one QEMU,
snapshot disk, 150-second deadline) confirms native framebuffer binding is
active even in banked V86 mode. Idle pointer samples were 45–51 ms, within
the screenshot polling limit; while Doom displayed its menus, samples rose
to 208–618 ms. The initial menu-key sequence did not reach gameplay and the
exit sequence did not close Doom; these samples must not be labelled a new
game or post-exit validation. Evidence: `build/tests/lag-owner-image-2026-10-03/`.

Cursor correction follows [VBE 3.0 display-start/page ownership semantics,
section 4F07](https://courses.cs.washington.edu/courses/cse451/21sp/readings/vbe3.pdf):
prepare a complete hidden page before displaying it. Current code instead
erases the visible cursor during catch-up and again before switching pages.
Use one saved cursor background per page; keep the front cursor visible,
remove its copied pixels from the hidden page using its saved background,
draw the next cursor before the flip, then restore only the now-hidden old
page. Batch all cursor rows in one transfer. This fixes pixel ownership and
does not change scheduler quanta or rely on a retrace delay.

## Owner's run

The 09:44 visual log was read before cleanup. The owner opened
`C:\DESKTOP\TestGames` in Files, opened the volume popup, then double-clicked
`DOOM.COM`. VM 11 became active; DPMIRUN reported a nonzero TSC rate, audio,
and an active session. Doom reached `ST_Init: Init status bar.`. The rest of
the log was desktop paint markers. No key, PS/2 mouse packet, frontend focus,
or game-loop progress was logged, so this log alone cannot locate the reported
input freeze. The optional `CyberMan: Wrong mouse driver - no SWIFT support`
message is not evidence that the ordinary mouse failed.

## Repository behavior and repeatable checks

`src/apps/dosvm.c` sets VM 11 as keyboard focus when its window becomes active;
its `vm_focus` request keeps physical IRQ12 with desktop VM 0. The desktop
relays pointer motion inside the DOS client through `DEV_MOUSE`. The keyboard
instead goes through `jlm_filter` or HDPMI's protected IRQ handler to the
focused VM's device model (`src/vm/session_vmm.inc`,
`src/vm/session_devices.c`). These are distinct paths, so simultaneous failure
at the visible QEMU window also calls for a frontend focus/grab check.

On the owner's full image, a headless QEMU/KVM run with the original
`\DESKTOP\TestGames\DOOM.COM` measured software pointer visibility at a
median 59 ms on the idle desktop and 192 ms with Doom active (four samples;
max 312 ms). The measurement includes repeated QEMU screendumps, so it is an
upper bound with roughly 60 ms polling resolution. Desktop icon clicks stayed
at 97 ms and 95 ms median respectively. Output:
`build/tests/user-doom-baseline-2026-10-03/report.json`.

A second headless run reproduced Files -> volume popup -> `DOOM.COM` using
PS/2 events. Doom reached its title screen and then gameplay. Esc opened its
main menu and another Esc closed it. A RAM snapshot showed VM focus = 1. This
shows that the guest path can work; it does not prove that real keyboard and
mouse events reached QEMU's SDL/X11 window in the owner's visual run. The
existing automated tests mainly inject monitor events and therefore cannot
settle that difference. A CS:EIP sample while the game ran counted 155/250
samples in the protected-mode client, 55/250 in ring 0, and only 15/250 in
the V86 desktop; it does not identify the latency between a physical input
event and the desktop's next slice.

The old `.log*`, `.png`, and `.ppm` files under `build/full` were removed after
the owner's log was read. `build/full/obj` was excluded because it contains
compiler source and artifacts, including bitmap examples. Diagnostic runs in
`build/tests` remain separate.

## Sources and decision

QEMU's [PS/2 implementation](https://github.com/qemu/qemu/blob/master/hw/input/ps2.c)
emits key scan codes and mouse packets through separate controller paths.
QEMU documents its [input-event trace points](https://www.qemu.org/docs/master/devel/tracing.html),
and this installed QEMU 11.1.1 exposes `sdl2_process_key`, `input_event_key_qcode`,
`input_event_rel`, `ps2_put_keycode`, `ps2_keyboard_event`, and
`ps2_mouse_send_packet`. A [QEMU Wayland grab report](https://gitlab.com/qemu-project/qemu/-/issues/3192)
shows that frontend input can fail separately from a guest PS/2 driver; it
does not prove this XWayland run has that defect.

Do not change the scheduler or declare the input freeze repaired from the
headless run. The next visual reproduction needs a QEMU trace spanning SDL
input, QEMU input events, and PS/2 delivery while the real mouse and keyboard
are used. Compare that with VM focus and guest device counters at the same
time. A rendering change for pointer latency needs a direct latency measure
with enough samples and a repeat run of the user's Files/volume/Doom path.

## Live visual reproduction with the owner

The owner repeated the failure in the traced SDL/X11 QEMU session and
confirmed that keyboard and mouse did not control Doom. Artifacts are in
`build/tests/user-visual-input-2026-10-03/`: `physical-serial.log`,
`physical-qemu.trace`, `physical-state-blocked.bin`, and
`physical-blocked.png`. The serial log again reaches `ST_Init`; the screenshot
shows live Doom gameplay in window 11 with Files behind it. The RAM snapshot
has `vmm_focus=1`, `vmm_mouse_home=1`, `vmm_ticks=4193`, and
`vmm_switches=3461`: the manager still switches VMs.

The QEMU trace records 14 keyboard events from the SDL window, two later Esc
events from the QEMU monitor, and 3419 PS/2 mouse packets over the session.
Specifically, SDL translated the owner's dedicated
arrow keys into Linux scan codes 108/105/103/106, and QEMU emitted matching
`ps2_keyboard_event` and `ps2_put_keycode` entries. Therefore the tested
arrow keys reached QEMU's emulated 8042; the failure for those keys is further
downstream or in Doom's interpretation of the extended scan codes. The trace
also shows many relative mouse events reaching `ps2_mouse_send_packet`. It
does not prove the desktop consumed those IRQ12 bytes or forwarded the
corresponding `DEV_MOUSE` deltas to Doom. One later Esc came from the QEMU
monitor (`con -1`); its resulting screen change is not enough to distinguish
an Esc response from Doom's attract-mode progression. The owner also pressed
Ctrl+Alt+G to release QEMU's grab; those key events should not be counted as
game input. The diagnostic QEMU session was then closed.

## Physical-ISR ownership audit and fix, 3 October

The new SDL/X11 reproduction in
`build/tests/input-fix-2026-10-03/sdl-baseline/` reproduces the owner's
persistent state: VM 0 has `guestIF=0200h`, pending `1001h` (IRQ0/IRQ12),
ISR `0001h`, and 1092 owed timer ticks. Its saved frame is `1180:6380`, back
in the shell input loop. The captured SeaBIOS timer handler executes its
master-PIC EOI at `F000:F532`; the handler has returned, yet the virtual
IRQ0 remains in service. The live device instance is enabled and focused;
the owner's blocked dump has no physical keys forwarded, and an empty
keyboard queue. No extended-key rewrite is justified.

Sources checked before implementation: the
[Intel 8259A specification](https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf)
describes fixed priority and in-service/EOI arbitration. The pinned
[HDPMI IRQ path](https://github.com/Baron-von-Riedesel/HX/blob/f2276db9accfc57facf2588bc016a27130597bb1/Src/HDPMI/HDPMI.ASM)
passes physical IRQs to its client; CiukiOS's IRQ1/IRQ12 bridge acknowledges
them only after `DEV_PHYSICAL_IRQ` returns. The
[Jemm monitor](https://github.com/Baron-von-Riedesel/Jemm/blob/master/src/JEMM32.ASM)
with the local scheduler/profile patch calls the V86 IRQ0 callback before
`CvIRQ` acknowledges that IRQ0; its ring-0 IRQ path acknowledges first.
The local PIC broker sends an EOI to the physical PIC while a physical ISR
is present. DPMIRUN chains to the BIOS timer EOI before `VMM_YIELD`.

The urgent input, audio, and HLT switch paths already check physical ISR
ownership. The regular `vmm_irq` and `vmm_dpmi_tick` paths did not. An IRQ0
can interrupt the real-mode leg of HDPMI's physical input bridge and switch
to VM 0 while the input IRQ remains in service. VM 0 must not inherit the
other VM's outstanding physical acknowledgement. This is a concrete unsafe
switch in the implementation; the exact first misclassified EOI in the
owner's historical run was not traced.

Decision: guard both regular timer-switch paths. At the monitor's V86 IRQ0
callback only its own IRQ0 bit may be ignored, since `CvIRQ` acknowledges
it before returning to the resumed VM; all other physical ISR bits defer
the switch. Ring-0 and DPMI callbacks require both physical PICs idle.
Remember a deferred slice and complete it at the next safe return to V86,
retaining DOS/FAT gates and urgent input/audio priority. Honor the existing
no-switch flag for a client's virtual-CLI bridge. Keep `VMM_SLICE=2` and
retain the PIC's physical/virtual EOI rules. `CVDEFER1` records deferred
requests and completed switches for validation.

## Follow-up after host kernel faults

The sequential headless M4 run in `build/tests/input-fix-2026-10-03/headless-safe/`
confirmed Doom fire input, non-silent game PCM, and bounded window close. It
failed to open the desktop menu after starting COMMAND; it did not pass the
two-VM gate. A diagnostic repetition ended during the host fault described
below and is not valid evidence of guest responsiveness. Its live Jemm PIC
state, identified by the exact `CvTrapPorts` sequence, had IF=0200h, pending=0,
and in-service=0. Saved VM records alone are not the live PIC state.

At 11:44:07 the host logged `Oops: invalid opcode`, `Comm: rg`, and
`rcu_gp_kthread.cold+0x38/0x4a`, with Btrfs directory lookup/statx frames.
The excerpt is `build/tests/input-fix-2026-10-03/host-kernel-oops-114407.log`.
The two QEMU scopes peaked at 217 MiB and 167.9 MiB; no OOM event was logged.
Further builds, emulator runs, indexing, and directory walks were stopped.

A bounded source audit found another unsafe physical/virtual EOI overlap:
`CvLeave` can inject pending virtual IRQs on a V86 return from HDPMI before
the host acknowledges its physical IRQ. Profile function 8 can likewise
accept a virtual IRQ for the protected-mode client while a physical ISR is
still set. Deferring a VM switch alone does not guard either operation.
The Intel 8259A and pinned HDPMI sources linked above remain the basis for
the ownership rule: a virtual handler must not start before the physical
handler finishes its EOI. The physical-first EOI broker is retained.

Decision before implementation: check both physical PICs before V86 virtual
IRQ injection and protected-mode virtual IRQ acceptance. Preserve the pending
request and all caller registers on deferral. Keep scheduler intervals,
virtual PIC priority, and the VM-state ABI unchanged.
Mask physical interrupts through the ISR check and the acceptance/injection
operation, restoring the entry FLAGS on return; an ISR snapshot alone would
otherwise allow a race if a caller arrived with physical IF set.
`pm_accept` retains
its pending bit when profile function 8 refuses delivery, so this guard does
not discard protected-mode device requests.
This is an additional source correction, not a claim that the owner's
physical-input run is fixed. It requires a future build and runtime validation
on a stable host; the HDD and Windows ZIP built at 11:19 contain only the
earlier CVSESSION guard.

Offline checks: `git apply --numstat` parses the updated scheduler patch
with its 1015-line CVIRQ include. The CVIRQ hunks of the existing HLT patch
apply in `patch --dry-run --batch` to that extracted include, with no fuzz.
Temporary files for this check lived only under `/run/user/1000` and were
removed. No compiler, emulator, directory discovery, or new image build was
run for this follow-up. The physical-ISR probe and virtual acceptance are
inside saved-FLAGS/CLI regions; both success and refusal restore that FLAGS
stack entry, while the protected-mode service returns its usual status CF.

Cleanup removed old generated images, ISOs, and VM snapshots listed in
`build/cleanup-manifest-2026-10-03.txt`. Build usage fell from about 386 GiB
to 68 GiB; Btrfs sharing means the actual recovered free space was about
45 GiB. Prior dirty source changes, build objects, current images, dependency
archives, and the owner's 3 October evidence were preserved.

## Owner confirms commands work; latency persists after exit

The owner subsequently reports that Doom starts, plays audio and responds to
controls, but the system and pointer remain slow even after Doom closes.
The current `build/full/qemu-visual.log` (6131 bytes, modified 12:50:13 local
time) records `HDPMI32 uninstalled`, `[DPMIRUN] VM END`, `[DOSVM] ended`, then
window 11 close and `[DOSVM] closed`. This is an actual terminated VM, not
merely a game that returned to a DOS prompt. This log was read in place;
no emulator was launched for the following audit. Its exact 6131-byte copy is
`build/tests/input-fix-2026-10-03/owner-latency-1250.log` so a later visual
launch cannot overwrite this evidence.

The [Intel 80386 manual](https://pdos.csail.mit.edu/6.828/2018/readings/i386.pdf),
chapter 15 and the instruction exception tables, specifies that V86
interrupt/flags instructions trap when IOPL is below 3. The local Jemm
profile deliberately sets IOPL=0, handles these instructions, and calls the
owner's poll on returns to V86. `vmm_arm` enables this profile at the first
fork. Neither `vmm_kill` nor the DEAD-record release in `vmm_switch` releases
it after the last child VM; only module unload does. Thus the desktop keeps
the interrupt emulation and poll overhead after a game has completely ended.
This persistence is demonstrated by the code, but its latency contribution
has not been measured in this follow-up.

Decision before implementation: park the VMM profile at a canonical return
to desktop VM 0 when it is the sole VM, all sessions/audio/schedulers and
native allocations are gone, owed ticks have drained, logical IF is set,
and physical and virtual PICs have no outstanding IRQ. Check and release
under saved FLAGS/CLI so a new physical interrupt cannot race the operation.
Keep the initialized VM/page tables and CMOS tracking; `vmm_arm` reactivates
the profile at a subsequent DOS fork or native-process start. Clear obsolete
routing state and cached virtual-PIC validity only after successful release.
The PIC quiescence requirement follows the
[Intel 8259A EOI rules](https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf):
an in-service handler must finish acknowledging its IRQ before its virtual
PIC ownership can be withdrawn. Jemm's `CvLeave` must notice a release by
its poll callback and restore the non-profile path in that same return,
instead of rematerializing IOPL=0 after removing the callback.

A separate arithmetic defect was found in `session_devices.c:advance`:
microseconds are multiplied by about 1193 instead of 1.193182 to obtain PIT
clocks. `cvgp_advance` consumes actual PIT clock units; this advances OPL
timers about 1000 times too fast. The physical PIT itself is not reprogrammed
by this calculation. The [Intel PIT description](https://cdrdv2-public.intel.com/332995/332995-skl-io-platform-datasheet-vol1_rev004.pdf)
gives the 14.318 MHz / 12 clock. Correct the conversion with bounded 32-bit
integer arithmetic and retain fractional clocks across polls. Neither change
reduces `VMM_SLICE` or changes the physical PIT divisor. Build and runtime
validation remain suspended because of the host kernel faults.

The physical-ISR guard added to `CvLeave` currently performs six physical
PIC I/O operations on every interruptible V86 return, even when there is no
unmasked virtual IRQ to inject. These register accesses are unnecessary in
that case. Move the guard after the pending/mask/cascade eligibility check,
keeping the same saved-FLAGS/CLI region and the guard before any virtual ISR
bit is set. This removes hardware I/O from empty returns without relaxing
the physical/virtual EOI ownership rule. Its performance effect still needs
a future measurement; the redundant accesses themselves are demonstrated
by the code.

Implementation: `vmm_park` now applies these quiescence guards at the end of
`jlm_poll`, preserving FLAGS/registers. It releases the profile first, then
the exact tick callback. If tick removal fails, its owner bit remains set
and the request/filter/poll/HLT callbacks are restored together while IF is
still clear; it never reports a successful park after partial release.
`vmm_arm` uses the same callback setup on the next fork/native start.
The pinned profile's valid setters 1/4/6 and the HLT patch's setter 11 store
their value and return through `profile_state` with CF clear; restoring
these callbacks does not introduce a second fallible resource acquisition.
`CvLeave` restores its owned IOPB bits, physical masks and IOPL=3 immediately
when that poll releases the final owner with no virtual IRQ outstanding.
Its physical-ISR probe now runs only after finding an unmasked pending IRQ,
still before setting any virtual in-service bit. The device time conversion
carries fractional clocks and resets that phase at each `cvdev_begin`.

Static checks: targeted `git diff --check` passed; `git apply --numstat`
parses the 1024-line CVIRQ include. The dependent CVIRQ HLT hunks apply
without fuzz to the extracted include in a temporary RAM-backed directory.
Arithmetic review yields 596 clocks plus fractional phase for 500 us,
4772 clocks plus phase for 4 ms, and exactly 1,193,182 clocks across 250
successive 4 ms intervals; all intermediates fit in 32 bits at the 200 ms
cap. The temporary files were removed. No compiler or emulator was used,
and the owner's 12:49 image has not been rebuilt with these latency changes.
The owner's command/audio success is runtime evidence for that prior image;
neither the active-game latency nor the post-exit latency of these new
source changes is yet runtime-validated.

## Active-game renderer redesign: continuous full-frame gameplay

The owner clarifies that the whole desktop becomes slow as soon as a new
Doom game starts, and authorizes redesigning the rendering architecture.
Parking the profile after exit does not address the active rendering path.

Source audit: `DOSVM.APP` queries damage, then each scratch band invokes
`VIDEO_BAND` on live guest VGA. Band-relative window coordinates and scratch
height change at each call. `cvp_present` consequently rebuilds the complete
palette and scaling maps repeatedly; its NO_DAMAGE branch does not set
`initialised`, so even identical bands redo setup. Vertically enlarged rows
are also converted repeatedly. `ui_pages_sync` paints all previous damage a
second time, calling the live game renderer rather than preserving the
completed front-page pixels. Finally `vc_fb_put_rows` goes through the V86
bank aperture: crossing a bank invokes 4F05, changes PTEs and flushes CR3.
There is an existing protected-mode framebuffer mapping/copy service, but
the desktop band writer does not use it. These multipliers are demonstrated
in code; their individual durations have not been measured in this run.

Primary sources checked before implementation:
[SDL streaming texture lifetime](https://wiki.libsdl.org/SDL3/SDL_LockTexture)
and [SDL presentation lifetime](https://wiki.libsdl.org/SDL3/SDL_RenderPresent)
require applications to retain image data outside disposable presentation
buffers. The [VBE 3 specification](https://courses.cs.washington.edu/courses/cse451/24wi/documentation/vbe3.pdf)
defines physical framebuffer access, display-start page changes, and page
ownership; [DirectDraw Flip](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-flip)
changes the displayed buffer, without regenerating the producer's image.
The Jemm page services already used by CVSESSION remain the sole allocator.

Decision: separate guest-frame capture, retained rasterization, composition
and physical presentation. Give each VM one frozen VGA frame and retained
native scanout rows; capture only when guest damage changes the image, and
reuse unchanged converted rows, palette and scale maps across all compositor
bands. A band copies already-owned pixels rather than repeatedly rendering
the moving producer. Introduce bounded protected-mode desktop row transfer
and page-copy operations so VRAM presentation avoids V86 bank remapping.
Page catch-up copies the exact completed front pixels and never calls an
application painter. Keep existing drawing APIs, window occlusion, format
validation, DOS compatibility and fallback video paths; do not lower the
game's resolution or impose an artificial refresh-rate limit. Allocation is
sized to the actual source rows/window width and released with its VM.

Native VRAM transport in the supported V86 profile is distinct from the
real-mode LFB writer. `vc_validate_layout` deliberately sets `vc_lfb=0`
under Jemm, so checking that flag would silently leave every desktop band on
the old bank-remapping path. QEMU Standard VGA registers PCI BAR0 over the
same `s->vram` used by its banked aperture, independently of the LFB mode bit:
[QEMU VGA PCI source](https://raw.githubusercontent.com/qemu/qemu/master/hw/display/vga-pci.c),
[QEMU VGA bank alias and scanout source](https://raw.githubusercontent.com/qemu/qemu/master/hw/display/vga.c),
and [VGABIOS VBE PhysBasePtr source](https://github.com/qemu/vgabios/blob/master/vbe.c).
Decision: permit native copies in banked mode only after a Bochs DISPI ID
check; preserve and restore its index port. Keep the validated active banked
pitch/RGB format and VBE PhysBasePtr/extent. Other adapters keep their
existing banked path. Neither guest VGA limits nor the SHELL.COM arena ceiling
is increased. Cursor storage and rectangle-subtraction workspace move from
that code arena into the existing desktop resource allocation; the artwork
file format and approved Ciuki assets are unchanged.

### Implementation and bounded checks completed

The active-game redesign is implemented in source. `session_video.c` now
captures one immutable VGA image on a changed DAMAGE query. The retained
presenter preserves its palette, scaling maps and converted source rows
across compositor bands, with storage sized to the actual source geometry
and window width. Closing the session releases that storage; a failed
release retains ownership and refuses final teardown, allowing a retry.
ARM and the retained DAMAGE path cannot consume the dirty maps concurrently.
VIDEO_STATE offsets 240/244/248/252 report captured images, unique source
rows converted for the current image, storage capacity and readiness.

`ui_comp_list` synchronizes the hidden page by copying only previous damage
minus current damage. It never invokes an app painter for page catch-up.
Eager copying during idle was removed: identical full-game damage on the
next frame therefore requires no old-frame VRAM read/write. Subtraction uses
two bounded lists of 81 disjoint fragments per previous rectangle, with a
conservative whole-rectangle copy on overflow. Both these lists and the
pointer save/row buffers live in the desktop resource block, which is
released with that desktop. Shared VBE consumers retain their own local row
buffer. `dosvm.c` no longer fills a successful guest band black first.

Native CVFR/CVFC transfers validate all pages, pitched extents, offset wrap,
packet headers, direction and framebuffer bounds. They copy at most 16 KiB
and 64 rows per call, with read-only guest sources accepted and page-copy
overlap rejected. The shell checks the full capability packet and physical
binding identity/extent. Banked acceleration is gated by Bochs DISPI identity;
other VBE adapters retain their original access path. No scheduler slice,
game resolution, frame-rate limit or VGA memory limit is changed by this
renderer redesign.

Checks ran sequentially in systemd user scopes with MemoryMax=256M,
MemorySwapMax=0, CPUQuota=25%, TasksMax=16 and short timeouts. Outputs were
created in /dev/shm and removed, without producing HDD images, RAM dumps or
QEMU traces. Results:

- Host retained-presenter regression: PASS. Verifies frozen scanout after
  live VGA writes, reuse across different band shapes, partial dirty-row
  invalidation, off-screen clips and invalid format/storage rejection.
  At the actual 674x434 DOS-window size, both 24-bit and 32-bit output
  convert exactly 200 source rows through 20 scratch bands; a repaint
  adds no conversion. This is a work-count regression, not measured FPS.
- Pinned OpenWatcom compilation of vga_presenter.c and session_video.c with
  warnings treated as errors: PASS.
- Pinned JWasm assembly of CVSESSION: PASS, zero warnings/errors.
- Freestanding CVSESSION link with the unchanged cached dependency objects
  and nodefaultlibs: PASS. This verifies linkage, not a qualified full build.
- NASM SHELL: PASS, 60896 bytes, arena end EEE0h below the existing EF00h
  ceiling. No ceiling increase. Shared VGASETUP assembly: PASS, 21552 bytes.
- Targeted diff whitespace check: PASS.

No full build, QEMU run or Windows-bundle refresh was performed for these
changes. The current HDD image/release bundle do not contain this redesign.
The owner's new-game physical-input scenario remains the runtime acceptance
criterion, including pointer behavior after Doom closes. The defect is not
marked resolved by the source and host checks above.

## Implemented performance follow-up

The independent VM PIT, changed-only frame notification and VGA mapping work
was subsequently implemented and validated. See
[the integrated result](2026-10-03-independent-vm-clock.md) for the final image,
34.39 game tics/s measurement, concurrent second-VM check, successful game exit,
and measurement limits. This does not retroactively change the earlier
physical-input evidence or claim a new physical SDL/Windows test.
