# F2 ring-3 desktop and client protocol

This is the f2-08 implementation handoff, not QEMU or hardware acceptance.
All authored application code is within `apps/desktop/` and `apps/demo/`;
kernel and SDK files are unchanged. The lead owns integration, the development
diary and `server=desktop` evidence for the image it actually runs.

## Build and payloads

```sh
bash scripts/build_sdk.sh --archive /path/to/newlib-4.5.0.20241231.tar.gz --jobs 2
python3 -B apps/desktop/build_desktop.py
python3 -B -m unittest discover -s tests/host/desktop -v
```

`make desktop` uses the established capped recipe; the direct commands above
are the directive's implementer exception. The SDK prerequisite in this
worktree completed in 18.113 seconds, including its mandatory host checks.
All scratch/compiler files remain inside the worktree, never in host `/tmp`.

The recipe compiles both programs through `ciuki-cc`, validates current SDK
provenance and runs the SDK ELF checker: static i386 ELF32, separate RX/R/RW
LOADs, nonexecutable GNU_STACK, no undefined symbols or dynamic/TLS segments,
no SSE/MMX. It separately audits the compositor object against the production
instruction classifier, rejecting x87 too. The compositor contains only
integer arithmetic. The libc may use the SDK's qualified x87 instructions.

`build/apps/desktop/manifest.json` records commands, source/SDK hashes, ELF
sizes/LOAD memory, conversion parameters and hashes. `scripts/build_image.py`
validates these before image creation and adds `/bin/desktop`, `/bin/demo`,
and `/system/assets/ciuki-portrait.xrgb`. The image manifest includes each
payload's size and SHA-256 plus desktop/portrait provenance, and existing T1
mtools read-back applies to all three. `make build-full` orders kernel, Lua,
desktop, then image. No full image build was run by this implementer.

## Ciuki identity and deterministic conversion

The source is the exact approved `assets/brand/ciuki-logo.png`, RGBA8,
noninterlaced, 1254x1254. Source SHA-256:

```text
17210ac9cfe07c54d6b0069a1502a036d52cb6340715ad47bdd54fa4dd386620
```

`apps/desktop/convert_portrait.py` uses Python's standard library only, checks
that source hash, PNG signature/chunk CRCs/layout, and reverses PNG filters.
For each output coordinate `(x,y)` it selects source
`(floor(x*1254/256),floor(y*1254/256))`, preserving the complete square and pose.
Each RGB channel is flattened onto the desktop colour `#375564` with
`(channel*alpha + background*(255-alpha) + 127)//255`. The output is a raw
256x256 array, 262144 bytes, little-endian B,G,R,0 (XRGB8888). Array SHA-256:

```text
1c63236be6380187c5fde01a9e91a69cadd581a68910eaf0055762a4c3e96839
```

Both hashes are pinned in the conversion script, verified by host tests, and
included in build/image manifests. There is no redraw, palette replacement,
artwork substitution or source edit. The boot photograph is untouched.
The portrait remains a project asset separate from Tango's Public Domain icons
and from the application's MIT code. F2 adds no replacement system icon family.

The compositor centres the converted portrait against a solid blue-grey
background. Its quiet dark top bar displays `CiukiOS`, the exact English
tagline `A modern Retro OS`, and MONOTONIC uptime as HH:MM:SS (hours modulo 100).
Window frames use a cream close button, a dark focused title and a muted
unfocused title. `Not responding` is the five-second heartbeat warning.
Original 5x7 bitmap glyphs render English UI and printable ASCII titles; the
demo renders incoming ASCII text, while the protocol retains Unicode scalars.

## Protocol version 1

`apps/desktop/protocol.h` is shared by both programs. SDK-overlay edits are
outside f2-08; the lead should install this exact header as `ciuki/desktop.h`
under a later directive. The authoritative syscall records remain `ciuki/abi.h`.

Every message is a `ciuki_message`. Payload header bytes: version=1 at 0,
opcode at 1, zero at 2 and 3. All integers below are little-endian; offsets
start at payload byte zero. Reserved fields, unused fd slots and payload tail
must be zero. The kernel stamps `sender_pid`; the desktop binds each endpoint
to its first sender and rejects a different PID. One channel has one client
and at most one live window; 16 channels are supported. A message in the wrong
direction, unsupported version/opcode, bad length/count, invalid title or
geometry, duplicate HELLO/CREATE, or operation before its lifecycle stage
closes only that client's endpoint/window. Every received attachment is closed
unless successfully retained as the window surface.

