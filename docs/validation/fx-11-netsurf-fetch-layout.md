# fx-11 — NetSurf: remote pages fail at connect; local layout spends 2.1 s opening fonts

Written 2026-10-23 (directive `docs/directives/t23-423b803a-trial-fixes.md`,
item fx-11). Evidence: T23 trial of preview `423b803a…` (private, never
committed): `legacy/local/physical/423b803a/serial.log` and
`disk/LOGS/NETSURF.LOG`.

## 1. Remote pages: the failing stage is `connect`, errno 22 (EINVAL)

Serial lines 16273–16277 (the fetch records f6-24 added go to stderr, so they
reached the serial `[console]` but not `NETSURF.LOG`):

```
[entropy] sources=3 ... health=ok reseeds=2 jitter=ready
[net] resolver first_answer ip=104.18.22.19 servers=192.168.1.254,0.0.0.0 elapsed_ms=18 generation=2
[netsurf] fetch url=https://www.w3.org/ stage=connect code=0 errno=0 ... ms=225 id=1 at=connect tls_code=0 rtc=1 utc=1792766542
[netsurf] fetch url=https://www.w3.org/ stage=error code=0 errno=22 ... ms=226 id=1 at=connect tls_code=0 rtc=1 utc=1792766542
[netsurf] fetch url=https://www.w3.org/ stage=done code=0 errno=22 ... ms=226 id=1 at=connect ...
```

DNS answered (18 ms), entropy and the qualified RTC were ready, and the fetch
failed 1 ms after entering `connect`, before any TLS byte. No `[net] tcp
connect` summary appears in the session, and the stack's `tx` counter rose by
two between the `[net] stack` lines around the fetch (11 → 13: the resolver
query and one more frame), where a TCP attempt needs a SYN plus either the
handshake ACK and request or SYN retransmissions: no SYN was sent.

Cause. `open_connection` (`sdk/netsurf/port/net.c`) makes the socket
nonblocking with the portable idiom `fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) |
O_NONBLOCK)`. For a socket, F_GETFL reports `O_RDWR` (`reserve_socket`,
`src/kernel/net/lwip/sockets.c`). `file_fcntl` (`src/kernel/proc/fdtable.c`)
rejected every F_SETFL argument with a bit outside `O_APPEND|O_NONBLOCK`, so
the call returned EINVAL and the fetch ended at `connect`. Every FCNTL call
reaches `file_fcntl` (syscall 45 is inside the file range `file_syscall`
claims first), so the same EINVAL hit desktop channels (`O_RDWR`, e.g.
`apps/ciukide/process.c:19`) and read/write files; read-only files (access
mode 0) and the desktop input grant were unaffected.

