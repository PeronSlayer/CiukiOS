# Native printing and shared spool queue (G31)

## Reference behaviour and implementation decision

Windows 98 Resource Kit, chapter 11, `wrkc11_1.htm`, sections Enhanced
Metafile Spooling, Printing Support for MS-DOS-based Applications and Printer
Drivers, describes background interpretation, RAW PostScript, universal printer
drivers and DOS port capture into the same spooler. Chapter 19,
`wrkc19_10.htm`, Deferred printing, requires persistent jobs while disconnected
and automatic background delivery after reconnect. The local licensed text was
read at `/home/peronslayer/Desktop/CiukiOS/build/rskt`; it is not redistributed.

Primary online sources:

- [Microsoft Print Spooler API](https://learn.microsoft.com/en-us/windows/win32/printdocs/print-spooler-api)
- [Microsoft SetPrinter](https://learn.microsoft.com/en-us/windows/win32/printdocs/setprinter)
- [Microsoft SetJob](https://learn.microsoft.com/en-us/windows/win32/printdocs/setjob)
- [Adobe PostScript Language Reference, third edition](https://www.adobe.com/jp/print/postscript/pdfs/PLRM.pdf)
- [HP PCL 5 Printer Language Technical Reference](https://www.hp.com/ctg/Manual/bpl13210.pdf), raster graphics chapter.

The initial repository provided direct LPT I/O and DOS INT 17h forwarding but
no shared persistent queue or native driver layer. G31 adds a native universal
rendering layer with generic text, PostScript and monochrome PCL 5. This is a
native driver model; Windows binary printer drivers remain excluded.
Native text and images render to RAW payloads before publication. This shared
queue does not claim Windows EMF interpretation: applications submit rendered
printer data, and the Win32 adapter is responsible for its GDI representation.

## Protocol and durability

`apps/libprint/print.h` is an internal shared header, not a syscall or public
SDK ABI. All disk fields are fixed-width little-endian on the i686 target.
`/WINDOWS/SPOOL/printers` contains `cp_config`, version 1, with eight printers,
the default index, driver and output port. Configuration replacement writes and fsyncs a temporary file, retains the previous
record as `.bak`, renames the temporary record into place, then removes the
backup. Readers recover a complete temporary or backup record only when the
primary record is absent and a backup proves interrupted replacement. Writers
serialize configuration updates with a `config.lock` directory containing an
`owner` native PID; dead owners are reclaimed with `kill(pid, 0)`. A crash before
the owner file is written requires manual stale-lock removal. The shared root
is the writable Windows compatibility tree so the Win32 file service preserves
its existing protection of `/System`. `cp_page` uses 1/72 inch points,
Letter/A4/custom paper, orientation, margins, copies and inclusive page range.

There are 128 slots `/WINDOWS/SPOOL/j001` through `j128`. The producer reserves a
slot using exclusive directory creation, writes `data`, closes it, then writes
`job.tmp` and renames it to `job`. The worker ignores unpublished slots. `job`
is a fixed `cp_job` record (424 bytes) containing state, progress, printer,
title, page options and optional output path. There is a 16 MiB limit per job.
A queue-full condition is reported to the producer; it never overwrites a job.
A separate `ctl` file contains `p`, `r` or `c`, so producers do not overwrite
worker progress when pausing, resuming or cancelling. Purge posts cancellation
for all matching published jobs. The sole background worker persists progress
after each 1024-byte write. Regular file output is fsynced before progress
publication and seeks to its recorded offset, so interrupted progress updates
can overwrite the same bytes on retry. Device outputs use `O_NONBLOCK` without
file creation or append flags. Completed and cancelled jobs release their slots.
Scanners (the background worker's pass, Purge, the Printers window) read the
spool folder once and visit only the `jNNN` slots it names (`cp_job_ids`,
POSIX [readdir](https://pubs.opengroup.org/onlinepubs/9799919799/functions/readdir.html);
FAT may return the short name in either case). The first G31 worker instead
opened `job` and `job.bak` in all 128 slots on every idle pass, once a second:
256 failed path walks through `/WINDOWS`. On image 56e58374 in QEMU it was the
top CPU user (median 38 % of each two-second window) and held C: for about
81 ms per lookup while DOS sessions started and closed
(`docs/validation/dos-close-hang-2026-10-24.md`). An idle pass now opens no file. Windows exposes
queue changes through notifications rather than slot probing
([FindFirstPrinterChangeNotification](https://learn.microsoft.com/en-us/windows/win32/printdocs/findfirstprinterchangenotification));
the worker is now event driven (amendment below).
The desktop starts `/System/printspool` once per session. A separate `worker.lock`
directory and owner PID prevent a desktop restart from creating a second active
service; graceful shutdown removes the lock and dead owners can be reclaimed. No GUI process
performs printer I/O. Offline/open/write failures retain jobs and retry later.
Printer-level pause prevents scheduling while job-level pause retains its
progress. A restarted worker resumes a partially delivered job.

Successful submission means the payload and published metadata have been
closed and flushed with `fsync`; durability ultimately depends on the existing
filesystem/device cache contract. Power loss between physical output and
progress persistence can repeat the last chunk: physical printing cannot be
transactional. Unpublished slots after a producer crash are deliberately
retained for diagnosis rather than silently treating partial data as a job.
No network printers, PPD/minidriver import, binary Windows printer drivers,
EMF replay or bidirectional printer-language status decoding is implied.

## Amendment 2026-10-24: event-driven worker

QEMU evidence on image 9fe0a02e (`docs/validation/doom-launch-audio-2026-10-24.md`):
the "one-second" idle poll never slept. `spool.c` asked `nanosleep` for
`tv_nsec=1000000000`, which POSIX
([nanosleep](https://pubs.opengroup.org/onlinepubs/9799919799/functions/nanosleep.html))
and the kernel (`src/kernel/proc/clock.c`, `file_sleep_deadline`) refuse with
`EINVAL` without sleeping, so the worker read its folder back to back:
60-77 % of the CPU in every two-second window, about 90 % of it in the
kernel, and C: held 50-421 ms per read while DOS games loaded. The host
replay of that loop reads the folder 209,116 times per second.

Reference: the Windows spooler starts a job when its producer schedules it,
not by polling. A producer calls `AddJob`, writes the spool file and calls
[`ScheduleJob`](https://learn.microsoft.com/en-us/windows/win32/printdocs/schedulejob);
the local print provider starts the printing thread there and scans its spool
folder only once, at start-up. The Windows 98
Resource Kit (chapter 11) describes the DOS port capture as a spooler VxD
that hands the captured output to the 32-bit spooler.

Decision:

- The worker scans once at start and then sleeps on a private wait word
  (`ciuki_wait_word`) until a producer rings it, a stop request arrives, or,
  only while a job is Offline (or the queue is unreadable), `CP_RETRY_MS`
  passes: 15 s, the Windows 9x printer "Device not selected" default.
  The doorbell bumps the word in the signal handler before the wait compares
  it, so a ring between the worker's empty scan and its sleep is never lost.
  While a job prints, the bounded 1 KiB step and 1 ms pacing are unchanged.
- Safety net (lead decision): an idle worker also reads the folder once every
  `CP_IDLE_RESCAN_MS` (60 s), one directory read and no slot opened unless a
  published slot exists, so a job from a producer path that publishes without
  ringing is printed within a minute.
- Doorbell: `SIGCHLD` to the PID in `worker.lock/owner`
  (`cp_service_notify`). Native producers ring after a submission, job
  control, printer pause/resume and configuration save (libprint); the DOS
  LPT capture rings after it publishes a job (`dosvm_print.c`); the Win32
  `MoveFileA` service rings after a rename that publishes
  `C:\WINDOWS\SPOOL\PRINTERS` or `Jnnn\JOB`/`CTL` (`fs/spool_ring.c`), which is
  how gdi32's printer DCs publish. `SIGCHLD` because its default action is to
  ignore: an owner record left by a crash or power loss names a PID that the
  next boot may give to another program.
- The worker lock reclaims a record that names the worker's own PID (left by
  an earlier boot: PIDs restart at every boot and the deterministic start
  order gives the spooler the same PID), which previously refused the lock
  with `EBUSY` after every unclean shutdown.

Host evidence: `tests/host/print_spool_wait_test.c` (in
`tests/host/test_printing.py`) runs the production `spool.c` loop over
libprint, with the intervals scaled 60:1 (rescan 1 s, retry 300 ms; the
runner checks the production values in `print.h`): idle 2 folder reads in
2.5 s (the previous loop: 209,116 per second, FAIL), a job published without
a ring printed by the next rescan (397 ms, one read), a submitted job printed
after 2-6 ms, a job published between the
worker's empty scan and its sleep printed at once, an offline job retried on
the interval (4-5 reads per second at a 300 ms test interval) and printed
when its port appeared, and a stop releases `worker.lock`.
`tests/host/dos_print_test.c` checks that a published DOS job rings the owner
once and that a missing, dead or unknown owner or a failed publication rings
nothing.

## Rendering and UI

Generic text converts line endings to CRLF, wraps within printable columns,
paginates, applies the requested page range, and ejects with form feed. Text
output is plain 7-bit printer text; unsupported bytes become `?`. PostScript
uses escaped Courier text and RGB `colorimage` with a top-down image matrix.
PCL uses reset, orientation, Letter/Legal/A4 paper selection, printable area
positioning and uncompressed monochrome raster rows at 75 dpi (most significant
bit first). Unsupported PCL custom physical paper sizes are refused; PostScript
supports custom paper. Image output
is fit to the printable area with preserved aspect ratio. Generic-text image
requests are refused rather than claiming graphics support.

Printers provides a printer list, Add Printer flow, driver and port selection,
default printer, printer pause/resume, job queue, pause/resume/cancel and purge.
Print-to-file paths and `/dev/lpt0` through `/dev/lpt2` use the same queue.
Shared Print and Page Setup dialogs are application-owned consumers of this API.

## Host validation

`tests/host/printing_test.c` checks configuration/default persistence, driver
bytes, text escaping and page selection, image raster generation, shared slot
publication, queue-full handling, pause/resume/cancel/purge, offline retry,
restart progress, partial failure handling and print-to-file completion.
Target compilation uses the binding directive's ciuki-cc warning/ABI flags.
Hardware printing, DOS integration and Win32 integration need their own
qualified evidence; host byte tests do not establish physical printer support.