| Opcode | Direction | Payload bytes | Fields and fd count |
| --- | --- | ---: | --- |
| HELLO=1 | Client → desktop | 4 | Must be first; 0 fds |
| CREATE_WINDOW=2 | Client → desktop | 16+n | u32 w@4,h@8,n@12; n printable ASCII title bytes@16, 1–63; exactly 1 surface fd |
| DAMAGE=3 | Client → desktop | 20 | i32 x@4,y@8; u32 w@12,h@16 in surface coordinates; 0 fds |
| MOVE=4 | Client → desktop | 12 | i32 frame x@4,y@8, each -2048..2048, clamped to display/work area; 0 fds |
| CLOSE=5 | Client → desktop | 4 | Close this window/channel; 0 fds |
| PING=6 | Both | 8 | u32 serial@4; 0 fds |
| CONFIGURE=7 | Desktop → client | 12 | u32 actual w@4,h@8; 0 fds |
| KEY=8 | Desktop → client | 44 | Encoded 40-byte input event@4, type KEY/TEXT/RESYNC; 0 fds |
| MOTION=9 | Desktop → client | 44 | Event@4, type MOTION; value/value2 are client-relative absolute x/y; code=0; 0 fds |
| BUTTON=10 | Desktop → client | 44 | Event@4, type BUTTON; code=1/2/3, value=down/up; 0 fds |
| FOCUS=11 | Desktop → client | 8 | u32 focused@4, 0/1; 0 fds |
| PONG=12 | Both | 8 | Echo serial@4; 0 fds |
| CLOSED=13 | Desktop → client | 4 | Best-effort notification before endpoint closure; 0 fds |

CREATE geometry must equal immutable surface info: size=24, format=1, each
dimension 1–2048, stride=w*4, exact page-rounded allocation<=16 MiB. The desktop
maps attachments with PROT_READ only. DAMAGE accepts empty rectangles but must
remain within that surface; widened/clipped frame/damage arithmetic prevents
signed endpoint overflow. The output surface equals the qualified display size.

Input-event encoding at payload byte 4 follows the ABI's offsets, including
the complete u64 monotonic timestamp. KEY retains public set-1 A=0x1e, E0
positions and Pause=0x200. MOTION replaces the kernel's relative deltas with
client-local cursor coordinates, which may be negative/outside when focus
remains on a window. BUTTON retains the ABI's code/value semantics. Source,
sequence, generation and lost_count remain available. There is no resize
operation in version 1; CONFIGURE confirms the accepted dimensions.

The desktop initiates PING after one second and marks a window unresponsive
when the outstanding serial has no matching PONG for five seconds. A later
matching PONG restores its title. An unresponsive window remains until its
channel closes or an explicit CLOSE/close-button action. IPC reads and sends
are bounded per turn. A stalled peer's 64-entry outbound queue cannot block
the desktop: excess transient replies/events may be discarded; CONFIGURE and
FOCUS are retained as pending state, and a KEY/RESYNC with lost_count=1 repairs
input after the backlog drains. Clients must clear remembered input on this
RESYNC as well as on kernel RESYNC. SIGPIPE is ignored: EPIPE performs ordinary
client cleanup, as confirmed against `proc/channel.c:channel_send`.

## Focus, input, damage and lifecycle

Left-click on client content selects/raises that window. Left-click on title
begins a drag; button-up or RESYNC ends it. The close button drops that window.
Keys/text/motion go to focus; buttons pressed in content retain their recipient
until release or focus change. Focus loss synthesizes releases for remembered
keys/buttons. RESYNC releases remembered keys/buttons, records the kernel's
current button bitmap and sends the reset event; stale drag state is cleared.
With no clients, input and the software cursor/clock continue working.

Damage is clipped to display bounds, merges touching/overlapping rectangles
transitively and keeps at most 32 regions. On saturation it uses their bounding
union. Each region rebuilds background/portrait/bar, windows in stacking order
and cursor, then presents exactly that region with equal source/destination
coordinates. A failed present retains unpresented damage for retry. Frame
movement/focus/closure marks the old/new affected frames; cursor motion marks
the old/new cursor bounds. Client surfaces remain read-only and immutable in
geometry even when their owners race pixel writes.

Peer closure is drained before EOF. EOF, malformed input and EPIPE unmap the
client's complete allocation, close its surface fd and channel, clear pending
messages and repaint. Another focused window is selected. The desktop never
exits because a client failed. Process group isolation and grant noninheritance
are enforced by the kernel/supervisor, not invented by the compositor.

## Bootstrap and f2-09 wiring

