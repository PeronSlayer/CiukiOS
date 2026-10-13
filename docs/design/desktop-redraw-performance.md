# F1-25 desktop redraw pipeline

Research and repository comparison, 2026-10-13 (host implementation; lead owns
QEMU and hardware qualification).

* [DRM damage semantics](https://cdn.kernel.org/doc/html/latest/gpu/drm-kms.html):
  retain bounded, clipped damage and force the complete plane only when its
  contents or geometry are invalidated. Ciuki already clips/coalesces damage;
  its cursor currently adds every intermediate position to scene damage.
* [SDL event draining](https://wiki.libsdl.org/SDL3/SDL_PollEvent) and
  [timed waiting](https://wiki.libsdl.org/SDL3/SDL_WaitEventTimeout): consume
  queued input before drawing, then sleep between frame deadlines. Ciuki's
  nonblocking input grant has no poll/select; use a sleeping 1 ms input service
  cadence and an independent 60 Hz maximum rendering cadence. The VBE inventory
  exports no measured refresh rate, so 60 Hz is a conservative policy, not a
  detected monitor rate.
* [VESA VBE/Core 3.0, display start / page flipping](https://www.phatcode.net/downloads.php?action=get&file=vbe30.pdf&id=221):
  software back buffers avoid publishing intermediate drawing, but true scanout
  tear prevention needs retrace-synchronised display start or a qualified GPU
  path. The current VBE LFB copy API has neither. Do not claim that software
  buffering establishes hardware tear-free scanout or add unqualified VGA I/O.
* [POSIX fsync](https://pubs.opengroup.org/onlinepubs/009695399/functions/fsync.html)
  may wait for storage. Move settings writes and durability operations to a
  bounded worker with a latest-snapshot mailbox; no disk operation under its
  mailbox mutex. Keep errors visible and shutdown pending while the writer is
  busy. The existing sound worker establishes native pthread support.
  Theme polling also uses the worker's bounded read mailbox; changing mouse,
  keyboard or clock preferences retains the rendered wallpaper instead of
  decoding the default PNG again on each reload.
  Display inventory is copied only after its notification counter changes,
  so the 1 ms input cadence does not rescan PCI/mode information every turn.
  The mixer syscall's `audio_master_set` also commits AUDIO.CFG synchronously;
  the desktop queues copied gain/mute records to the same worker, using its
  existing delegated descriptor. The kernel API/durable format are unchanged.
  Slider updates coalesce to the latest value; errors and unsaved state return
  to the shell, and shutdown waits before closing that descriptor.
* [Linux i8042 interrupt delivery](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/input/serio/i8042.c):
  native Ciuki input already arrives through IRQ1/12; its setup polling is
  finite and unrelated to redraw. Preserve activation and device sample rate.
  Reduce desktop consumption latency; inspect firmware input service separately.

Implementation decisions: composite all scene rectangles in a private RAM
buffer before copying any completed rectangles to the presentation surface.
Keep the scene cursor-free; restore/save the small arrow underlay and present
only its old/final footprints. Retain failed presentation damage for retries.
The shadow painter skips each opaque interior span at once: it previously
visited every client pixel solely to discard it during each window move.
An independent pixel reference checks clipping and the unchanged falloff.
Cache the entire work area including icon glyphs/halos; refresh icon cells only
when their visual state changes. Clock damage is its tray text box, once per
minute when seconds are disabled. Frame counters distinguish scene pixels,
presented pixels, successful calls, LFB bytes and composite/present/input times.

Display sequencing: `display_set` explicitly rejects `awaiting_frame`, not the
Control Panel's private surface or a windowed DOS VM. Defer successful settings
replies until the new complete frame is presented; retain the kernel watchdog.
Control Panel retries transient EBUSY with a finite deadline and explains it.

Telemetry uses explicit v2 `DESK_SYSTEM_QUERY` (8 bytes) and
`DESK_SYSTEM_INFO` (80 bytes), restricted to the existing Task Manager identity.
The reply contains a serial and nine little-endian u64 values: frame attempts,
present damage pixels, successful present calls, LFB bytes, composite ns,
present ns, input-handling ns, maximum event queue lag ns, sample interval ns.
The sample covers at least two monotonic seconds. Task Manager displays the
sample, average damage/composite/present per frame and its maximum input lag.
Verbose output is queued to the worker, so serial/log I/O does not run in the
renderer. The existing runtime gate capture paths retain their deliberate
full frames and pacing; normal desktop frames follow the deadline.

Memory: no allocator limit or hardware qualification is changed. The 128 MiB
reference floor and 64 MiB unqualified target in `hardware-baseline.md` remain
binding. At 1024x768 the extra scene buffer is 3 MiB and the complete background
cache is 3 MiB; at 1600x1200 each is 7.325 MiB. Host counters and allocation
failure tests do not measure physical E820 reserved/free RAM; the lead must
record those with the new image before qualifying the hardware budget.

Logging: bootlog producers already capture bounded RAM chunks; disk draining
uses the existing storage worker. Batch normal drains at 250 ms rather than
on every log byte at the 1 ms timer cadence, leaving explicit shutdown drains
and writeback deadlines intact.

Wallpaper startup (f2-44, 2026-10-13): the native pthread contract in
`posix-subset.md`, `src/kernel/proc/thread.c` and the SDK's `pthread.h` provides
kernel-scheduled worker threads. Following [POSIX thread creation](https://pubs.opengroup.org/onlinepubs/000095399/functions/pthread_create.html)
and [mutex memory synchronization](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/V1_chap04.html),
use a bounded latest-request mailbox and transfer ownership of a completed
XRGB frame under its mutex. No filesystem, decoder or scaler operation holds
that mutex; the desktop polls with trylock and never joins a busy worker.
The first frame is solid colour. Start wallpaper work only after presentation;
obsolete requests cannot publish over a newer path, mode, colour or geometry.
[stb_image 2.30](https://raw.githubusercontent.com/nothings/stb/master/stb_image.h)
decodes memory buffers; serialize wallpaper decoding in this worker and retain
the existing integer renderer. Cache reads and writes also belong to the worker,
including a cache hit (a multi-MiB disk read cannot run on the input thread).
The actual native API limits a file syscall to 1 MiB and `byte_io` currently
returns at most one 4 KiB page (`syscalls_file.c`, SDK `posix.c`, F2 file-I/O
contract). Following [POSIX read](https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html),
loop over bounded requests and handle short transfers/EINTR in the worker for
both source and cache files, and likewise for writes. A literal one-syscall
3 MiB frame read is unavailable; the UI instead receives one completed pointer
and does no read at all. No kernel/API scope change is needed.
Use a temporary file and [atomic rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html)
before publishing cache files. Cache failure leaves the in-memory result usable.
The namespace is `/System/cache/wallpaper`; its filename uses the first eight
hex digits of SHA-256 of a versioned canonical string including the source
path, size, mtime, geometry, mode, effective colour and bar sizes. Keep the last
three runtime frames by eviction on use/write. Five default frames are seeded
by the image builder; the first runtime access trims them to three. FAT mtimes
are UTC with two-second precision in Ciuki (`px_stat_locked`); image population
preserves the source mtime and pre-render keys use the same FAT normalization.
Host tests establish mailbox/cache behaviour and pixel equivalence, not the
T23's 50 ms reaction bound or hardware memory qualification.
Verbose startup stages are captured at their monotonic event times and drained
by the settings writer's separate ten-record mailbox. A cache hit reports the
read stage with `cache_hit=1` and the decoder/scaler stages with
`cache_hit=1 skipped=1`, distinguishing pre-rendered work from live decoding.

Integration with f1-26/f2-42 (2026-10-13): rechecked the
[SDL queue-draining guidance](https://wiki.libsdl.org/SDL3/SDL_PollEvent) and
[X.Org Savage copy/fill implementation](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c).
Retain input draining, frame deadlines and the private cursor-free scene; use
the incoming synchronous copy/fill presenter against its last completed front
cache, after completing the RAM output. A failed copy queues full reconstruction
for the next frame before any further upload. Software cursor motion remains a save-under overlay; a hardware cursor
moves without scene damage or uploads. Hardware faults retain full recovery
damage and do not acknowledge a display change before recovery succeeds.
Count successful rectangle operations as presents, and only CPU uploads as
LFB bytes. Keep the accelerator front cache separate from the scene because
failed/partial uploads must compare against the pixels actually presented.
This adds one more width*height*4 allocation on rectangle-capable displays;
physical memory qualification remains with the lead.

Keep main's FILE_DROP opcode (25), then assign SYSTEM_QUERY/INFO 26/27; all
clients build from the merged header. Refresh updates the real desktop folder
and reloads settings/wallpaper. Icon moves invalidate the background cache;
inline rename renders live over wallpaper and invalidates the stable cache
until editing ends. AC'97 diagnostics and asynchronous Control Panel settings
saving share the merged host callbacks.
