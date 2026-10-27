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