POSIX.1-2017 `fcntl()`, F_SETFL: "Bits corresponding to the file access mode
and the file creation flags, as defined in <fcntl.h>, that are set in arg
shall be ignored." (https://pubs.opengroup.org/onlinepubs/9699919799/functions/fcntl.html).
Linux F_SETFL(2const): "File access mode (O_RDONLY, O_WRONLY, O_RDWR) and file
creation flags (i.e., O_CREAT, O_EXCL, O_NOCTTY, O_TRUNC) in arg are ignored."
(https://man7.org/linux/man-pages/man2/F_SETFL.2const.html).

Why no earlier test saw it. The NetSurf network fixture
(`tests/host/netsurf/stack_fixture.c`) wraps `fcntl` and stored any value in
the socket's flags; the kernel fd tests only ever passed `O_NONBLOCK` alone
(`tests/host/lwip/fd_test.c`), and `tests/host/proc/files_test.c` asserted the
non-POSIX EINVAL for `F_SETFL, O_RDWR`. The f6-18 QEMU browser cases (the only
guest path through the real syscall) were never run; QEMU tiers were
suspended from 2026-10-19.

Decision and fix. `file_setfl_bits` (`src/kernel/include/ciuki/files.h`)
ignores the access mode and the open-only flags (O_CREAT, O_EXCL, O_TRUNC,
O_DIRECTORY, O_CLOEXEC), keeps O_APPEND/O_NONBLOCK as the settable status
flags and still fails EINVAL for any other bit; `file_fcntl` uses it and the
access mode can never change through F_SETFL. The contracts state the rule
explicitly (`docs/design/execution-abi.md` [F2] fcntl paragraph,
`docs/design/network-contract.md` socket F_SETFL sentence).

Clock. The guest reads the BIOS RTC as UTC (no time-zone handling); the
trial's records stamp 14:42:22Z while the serial capture ended at 12:51 UTC,
so the T23 RTC holds local time (CEST) and the guest clock is about 2 h
ahead. BearSSL validity checks are unaffected except for certificates that
expire within those 2 h; not a cause here.

## 2. NETSURF.LOG names the stage

Each fetch error record (stage, errno, BearSSL error, TLS category, RTC
qualification, UTC) is also appended to `C:\SYSTEM\LOGS\NETSURF.LOG`, at
most 16 per browser session, and the exit line carries `fetches=N
fetch_ok=N fetch_errors=N last_error=<stage>:<errno>:<tls>` before
`network=` (`sdk/netsurf/port/net.c`, `chrome.c`).

## 3. Local page: 2,117,000 of 2,175,000 µs were two font opens

`[netsurf] layout work elapsed_us=2175000 class=local pass=1 font_opens=2
font_open_us=2117000 font_measures=52 font_measure_us=44000`. The f6-20 and
f6-23 host models charged the welcome page 22–24 ms at the ×32 T23 scale
because they read the fonts from the host page cache through glibc, whose
`fread` reads a large request directly; f6-20 noted that its model "excludes
the T23's disk time for the first font reads".

`ciuki_font_open` (`sdk/libfont/font.c`) read the face with `fopen`/`fread`.
Ciuki's newlib is configured with `HAVE_BLKSIZE`
(`sdk/newlib/patches/0001-ciukios-configuration.patch`), so `__swhatbuf_r`
(newlib `libc/stdio/makebuf.c`) sizes the FILE buffer from `st_blksize`, and
`_fread_r` (`libc/stdio/fread.c`) refills that buffer with one `read(2)` per
block. The kernel reports the FAT cluster as `st_blksize`
(`src/kernel/proc/posixpath.c`), 8 × 512 bytes on the system volume
(`scripts/build_image.py` FAT32 geometry). LiberationSans-Regular (410,712
bytes) and -Bold (414,456 bytes) therefore cost 101 + 102 = 203 read
requests, about 10.4 ms each on the T23 (NetSurf ran at 34.6 % CPU, 26.6 % in
the kernel, in that window: `[cpu] top=… netsurf#59:346k266s4`).

Fix: `ciuki_font_open` reads the face with `open`/`fstat`/`read` in requests of
at most 1 MiB (`CIUKI_IO_MAX`), one request per face; FAT reads contiguous
clusters in 64 KiB runs (`read_span`, `BLKDEV_READ_BYTES`). The same library
serves the desktop (`apps/libdeskui/text_ttf.c`), the Control Panel, w32srv
and gdi32, which all opened faces the same way.

## Host tests

- `tests/host/lwip/fd_test.c` (`fd_run.py`): the socket idiom on the
  production `file_fcntl`; fails at line 46 on the previous code.
- `tests/host/proc/files_test.c`: POSIX F_SETFL on files, access mode never
  escalated (read-only description stays read-only, write EBADF).
- `tests/host/netsurf/test_network.py`
  `test_trial_423b803a_guest_fcntl_connect`: the fixture's fcntl applies the
  production rule; with the previous rule both WAN fetches reproduce the
  trial (`stage=connect`, `stage=error errno=22 at=connect`, `stage=done
  errno=22`); fixed, both complete. `test_fetch_stage_records_and_privacy`
  checks the NETSURF.LOG hook and the summary, and the browser failure pages
  check the NETSURF.LOG file itself (error record, exit summary).
  `compile_stack` now links the units f2-100 added to the lwIP harness (the
  module did not link on `main`), and the fixture's fcntl passes host
  descriptors to the real call (since f2-98c `apps/libdeskui/completion.h`
  sets FD_CLOEXEC on its channel pair, which aborted every browser test).
- `tests/host/font/test_font.py` `test_open_reads_face_in_one_request`: the
  guest's stdio refill pattern through `--wrap=fread`; previous code 101/102
  reads of 4096 bytes, fixed 1 read per face.

## QEMU cases (lead)

`tests/suites/f6-net.json`: `e100-http-page` and `e100-https-page`, the f6-18
browser probe on the T23 NIC model (`i82559er`, driver e100) against the
fixture page over HTTP and HTTPS; `tests/host/test_browser_runner.py` checks
their arguments and that the trial outcome (`status=0`, error page) fails.

## Lead decisions 2026-10-23 and follow-up

### Top-level document layout limit (10 s)

Lead decision 2026-10-23: the document of a top-level browser window gets a
10 s layout limit; frames, iframes, embedded objects and every other remote
span keep 75 ms (recorded in `docs/design/netsurf-port-plan.md`, "Layout time
limits", and `sdk/netsurf/port/budget.h`). Implementation: per-span limits in
`budget.c` (`ciuki_layout_begin_limit`, nested spans inherit the outer
deadline and limit) and patch
`sdk/netsurf/patches/0007-fx-11-top-level-document-limit.patch`:
`browser_window.c` flags the reformat of a window without a parent (the first
layout runs before `content_open` gives the content its window); later
reflows match the root window's current content (`layout.c`, `textplain.c`).

Host evidence (`test_network.test_top_level_document_layout_limit`, budget
clock ×20,000): the top-level page charged 5.55 s and was accepted (`layout
slow … class=remote accepted=1`); its iframe document was rejected at 77.2 ms
(`NetSurf layout rejected: remote page, time limit, elapsed_us=77200`).

While a layout runs, NetSurf neither repaints nor handles input; the desktop
keeps the last frame, can move the window and marks it "Not responding" after
an unanswered 5 s PING. The keepalive pump keeps reading the desktop channel,
now also past a full 64-packet hold ring (input dropped and counted, keys and
buttons released afterwards; control packets replace held input), so the
desktop never drops the client (it drops one that read nothing for 5 s with a
full queue). Input latency is bounded by the layout time.

### net-input worker (fx-9 analysis)

`src/kernel/drivers/net.c` `worker_main` ran `net_worker_step();
task_sleep_ms(1);` unconditionally: 1000 passes/s with three `task_yield`s
each, IRQ or not (the IRQ handler only set `irq_pending`). In trial 423b803a
`net-inpu` appears in 370 of 381 `[cpu] top` windows, median 6.6 % of the CPU
(2.6–10.1 %). It also limited reception to one frame per millisecond.

Now: the IRQ handler wakes a `kwait` (`kwait_wake_irq`), `net_stack_transmit`
and shutdown wake it from thread context (`net_worker_kick`), and a pass
returns its sleep: 0 after a received frame (back-to-back passes, a 1 ms sleep
after 16), 1 ms while transmit frames wait for ring space, 250 ms (the stack
watch cadence) with working IRQs, and a bounded fallback poll (2 ms within
100 ms of traffic, otherwise 20 ms) for devices without a working IRQ: not
attached, external (NDIS), or with frames found unsignalled by three passes in
a row (`[net] irq_fallback …`; the device record then says `mode=poll`). The
register sequence of a pass (one `receive_poll` per device, NIC unmasked
afterwards) is unchanged.

Host evidence (`tests/host/net/net_test.c` `test_worker_events`): idle with a
working IRQ, 40 passes and 80 yields in 10 s (the 1 ms loop: 10,000 and
30,000); a frame is delivered in the pass its interrupt wakes; a full e100
ring and a 20-frame RTL burst drain back to back without a missed-IRQ count;
fallback 500 passes in 10 s idle; missed interrupts switch to fallback after
three passes. At the trial's median cost per pass (about 66 µs) the idle cost
falls from about 6.6 % to about 0.03 % of the T23 CPU.