Normal launch is `/bin/desktop`; current supervisor stdio uses fd 0–2 and
`grants_install` chooses the next free slots, so display/input default to 3/4.
The supervisor must pass `--display-fd=N --input-fd=N` if it reserves other
descriptors first. Repeated `--channel=N` registers explicitly inherited
endpoints. No ordinary desktop invocation spawns programs.

Only a kernel/controller-selected `--test=crash-isolation` permits repeated
`--demo=none` or `--demo=<fault-kind>`, and/or `--victim=<fault-kind>` with
`--cycles=1..100`. For example the lead can launch:

```text
desktop --test=crash-isolation --demo=none --victim=bad-pointer --cycles=100
```

The desktop creates each test pair, explicitly maps the child's endpoint to
fd 3, spawns `/bin/demo` with CIUKI_SPAWN_NEW_GROUP, and closes its own duplicate
of the child's endpoint. Sequential victims are reaped with waitpid and a
120 ms minimum interval before the next spawn. The survivor stays alive and
exchanges PING/PONG turns. Cycle output is plain application output with PID,
raw wait status, present/input/message/reply counters. The lead must capture
it through the controller, never treat it as CIUKI_TEST records, and measure
actual post-fault ticks, resource ledgers, pixel digest and external interaction.
This argument convention is available for f2-09; the kernel does not yet wire
it or spawn the actual desktop during selected probe boots.

| Demo fault | Operation after sharing and queuing PING | Expected outcome |
| --- | --- | --- |
| bad-pointer | Invalid channel-receive buffer then unmapped store | EFAULT/EAGAIN boundary followed by SIGSEGV |
| closed-peer | Create temporary pair, close peer, send; then close desktop endpoint | EPIPE (SIGPIPE ignored), EBADF on stale endpoint, exit 70 |
| forged-fd | Attach fd 63 to message | EBADF; exit 70 |
| grant-fd | Try display_info/present/input_read on own channel; attach nonsurface endpoint | EBADF; no grant acquired; exit 70 |
| handler-fault | SIGUSR1 handler stores to unmapped address | Handler-fault policy terminates process |

Every fault path waits for CONFIGURE and leaves a desktop-channel PING reply
unconsumed. A negative check with the wrong result exits 71, so the lead must
distinguish it from expected exit 70 or signal termination.

## Contract boundaries requiring lead action

1. The directive asks for blocking input with a timeout, but call 61 and the
   SDK take only `(fd,events,capacity)`. Poll/select and interval timers are
   excluded. This implementation uses O_NONBLOCK input plus interruptible
   10 ms nanosleep; an idle keyboard cannot stall channels or five-second
   liveness checks. The lead must accept this interpretation or extend the
   contract/API in a separate directive.
2. There is no named channel service, listener or endpoint-transfer operation.
   Clients need explicit launch inheritance. The existing supervisor installs
   grants but supplies no client channel, and skips desktop bootstrap on test
   boots. f2-09 must wire the selected argument/endpoint convention and actual
   desktop probe. No kernel change was made here.
3. Grants cannot be inherited or attached. `grant-fd` tests descriptor-kind
   forgery/rejection from an ordinary client; it cannot manufacture a genuine
   installed grant or claim a valid-grant EACCES case. That case remains in
   the kernel/supervisor test harness, as required by the ABI.
4. Guest acceptance still requires desktop/survivor PID and CR3 identity,
   at least 100 replies and 100 timer ticks after *every* fault, actual redraw
   plus post-fault key and mouse/button consumption, 100 cycles with kernel
   ledgers, pixel digest and desktop_restarts=0 on all three profiles. The
   120 ms user delay and host counts are not substitutes for those measurements.
5. On ENODEV from display_info the desktop reports console fallback and exits;
   the lead must run the contracted stand-in isolation as the additional
   no-LFB/safe-mode case. Runtime heap/stack commitment and memory ledgers also
   remain guest measurements. No hardware memory limit was changed.

## Research and implementation decisions

Research preceded implementation, then was checked against this repository:

