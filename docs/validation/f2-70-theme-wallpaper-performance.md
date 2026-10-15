# F2-70 theme and wallpaper transition performance

Research and source comparison, 2026-10-15. Host evidence does not qualify
frame latency on the T23.

[FreeType's cache reference](https://freetype.org/freetype2/docs/reference/ft2-cache_subsystem.html)
keys glyph images by persistent face identities and size. Colour changes do
not change those identities. Ciuki's shell uses immutable FreeType-generated
alpha atlases in `ui_fonts.h`; client FreeType caches are independent of the
shell chrome cache. Retain these atlases and caches across theme changes.
The compositor already keys retained caption rasters by theme and frame
state. Verify that only the transition rebuilds those rasters.

[POSIX condition waits](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_clockwait.html)
release the mailbox mutex while sleeping and require rechecking the predicate.
Keep the existing latest-request mailbox, trylock UI operations and worker-only
I/O. Extend worker ownership to procedural wallpapers and bounded reusable
surfaces. Capture portrait caption style in the request; the worker must not
read the changing global theme while rendering it.

Source inspection found that `wallpaper_load` only submits absolute filenames;
procedural wallpapers go through `desk_shell_wallpaper_update` on a service
turn. `wall_worker` relinquishes its only prepared frame to the desktop, so
revisiting a request repeats disk reads, or decode/scale if its disk entry was
evicted or unavailable. A theme colour is part of that disk key. Avoid repeated
decode on colour/mode changes with a worker-owned decoded-source cache and
retain a bounded LRU of completed frames. Keep the existing file cache identity
and pixel goldens; internal procedural keys include explicit caption style.

The clean last-boot log records a 27,076,648-byte upload window, but neither it
nor the interleaved serial fragments identify the responsible settings event.
Do not attribute that whole window to a theme change or claim a leaked damage
region from it. The replay will measure continued work, exact scanout, settled
damage, cursor state and idle worker state before and after each transition.

## Implementation

All selected wallpapers use the existing worker, including procedural patterns,
plain colour and the approved portrait. Startup/resize can paint a plain-colour
placeholder; an interaction retains the complete displayed wallpaper and its
icon background until the replacement is ready. A wallpaper-only settings
reload no longer invokes the shell's full theme/layout repaint. Publication
requests one full upload. When a theme recolours chrome but the photograph's
work area is identical, publication avoids a duplicate full upload. A combined
theme and photograph change can still have a chrome frame and a later photograph
frame; each is complete.

The worker owns three completed XRGB frames with a total 9 MiB budget, one
decoded source capped at 8 MiB, and one colour-independent ARGB placement layer
capped at 9 MiB. At 1024x768 the layer occupies 3 MiB. Frames are returned as
private copies made on the worker, so publication retains the existing ownership
transfer. Source size/mtime and every rendering input remain cache keys; changing
the source invalidates reuse. A colour-only change blends the existing layer,
without decoding or sampling again. Requests beyond these optional cache budgets
and failed cache allocations retain worker rendering and disk-cache fallback.
Evicted requests may need decoding/scaling again. No kernel memory limit changes.
The supplied last-boot record reports 523,260 KiB usable RAM, 126,816 free NORMAL
pages and 2,274 free DMA pages at boot; this is not a running-desktop measurement
or qualification of the 128 MiB profile.

The existing file-cache keys and disk raw-frame format remain unchanged. Builtin
keys have a separate version and include caption font, colour and antialiasing.
The portrait painter now takes explicit text antialiasing, avoiding a worker
read of the changing global theme. Approved portrait pixels, font coverage,
icon assets and existing golden hashes are unchanged. Chrome cache misses are
counted; replay checks no misses during settled frames. The client font harness
also checks identical face identity and no new glyph-cache misses across every
theme.

## Replay and limits

The production desktop service, settings reader, wallpaper thread and compositor
run with three overlapping 300x200 client surfaces at 1024x768, software cursor
and software upload. The upload hook maintains a separate framebuffer. A fresh,
uncached full painter is the pixel oracle. Every transition is captured under
`build/host/f2-70/`, including recovery from a failed present. Synthetic clients
answer their heartbeat requests, avoiding unrelated unresponsive-window damage.
A 150 ms source-read delay runs on the worker. Every service/render turn is
bounded to 100 ms; the first settled frame must match baseline work within one
second, and the following 60 turns must retain baseline scene/upload/present
counts with no chrome/background rebuild, pending damage or busy wallpaper
worker. Font atlas identities remain unchanged.

The same replay was run against a temporary coherent source snapshot of
`8dc6e9ed`. Only unavailable new counters and positive new-feature assertions
were disabled in that reference. Its final assertion fails with
`main_renders=2`: procedural and portrait work ran on the desktop thread.
Reference theme change decoded/scaled again and presented two full frames;
wallpaper change also presented two full frames. The final implementation
requires one full frame for each separate theme or wallpaper change and no
main-thread wallpaper rendering.

Persistent post-change degradation was not reproduced on the host reference:
all settled windows already returned to 60 frames, 120 presents and 144,960
LFB bytes. Consequently this change does not claim a proven persistent T23
cursor, scanout or damage leak. The new checks reject those conditions in the
replay, including after a failed presentation. Hardware qualification remains
open, with the supplied serial fragments insufficient to identify the event
that produced the large upload window.

## Final host evidence

All runs were sequential under `nice -n 19`:

- Desktop discovery: 101 tests in 305.056 s, OK, five pre-existing native-payload
  skips. Existing screenshot goldens and file wallpaper keys remain valid.
- Desktop gate: ten tests in 47.105 s, OK.
- Desktop performance/settings worker: two tests, OK.
- Real FreeType Control Panel theme/cache and bounded-cache/thread tests:
  two tests in 5.483 s, OK.
- Native i686 syntax: all seven changed production C units passed with the
  pinned ABI and `-O2 -Wall -Wextra -Werror`, using current repository overlay
  headers and the installed SDK libc headers. This is not a link test or SDK
  qualification; the installed SDK's own overlay headers are older.

The complete verbatim output and before/after replay records are in
`build/codex/f2-70-report.md`. The replay's last normal frame costs match its
baseline and it recovers from an injected failed present within one second.
The deliberate observer-pause test remains distinct from the 100 ms service
turn bound. QEMU, T23 frame-gap/memory qualification and the reported persistent
hardware degradation remain for the lead, on an identified canonical image.
