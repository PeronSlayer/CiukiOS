# Directive f2-05: surfaces, channels, input and display grants, the bootstrap supervisor

- **Step:** F2. **Contracts:** `execution-abi.md` F2 extension ("Desktop
  surfaces, input and channels": syscalls 57–64 and 67; records
  `ciuki_surface_info`, `ciuki_display_info`, `ciuki_rect`,
  `ciuki_input_event`, `ciuki_message`; grant rules; process groups),
  `posix-subset.md` (console, `/dev/console` interplay), `f1-acceptance.md`
  (the F1 framebuffer presenter and input queue these objects wrap),
  `f2-acceptance.md` (`crash-isolation` with `server=standin`, `libc-smoke`
  framing of application records, `app-gate` output capture).
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh`.
- **Prerequisites on `main`:** f2-02 (processes, fd table, mmap arena),
  f2-04 (SIGPIPE hook), f1-08 (`drivers_init`, input queue with set-1 codes,
  fbdev presenter).
- **Worktree:** `wt/f2-desktop-objects`. Files: new `src/kernel/proc/surface.c`,
  `src/kernel/proc/channel.c`, `src/kernel/proc/grants.c`,
  `src/kernel/proc/syscalls_desktop.c`, `src/kernel/proc/supervisor.c`
  (PID 1 duties: reaping, bootstrapping the desktop or the stand-in
  server with the display/input grants and a separate process group,
  spawning the test program for `app-gate`/`libc-smoke` with fds 0–2 on
  null/console and framing their call-3 payloads into controller records),
  headers under `ciuki/`, `src/kernel/probes/f2_probes_desktop.c`,
  `src/kernel/core/syscall.c` (dispatch rows), `tests/host/proc/desktop_*`,
  `scripts/test/host_kernel_tests.sh`.

## What to build

1. **Surfaces**: XRGB8888 objects 1–2048 × 1–2048, stride = width×4,
   page-rounded allocation ≤ 16 MiB, zero-filled, reference-counted
   backing (mappings, queued messages and descriptions each hold a
   reference), `surface_create` (CLOEXEC fd, read/write), `surface_map`
   (whole object in the mmap arena, READ or READ|WRITE bounded by the
   description's maximum rights, no exec/private/COW), `surface_info`,
   transferred descriptions read-only; closing the fd never invalidates
   mappings; `munmap`/death release references.
2. **Channels**: `channel_pair`, bounded queues (64 per direction),
   288-byte messages with up to four attached surface fds (read-only
   references created at receive), atomic copy+dequeue, EMSGSIZE/EAGAIN/
   EPIPE rules, DONTWAIT and O_NONBLOCK, peer-closed drain then 0, no
   endpoint transfer inside messages, SIGPIPE on send to a closed peer.
3. **Grants**: display grant (`present`, `display_info`) and input grant
   (`input_read`), installed only by the supervisor into the desktop
   process, duplicable within the process, never inheritable or
   attachable (EPERM); `present` = synchronous clipped copy of a surface
   rectangle through the F1 presenter with pixel conversion; `input_read`
   drains the F1 queue into 40-byte events (set-1 codes, RESYNC on
   overflow with the button bitmap), blocking interruptibly or EAGAIN;
   ENODEV without LFB/input, EIO on quarantined sources.
4. **Supervisor (PID 1)**: created at boot by the kernel; reaps orphans;
   under a test selector spawns the probe's processes; otherwise spawns the
   desktop (`/bin/desktop`, directive f2-08) or, while it does not exist,
   the stand-in server; gives it the grants and a new process group;
   restarts nothing (a desktop death leaves the kernel console fallback
   and is counted); frames application call-3 payloads into
   `CIUKI_TEST … probe=<selected> event=DATA group=app …` records with the
   kernel-owned sequence, and captures stdout/stderr of the gate program
   into bounded buffers with a streaming digest and head/tail retention.
5. **Probe** `crash-isolation` with `server=standin` (a small ring-3 server
   payload that shares a surface and a channel with two clients; the
   victim faults in the listed ways; the survivor and the server keep PIDs,
   do 100 request/reply turns and 100 ticks of progress; 100 victim cycles
   release everything; no client maps the LFB or holds a grant), and the
   `libc-smoke` controller side (spawn `/bin/libc_smoke` when present,
   frame its records, report `not_run` when the payload is absent).

## Host tests (mandatory)

Reference counting and lifetime of surfaces across mappings, messages and
descriptions; channel queue limits, atomic receive with insufficient fd
slots, peer-closed semantics; grant authority checks (inheritance and
attachment refused); present clipping against the F1 reference renderer;
input event conversion incl. RESYNC; the supervisor's framing of
application payloads (a payload containing `CIUKI_TEST` text never
becomes a record) and its bounded capture.

## Acceptance by the lead

Diff review; host tests; kernel build with audit; F0/F1 suites unchanged;
on QEMU `crash-isolation` (`server=standin`) passes on the three profiles
and `libc-smoke` passes with the SDK program from the image. Reply with:
files, interfaces, test output, and any contract problem found.