- [W3C PNG specification](https://www.w3.org/TR/png/), scanline filters and
  non-premultiplied alpha: use a pinned RGBA8 decoder and explicitly rounded
  integer flattening; preserve encoded RGB samples and the approved composition.
- [Upstream Wayland surface protocol](https://wayland.freedesktop.org/docs/html/apa.html),
  surface-local damage and compositor ownership: use surface coordinates for
  DAMAGE and one private output surface. Ciuki's fixed message/grant ABI is
  authoritative; no Wayland transport, object model or buffer-commit promise
  is imported. Since Ciuki permits racing client drawing, the compositor
  promises memory safety and immutable geometry, not tear-free frames.
- `execution-abi.md`'s input/channel/grant sections and actual
  `proc/grants.c:grants_input_read`, `proc/channel.c:channel_send`,
  `proc/supervisor.c:supervisor_spawn` establish the timed-read gap, SIGPIPE
  behavior and bootstrap convention described above. The SDK wrappers and
  `apps/lua/build_lua.py` provide the existing typed-call/provenance pipeline.

The framebuffer masks, device decoding, process teardown and POSIX wrappers
were consumed as contracted interfaces, never reimplemented or changed.

## Host evidence

`tests/host/desktop/` compiles the production protocol/client/input/compositor
modules natively against a fake surface/channel layer with ASan and UBSan.
LeakSanitizer alone is disabled because it cannot run under this sandbox's
ptrace boundary; explicit fd/mapping counts return to baseline on every cycle.
Tests cover every opcode and direction, malformed lengths/counts/tails/version/
geometry, signed clipping extremes, transitive damage merges and saturation,
frame layout, focus/raise/drag/close, set-1 A/E0/Pause, TEXT and RESYNC releases,
empty desktop, backpressure, SIGPIPE/EPIPE, five-second unresponsive/recovery,
100 victim cycles with 100 survivor replies each, cleanup, incremental/full
compositor equivalence, z-order, guard words and retrying only damaged regions.
Python tests check repeat conversion/hash determinism, corrupt/source rejection,
ELF/SDK/source/output provenance and the production image payload inventory.

The final `ciuki-cc` build and inspections completed in 0.761 seconds. A repeat
build accepted the same source/output hashes without recompilation. These ELF
sizes include debug information, which remains in the image payloads:

| Payload | File bytes | Text | Data | BSS | Page-rounded LOAD bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| /bin/desktop | 505828 | 74716 | 1840 | 1636 | 86016 |
| /bin/demo | 396804 | 46872 | 1840 | 1636 | 53248 |
| /system/assets/ciuki-portrait.xrgb | 262144 | — | — | — | — |

Desktop ELF SHA-256:
`67cabe89ea3ffd21e64ca7b21acc7728a8acae28eaefab69f5f8bf1cfa3e3eab`.
Demo ELF SHA-256:
`4d13d90164cef45de30525a6f1dda3cf3fa9f0c2ab824400fde9c60ae05ca1ec`.
The three payloads total 1164776 bytes. No runtime heap/stack figure is claimed.

`python3 -B -m unittest discover -s tests/host/desktop -v` produced:

```text
PASS protocol: every opcode/direction, lengths, fds, versions, geometry, events
PASS damage/frame: clipping, signed extremes, transitive merge, saturation, coverage
PASS focus/input: set-1 A/E0/Pause, TEXT, routing, RESYNC releases, drag/close, empty desktop
PASS lifecycle: SIGPIPE/EPIPE isolation, backpressure, 5s heartbeat/recovery, 100 victim cycles, 100 replies/cycle, fd/map cleanup
PASS compositor: incremental/full equivalence, z-order, clipping, cursor, present retry/damage bounds
PASS desktop host model (guest qualification not_run)
T1 PASS: 3 payloads read back with matching sizes/SHA-256; required directories
PASS real FAT read-back: desktop, demo, portrait sizes/SHA-256
PASS portrait: pinned source/array SHA-256, repeat determinism, XRGB bytes, corrupt rejection
PASS desktop/demo ELF, SDK/source/output provenance, payload size/SHA-256 inventory
PASS stale/tampered portrait rejected before image payload acceptance
Ran 5 tests in 3.513s
OK
```

The separate existing payload regression command
`python3 -B -m unittest discover -s tests/host -p test_image_payloads.py -v`
reported `Ran 5 tests in 0.049s; OK (skipped=1)`. Its complete Lua inventory
case was skipped because this worktree has no built Lua application; its four
other tests passed. The desktop FAT fixture is a small host fixture, not a
canonical image build. Native visual inspection at 640x480 confirmed the
portrait, tagline, monotonic clock and cursor using the production compositor;
preview/scratch files were deleted afterward.

Files delivered: `apps/desktop/{desktop.c,desktop.h,client.c,protocol.c,protocol.h,
input.c,compositor.c,assets.c,convert_portrait.py,build_desktop.py,README.md}`,
`apps/demo/{demo.c,README.md}`, `tests/host/desktop/{__init__.py,fake.c,fake.h,
desktop_test.c,test_desktop.py}`, `scripts/build_image.py`, `Makefile`, and this
guide. The SDK was built as requested without changing SDK source. The
bootstrap/probe/SDK-header boundaries above remain lead-owned.

No QEMU, systemd scope, full image build, commit, push or Git command is part of
this implementer's evidence.
