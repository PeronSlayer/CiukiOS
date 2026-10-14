# F2-60 desktop footprint after the first frame

Research and host validation, 2026-10-14. QEMU and hardware validation belong
to the lead; this record does not qualify an image.

The [Newlib allocator documentation](https://sourceware.org/newlib/libc.html#malloc)
describes reusable freed storage and allocator growth through `sbrk`. A freed
decode buffer therefore does not prove that the process ledger returns to its
baseline. The [QOI specification](https://qoiformat.org/qoi-specification.pdf)
defines the sequential chunk decoder used by the repository's Tango reader;
the reader allocates both the decoded cache entry and temporary compressed
input on first use. Preserve that decoder and the approved assets; prepare
the bounded shell icon family before presentation.

The old allocation harness omitted `apps/libdeskui/icons.c` and the production
`d->icon` callback. With the actual reader and pinned store, a new generic
16-pixel client icon requests 1,040 bytes for its host cache record and
306 bytes for its compressed input. The gate survivor's title is `Ciuki Demo`;
fault victims are named `bad-pointer`, `closed-peer`, `forged-fd`, `grant-fd`
and `handler-fault`, selecting the
generic executable icon. Host evidence fits the reported one-page growth;
the lead must establish the guest extent/backing equality on the rebuilt image.

Disabling only icon preparation in a temporary host audit reproduces both
allocations specifically in `phase=crash_cycle`, after the `Ciuki Demo`
survivor frame. Restoring preparation gives zero allocations in that phase.

Implementation decisions:

- Prepare the static shell icon table, program/catalogue/game names and
  auxiliary popup/media names at the reader's four supported sizes during
  shell reset. Keep the existing store's negative caching and pixel output.
- Compare idle Desktop directory scans directly with the owned model. Skip
  hidden entries just as the Files browser does. Changed directories still
  use the existing refresh and layout path. A 16 KiB name index is reserved
  with the folder at init; rebuilt before each scan, it avoids comparing
  every observed name with every model entry.
- Give delegated `System\\Audio\\Master` saves a private 4 MiB static arena.
  Compile the existing SDK libreg parser/storage units with private names and
  allocator adapters, retaining generation checks, journal recovery, durable
  writes and unrelated keys. Serialize callers; reset the arena between
  transactions. Exhaustion reports `ENOMEM` through the normal unsaved result;
  never fall back to unrestricted heap growth. The normal libreg instance is
  unchanged. No kernel, USB, allocator limit or memory-floor policy changes.
- Use the production mixer poll helper in the host harness, retaining 250 ms
  scheduling and asynchronous preference saves on the settings worker.

The guard traces production desktop, Tango reader and ordinary libreg units;
fixture allocation and host libc internals are outside that call-site ledger.
It covers crash/recovery with actual titles, periodic heartbeat, clock damage,
network/media/folder service, unchanged and changed EC observations, delegated
mixer sets and durable registry preference saves, settings writes/reads,
startup records, full log mailbox, extended 84-byte FRAME and performance log,
and a missing-file wallpaper worker completion. The separate sound-worker
guard covers delayed first execution, playback and shutdown/timeouts.
Explicit new wallpaper images, directory edits and display geometry changes
remain data/geometry replacement operations, not fixed idle polling.

Measured host storage: 204 prepared icon variants, 865,532 live bytes and
871,942 peak bytes; the cache count, retained bytes and store bytes read stay
unchanged throughout the guarded paths. The master arena reserves 4,194,304
bytes in BSS. A 228,255-byte root containing 4,095 unrelated int32 values
survives three consecutive master saves with all tested values and generation
updates intact. A 2 MiB prototype failed this stress case with `ENOMEM`, so
it was not retained. Tests also cover save failure followed by recovery and
assert that preference persistence executes on the settings writer thread.

The ELF loader maps the complete segment memory span through `ua_map_at`;
`reserve_pages` allocates and zeros each backing page before execution. The
static arena therefore belongs to the startup ledger, including its physical
backing, rather than relying on delayed page commitment.

Final checks ran serially with `nice -n 19`: the desktop host suite passed
96 tests with five existing checks skipped because native application/image
outputs were absent; the gate host suite passed all ten tests. The five changed
production C units compiled with the installed i686 SDK flags, `-Werror` and
the 4 KiB frame budget; the final indexed folder unit was rechecked separately.
The final footprint/folder group passed all three tests and `git diff --check`
was clean. No QEMU, full image build, commit or diary entry was performed.

Required integration evidence remains `desktop-shell-qemu-t23` and `f1-usb`:
`processes_equal=1`, with equal extent/backing ledgers on the tested image hash.
