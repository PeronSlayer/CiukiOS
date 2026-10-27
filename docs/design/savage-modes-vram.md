# Savage target qualification and revocable cache (f7-15)

Research checked 2026-10-26 before implementation. This extends
[the display owner contract](display-driver-model.md) and supersedes the
scanout-bound qualification and CPU masked-clear rules in
[full-size admission](savage-fullsize-admission.md) and
[default formats](savage-default-formats.md).

Primary sources:

* [Linux v6.1 savage_state.c](https://github.com/torvalds/linux/blob/v6.1/drivers/gpu/drm/savage/savage_state.c),
  `savage_dispatch_clear`, sets BCI register `SAVAGE_BITPLANEWTMASK` (0xd7)
  around rectangle clears and resets it afterward. The pinned local source
  was read because the upstream web fetch failed. Use the engine for partial
  D24/S8 clears; save, restore and verify the inherited mask as part of the
  existing complete register restoration, including error returns.
* [X.Org Savage 2.3.9](https://www.x.org/releases/individual/driver/xf86-video-savage-2.3.9.tar.bz2),
  `savage_dri.c`, places front/back/depth/texture storage within local VRAM
  and programs tiled surfaces separately from scanout. The pinned local
  source is the fallback for the unavailable web archive. Retain Ciuki's
  checked registry claims, cursor exclusion, guards and alias mappings.
* [Quake III win_glimp.c](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/win32/win_glimp.c),
  `GLW_SetMode` and `GLW_InitDriver`, changes display settings before creating
  the GL window/context and selects formats for the resulting desktop.
  A mode change must revoke old leases while allowing fresh hardware contexts
  from retained target qualification, without repeating the full raster gate.
* Windows 98 Resource Kit, chapter 30, `wrkc30_3.htm` (local owner media,
  read-only): the display driver describes video-memory heaps and DirectDraw
  chooses among local, non-local and system memory. This does not justify
  treating aliases as extra memory or adding AGP support.

The 2D offscreen rows are revocable cache storage. Return their registry claim
before claiming a guarded 3D heap. Restore the rows only after every 3D writer
is proved idle, mappings revoked and claims released; invalidate cache content
on each transfer. Serialize this transfer with display batches and presenters.
Each successful transfer increments a separate cache epoch without revoking
scanout or presentation grants. `CIUKI_ACCEL_CACHE_EPOCH` (14) returns it;
legacy offscreen COPY/WRITE carries it in `background`, and a VIDEO batch
reference carries it in `reserved[0]` (all other reserved words stay zero).
Preflight and locked admission both check it. The desktop drops cached shadows
on an epoch change and uploads before reuse, even if the row range is identical.
No scanout/cursor reservation is borrowed. Context lifetime owns the reservation;
qualification uses a temporary reservation and returns it afterward.

Qualify 640x480, 800x600 and 1024x768 targets independently of scanout in the
idle background worker. Retain results by PCI identity, framebuffer/alias bases
and VRAM layout identity. A mode stop invalidates resources and presentation
leases, but retains these results. A fresh mode rechecks the inventory-derived
heap, pitch, guards, tiled copy and restore with the bounded minimum gate;
it does not repeat the full-size reference/precision gates. The cached minimum
engine check has an additional 50 ms deadline. The full-screen lease
transaction calls the family-neutral native prepare hook before publication, outside its locks, so initialization and marker I/O
do not consume the unchanged 2 s GL wire-call deadline. Native attachment also
calls the same hook for other mode-change paths, without requiring a new
desktop frame (a full-screen lease may already own that mode). If GL arrives
before the new 2D marker worker finishes, attachment waits at most 2 seconds
outside the presenter, then nominates this check without a presentation
event. Marker I/O and preparation retain their existing separately bounded phases. Device/layout or
integrity changes invalidate retained results. Existing durable marker and
phase/total budgets still cover each attempt. Transient presenter contention
retries only between restored windows and within those same budgets.

Ordinary readback has one absolute 200 ms budget across all attachments and
guards, in addition to the existing 128-read/1 ms windows. It restores state
and fails the context cleanly on expiry. This is an admission/latency policy,
not a claim that T23 silicon meets the budget. Three 1024x768 four-byte
attachments plus guards require about 9 MiB; 200 ms accommodates a stated
50 MiB/s readback assumption, whereas 20 ms would require over 450 MiB/s.
The unchanged engine execution window remains 20 ms. Host fixtures cannot establish
hardware throughput or BIOS mode-switch correctness.

Full-screen native presentation uses the GPU tiled-to-linear rectangle directly
into the current scanout, with its current pitch. Smaller unscaled drawables
are centered with engine-cleared black borders. The lease
presenter checks process, token, display generation, scale and identity gamma
before invoking the private backend hook. The renderer requires a resident
owned linear copy and a receipt naming its still-live, unmodified tiled source;
colour writes, uploads and frees invalidate that receipt. Scaled frames,
nonidentity gamma or an unavailable source use the canonical CPU path. No wire
record contains a physical address. The same engine/idle/restore rules and
source guards apply to the native copy, for XRGB8888 and RGB565 scanout.
Linux `savage_dispatch_swap` supplies the front/back rectangle precedent;
Ciuki uses the already qualified inline source/destination descriptors rather
than changing the retained global bitmap descriptor.

## Review round 1: GPU-resident targets, contained budgets (f7-15)

Checked 2026-10-27 against the sources above, the f7-1213 review and the
lead's amendment 1 to `docs/directives/f7-15-modes-vram.md`. These rules
supersede the per-job readback and the budget wording above where they
differ.

Register evidence for the masked clear: S3 RE046-B, page 140, "Bitplane Write
Mask Register (MM8128)", BCI index D7H ("if bit i = 0, bitplane i is not
updated"); X.Org `savage_bci.h` names the same index `BCI_BITPLANE_WRITE_MASK`
and `savage_accel.c` programs MM8128 for the 2D engine. Linux v6.1
`savage_dispatch_clear` sets it around `BCI_CMD_RECT|XP|YP|SEND_COLOR|
DEST_PBD_NEW`, ROP 0xCC (0x4BCC8C00, the word `clear_resource` emits) and
resets it to all ones. The boot D24/S8 gate runs the same `execute` path as
the runtime, so it measures the masked clear that games use.

**GPU-resident render targets (lead decision).** A hardware job leaves its
attachments in VRAM: `execute` reads nothing back and no longer checks the
full guard pages of resident attachments on every job (2048 uncached reads
each). The family-neutral service marks the written attachments `stale`
(backends with the new `download` hook only) and downloads one only when the
CPU needs it: a READ of that attachment (glReadPixels, the opengl32 sync), a
scalar job of a hardware context, or a CPU presentation copy. A PRESENT asks
the presenter first for its direct native path with NULL pixels; the linear
copy is read back only when presentation needs the CPU (scaling, gamma,
conversion). A job whose first engine operation overwrites a non-resident
attachment completely (a full clear, or the copy into the linear target)
uploads nothing: a frame that starts with a full clear never transfers the
previous contents. Guards are checked completely on every download, every
qualification read and every native scanout source; each COPY job (one per
presented frame) samples every 256th byte of each guard page.

Runtime CPU transfers (a job's uploads, one download) keep the job's
`drawing` claim and `accel_inflight` and yield only the presenter mutex every
128 accesses, under one absolute 200 ms deadline (`SAVAGE3D_READBACK_US`)
that no window restarts. Between windows the job's own AGD/tile values stay
programmed; the inherited values are restored and verified at the end of
`execute`/`download`. A download timeout after proven idle fails only that
context, as before; other errors lose the device. A passing presenter
conflict is retried for at most 20 ms (`R3D_SYNC_RETRY_US`, 1 ms sleeps).

**Shared opengl32 journal.** RAM is a checkpoint read back only when the CPU
needs the pixels. A successful GPU swap reads nothing back; the front buffer
of that swap is the provider's completed presentation copy, read on demand
(`read_presented`) when the application reads, draws or presents the front.
An unchanged `glDrawBuffer` selection (Quake III selects GL_BACK every frame)
keeps the GPU copy. A full journal is submitted and dropped without a
readback (`gpu_only`); a full unmasked, unscissored clear of every attachment
drops the commands before it and needs no upload. `glFinish` on a window DC
waits for completion only; a bitmap/memory DC is still made coherent. A
semantic refusal on a healthy context reads back before the scalar replay; a
transport or device failure replays the journal on the last checkpoint.

**Mode switch at the first swap.** CiukiOS takes the full-screen lease at the
first presented frame, so Quake III's context exists in the desktop
generation and the lease's mode switch revokes it. opengl32 makes RAM
coherent once before a first lease, probes the new generation after it, and
treats a generation change (also when found by a failing call) as a
revocation, never as a permanent hardware failure. The new context reloads
RAM or starts from a full clear.

**Contained budgets (f7-1213 P2-1, lead decision).** After the qualified
minimum every gate is optional: the full-size targets (640x480, then the
1024x768 desktop-mode lease size, then 800x600), each with D24/S8 before
RGB565. Optional gates must end `SAVAGE3D_PROBE_MARKER_US` before the 5 s
total, so the marker CLEAR keeps its own reserve. An optional overrun with a
restored, idle engine refuses only that gate (a total overrun also stops the
remaining gates), keeps the minimum and earlier targets, retains the partial
results for the layout, and the marker is cleared normally. The cached 50 ms
revalidation is contained in the same way: that mode is refused and the
marker cleared. An overrun without a restored, idle engine and every
mandatory-phase overrun keep the retained-marker rule. The phase deadline is
disarmed when a phase ends, so cleanup and logging are never `budget_phase`.
The texture pool of each mode is what its largest admitted targets leave:
a refused D24/S8 returns its share.

**Runtime reservation.** The first hardware context retries a passing
drawing-domain conflict for up to 100 ms (1 ms sleeps, preparation pin held)
instead of becoming a permanent software context; attachment is refused
while a qualification is still finishing. The direct scanout retries a
passing conflict for up to 20 ms outside the lease lock, because Quake III
treats one failed SwapBuffers as fatal.
