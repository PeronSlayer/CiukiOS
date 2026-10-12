# f2-39 launch measurement and host validation

The directive is `docs/directives/f2-39-app-launch-latency.md`. Hardware
acceptance remains the lead's T23 measurement; host timings are models only.

Sources checked before implementation (2026-10-12):

- [POSIX monotonic clocks](https://pubs.opengroup.org/onlinepubs/000095399/functions/clock_getres.html):
  elapsed intervals use a clock unaffected by setting wall time. Use the existing
  PIT monotonic clock, reported in microseconds with explicit 1000 us resolution.
- [POSIX read/pread](https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html):
  positional reads may be short or interrupted. The icon reader must complete
  each bounded range, retry EINTR, and reject premature EOF.
- [ELF program loading](https://gabi.xinuos.com/elf/07-loading-intro.html):
  segment contents and zero-filled memory remain unchanged when batching reads.
- [QOI 1.0](https://qoiformat.org/qoi-specification.pdf): preserve the existing
  chunk decoder, straight alpha and checksum. No asset regeneration.

Repository inspection: `icons.c` already decodes each name/size lazily and
retains only its directory and requested pixels. Its stdio reads obscure exact
range accounting. The portrait is reread for every size. Fonts are static
arrays included by `theme.c`, with no separate font file read. `image_open` in
`fdtable.c` reads the entire ELF into immutable physical pages in 4096-byte
requests. `elf_load` then makes 512-byte **RAM snapshot** reads. `fat_read`
traverses the chain from its start for each request; `cache.c` issues one sector
per miss. ATA already supports up to 256 sectors per command in `ata_transfer`.
Larger loader reads alone cannot reduce disk command count.

Implementation decision: measure click, spawn entry, ELF completion, surface
setup, desktop CONFIGURE, first accepted DAMAGE and successful desktop present.
An additive diagnostic syscall (74, LAUNCH_MARK) lets the supervisor's desktop
mark these events without modifying the desktop wire protocol or environment.
STATUS returns whether the console is verbose. Other markers are accepted only
from that desktop and only for its directly launched children. Quiet boot
collects and emits no launch diagnostics. BEGIN supplies the launching input's
monotonic timestamp; future timestamps are rejected. The next spawn consumes
the marker, including failed spawns, so a later attempt cannot reuse it.

Per-child accounting distinguishes successful volume payload bytes/requests
(ELF snapshot plus application reads), physical cache-miss bytes/requests
(including metadata), and loader snapshot bytes/requests. The spawning task
temporarily attributes preload I/O to its provisional child. #PF traps are
counted before fault dispatch, including repairable user faults. No global
counter subtraction, which would charge unrelated processes to a launch.
The first-frame endpoint is after successful presentation, not CREATE or DAMAGE.

Keep the icon descriptor open and use exact bounded positional reads of the
directory and required entries. Preserve cache pointer lifetimes and all pinned
pixels. Read the approved portrait once and retain its four deterministic small
conversions instead of four separate full-source reads. Increase loader batches
within the existing kernel heap/stack bounds. Driver/cache read-ahead is a lead
proposal: it requires its own storage failure and hardware evidence.

The report at `build/codex/f2-39-report.md` records tests, model breakdowns and
the outstanding hardware checks. No QEMU or hardware result is asserted here.

Host validation: the kernel host script passed, including the literal i686 ABI
oracle, lifecycle/cancellation, concurrent provisional-child attribution,
64-bit marker timestamps, and native FAT/snapshot read accounting. The desktop,
icons, toolkit, resizing, CiukNote and accessories suites passed 53 tests (four
payload/provenance checks skipped because no canonical desktop build exists in
this worktree). Focused follow-up checks cover runtime console verbosity,
failed-present retry, initial resize frames, and VFS read sizes. Changed kernel
units compiled with the pinned i686 flags and frame limits; changed desktop,
toolkit and SDK wrapper units compiled against the installed native SDK's
headers without writing to that SDK. All builds/tests used `nice -n 19`.

All 246 icon names at four sizes still match the pinned decoded pixel stream.
The directory-only read is 24,524 bytes; requesting folder at 32 px adds 1,068
encoded bytes. Repeated lookups add no reads or allocations. Four portrait
sizes read 262,144 source bytes once and retain 16,272 pixel bytes plus small
headers. The host directory/folder/scaled/portrait test peaks at 324,220 bytes
on the 64-bit host. This does not read the whole Tango store at startup.

The launcher fixture explicitly models 2 MiB/s disk delivery and 6 us per
snapshot request, with synthetic 512/384/448 KiB ELF payloads. Before/after
totals for Files/CiukNote/Control Panel are 296.144/273.536 ms,
232.108/210.652 ms and 264.126/242.094 ms respectively. These are deterministic
model results, not measured executable sizes, calibrated T23 costs or hardware
acceptance. Physical disk commands remain single-sector; cache read-ahead and
an immutable-snapshot FAT chain cursor are proposals for the lead's next
measured storage change. The canonical image uses eight 512-byte sectors per
cluster (`scripts/build_image.py`), so snapshot pages align to clusters.

Merge reconciliation with f2-38/f1-24 (2026-10-12): retain the shutdown
state check before launch verbosity, timestamp consumption, channel creation
or spawn. Accepted launches keep the existing monotonic measurement path and
first-frame polling. The harness retains both shutdown and launch timing cases,
with independent clock, fstat, termination-authority and marker fakes. A
combined regression rejects a verbose launch during shutdown without consuming
its timestamp or creating descriptors, then accepts it after cancellation.
The icon reader declares its POSIX feature level before its headers so gate
host builds expose pread without changing feature levels in desktop units
that also include the native ABI and host signal definitions.
This follows the [POSIX monotonic clock contract](https://pubs.opengroup.org/onlinepubs/000095399/functions/clock_getres.html)
and the distinction between [sending a termination signal](https://pubs.opengroup.org/onlinepubs/009604499/functions/kill.html)
and observing process exit: shutdown still waits/reaps before the power request.
The automatically merged framebuffer write-combining implementation is retained.
Merge validation and remaining lead checks are recorded in
`build/codex/f2-39b-report.md`; no QEMU or hardware evidence is implied.
