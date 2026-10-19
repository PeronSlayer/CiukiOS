# F5-08 presenter and delegated display interface

Research before implementation, 2026-10-19. The checked-in VGA presenter consumes
model DAC indexes, the framebuffer device accepts XRGB8888, and call 74 restricts
mode setting to the desktop. F5-04 supplies authenticated desktop channels but
no delegated presentation operation. These amendments retain that authority.

Primary sources inspected:

- Microsoft [DDPIXELFORMAT](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/ns-ddraw-ddpixelformat): explicit RGB masks and indexed formats.
- Microsoft [SetEntries](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawpalette-setentries): first entry/count updates to a palette object.
- Microsoft [SetGammaRamp](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawgammacontrol-setgammaramp) and [DDGAMMARAMP](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/ns-ddraw-ddgammaramp): three 256-entry WORD channel ramps.
- Microsoft [memory DCs](https://learn.microsoft.com/en-us/windows/win32/gdi/memory-device-contexts) and [Blt](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-blt): retained RAM surfaces and explicit transfers.
- [VESA VBE 3.0](https://www.cs.utexas.edu/~dahlin/Classes/UGOS/reading/vbe3.pdf), functions 01h–03h: use the confirmed pitch, masks and linear framebuffer; existing mediated mode transactions remain responsible for validation and rollback.
- [X.Org Savage driver](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c) and [Linux savagefb](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c): existing chip-specific programming stays in the qualified providers. This amendment introduces no register programming.
- [Intel SDM Vol. 3A](https://cdrdv2-public.intel.com/825758/253668-sdm-vol-3a.pdf), memory types and write-buffer ordering, and [Linux MTRR documentation](https://docs.kernel.org/arch/x86/mtrr.html): reuse framebuffer serialization and write draining, including on revocation; never add a user framebuffer alias.

Decisions: convert RGB565, XRGB1555, INDEX8 and XRGB8888 into native RGB
masks with a per-channel gamma LUT. Expand 5/6-bit source values by bit
replication; truncate gamma WORDs to their high byte, then to target precision.
Palette values are 0x00RRGGBB. Scale by 1, 2 or 3, centre, and keep borders
black even with a gamma ramp whose zero entry is nonzero. Timer pacing follows
the Win32 contract's 60 Hz monotonic policy. Hardware retrace is unavailable.

## Handoff to f5-09 (shared interface; desktop wire remains f5-09-owned)

`ciuki/present.h` is the shared surface description: five u32 fields
width,height,pitch,size,format. Formats 1..4 are XRGB8888, RGB565, XRGB1555,
INDEX8; pitches are positive and can have padding. Only the last active row
must fit size. Colours/palettes are little-endian 0x00RRGGBB; X bits are
ignored. Gamma is three arrays of 256 u16 values (red,green,blue).

The compositor can reuse `cvp_surface_row` for its translated/scissored bands
and `cvp_surface_blit` for centred full surfaces with half-open destination
clipping. Source rectangles remain in logical source pixels; multiply damage
coordinates by the selected integer scale before clipping in physical space.
The client retains the backing until synchronous consumption completes. Pins
prevent unmapping; clients must not modify backing while it is consumed.
Palette/gamma replacement, resize, format changes and activation require full
surface damage; ordinary paints carry only changed rectangles. Windowed gamma
is per surface and never changes the desktop framebuffer's global state.
Window generations and wire opcodes belong to f5-09; these formats do not
change existing native surface creation or v1/v2 protocols.

`ciuki/present_abi.h` freezes the additive call-101 copied record operation
`W32_DISPLAY=0x201` (80 bytes). This is separate from desktop wire opcodes.
The supervisor-authorized, registered desktop must have accepted the client's
F5-04 service endpoint; both endpoint descriptors must still match. It alone
can GRANT/REVOKE, with the client's PID and, on revoke, exact token/generation.
GRANT supplies source layout, listed hardware mode and scale 0..3. Scale 0
chooses the largest fitting scale <=3. `ciuki_present_choose_mode` implements
the contract's exact mode / integer multiple / smallest enclosing preference.
The desktop uses this helper, stops compositing, invokes GRANT, and transmits
the returned token, framebuffer generation and scale to its client. A holder
submits FRAME, PALETTE or GAMMA through the same record operation. All except
GRANT zero the unused layout/mode/scale fields. PALETTE uses first/count; the
other operations zero them. Records return result=1 or positive native errno
in error; the Win32 DLL translates revoked EACCES/EIO into SURFACELOST.

The kernel hides the old hardware cursor on grant. The desktop must reset its
accelerator/cursor cache and fully repaint on return, including a lease that
used the same native mode and framebuffer generation.

The desktop must synchronously REVOKE for its contract hotkeys, system modal
UI and normal cooperative mode; then notify activation/surface loss and paint
its restored mode. DOS native takeover and desktop call 74 also revoke in the
kernel. Process teardown and a 10 ms display-worker poll recover stopped
holders. No framebuffer mapping, display descriptor or mode authority is
handed to a game. The existing native and VBE providers are unchanged.

## Lease state machine and ordering

`idle -> transfer -> active(pid, token, framebuffer generation, layout, scale)`.
A failed mode set uses existing confirmed rollback. A failure after a successful
switch restores the saved desktop mode before returning. The desktop grant is
suspended before the mode transaction, after proving there is no in-flight
desktop submission. It stays suspended across confirmation, the final
live-process/endpoint check and publication; failure resumes it after rollback.

`active -> transfer (invalid) -> idle` on revocation. The lease lock serializes
final validation and each <=256-pixel native write with revocation. Native
writes also hold the framebuffer presenter mutex, validate registry/generation,
synchronize acceleration and drain WC writes before returning. Revocation
keeps the desktop grant suspended until restoration completes, then resumes
it. Revocation waits for any such write; no old-holder write can begin after the invalidation. User
pinning, reading, pacing and yields run outside the lease lock and revalidate
before a write. The last old frame must unwind before a new lease is granted.
Palette/gamma updates are refused while a frame is in progress, so a frame
cannot contain mixed LUT generations. Full FRAME submissions include borders.

No mutex crosses firmware. The transfer flag excludes concurrent transactions
while mode switching is asleep. Desktop death is recovered while its grant is
still attached during process cleanup. Restoration failure is reported and the
existing display code keeps unconfirmed framebuffer mappings unavailable.
A delegated mode also retains a 10 s first-frame deadline, revoked by the
display worker if no complete valid frame succeeds. The 10 s desktop frame
watchdog remains the last resort after return; a new lease does not satisfy a pending desktop repaint. Desktop authority is still
required by every mode transaction.

Frames are admitted one at a time and paced against absolute 60 Hz monotonic
slots. The implementation rounds deadlines up to the millisecond PIT clock;
missed slots are skipped. This is neither detected refresh nor vsync. Costs
measured on the host are RAM conversion timings, not T23 framebuffer evidence.

## Host evidence

The implementation report is written to `build/codex/f5-08-report.md`. The
host test entry point is `tests/host/test_presenter_display.py` (also included
in `host_kernel_tests.sh`). Its independent per-pixel oracle pins 24 FNV-1a
32-bit digests over padded targets; full byte comparisons and allocation
canaries accompany those digests. The real lease implementation is exercised
at pin/read/copy/pacing/death boundaries and with a pthread revoker blocked
behind an in-progress native write. Driver tests cover stale generations,
quarantine, extent rejection, accelerator synchronization and write draining;
display-service tests cover queued DOS takeover and the restored-frame wait.
Host evidence remains separate from the pending T23 tests above.

## F5-09B continuation: atomic desktop transaction (2026-10-19)

The lead amendment selects GRANT as the single mode transaction: it executes
under the desktop's call-74 authority, suspends composition, hides the cursor,
confirms VBE state and rolls back before returning an error. The desktop must
not issue a separate call 74 first. The existing kernel transaction and
`ciuki_present_choose_mode` are reused without changing presenter authority.
The v3 lease event adds the returned token/framebuffer generation, retained
source layout, scale and native mode; v1/v2.1 records are unchanged.

Primary sources reviewed for this continuation: Microsoft
[SetCooperativeLevel](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdraw7-setcooperativelevel)
and [RestoreDisplayMode](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdraw7-restoredisplaymode)
require exclusive ownership and restoration of the original display. This
is implemented by the existing f5-08 transaction, not by giving clients a
display descriptor. Microsoft
[ExpandEnvironmentStringsA](https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-expandenvironmentstringsa)
specifies case-insensitive, single-pass percent-variable expansion with
unknown variables preserved; uninstall REG_EXPAND_SZ uses these semantics
with bounded output before the existing command-line parser.
[QEMU fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html) supplies
the explicit probe selector; only the kernel controller emits evidence,
never fixture stdout. Host replay remains host evidence.

The actual copied-record transport is the public `CIUKI_SYS_W32 = 96`;
older call-101 prose above does not renumber that frozen enum.

A follow-up comparison with [Wine 10.0 environment expansion](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/ntdll/env.c) confirms that unknown percent-delimited spans are copied and consumed as a whole. The adapter retains empty pairs and unmatched percent signs, handles adjacent references, and never rescans substituted values.
