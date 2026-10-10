# Directive f2-08: the ring-3 desktop (compositor, client protocol, Ciuki identity)

- **Step:** F2. **Contracts:** `execution-abi.md` F2 extension ("Desktop
  surfaces, input and channels": surface format and limits, `present`,
  `display_info`, `input_read` event layout incl. RESYNC, channel messages
  of 256 bytes with up to four surface fds, grants installed only by the
  supervisor, process groups), `posix-subset.md` (SDK, paths, console),
  `f2-acceptance.md` (`crash-isolation` with the actual desktop: PIDs kept,
  ≥100 request/reply turns, ≥100 ticks of progress after each fault,
  redraw and consumption of a post-fault key and mouse/button sequence,
  100 victim cycles releasing everything, `desktop_restarts=0`),
  `AGENTS.md` (project identity: the approved Ciuki portrait
  `assets/brand/ciuki-logo.png` converted deterministically, the boot
  splash photograph untouched, tagline `A modern Retro OS`, UI copy in
  English, Tango icons for later icon needs).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Prerequisites on `main`:** f2-05 (surfaces, channels, grants, supervisor
  spawning `/bin/desktop`), f2-06 SDK, f2-07 payload pipeline.
- **Worktree:** `wt/f2-desktop`. Files: new `apps/desktop/` (`desktop.c`
  and modules: `compositor.c`, `protocol.c`, `input.c`, `assets.c`,
  `build_desktop.py`, `README.md`), new `apps/demo/` (a small client that
  opens a window, draws, and under a flag faults in the ways the
  acceptance row lists: bad pointer, closed peer, forged fd, handler
  fault), `scripts/build_image.py` (add `/bin/desktop`, `/bin/demo` and the
  converted portrait to the payload list), `Makefile` (`desktop` target),
  `tests/host/desktop/` (protocol and compositor tests compiled natively
  against a fake surface/channel layer), `docs/desktop.md`.

## What to build

1. **Compositor**: the desktop owns the display grant; it keeps one output
   surface at display size, composites a background (solid colour plus the
   Ciuki portrait converted at build time from `assets/brand/ciuki-logo.png`
   into an XRGB8888 array by a deterministic script with a recorded hash;
   no redrawing or restyling), a top bar with the tagline `A modern Retro OS`
   and a monotonic clock, window frames (title, close button) around client
   surfaces, a software mouse cursor; damage tracking so each `present`
   covers only changed rectangles; integer-only drawing (no x87 needed, but
   x87 is allowed in user code per the SDK).
2. **Client protocol** (`ciuki/desktop.h` in the SDK overlay — add it via a
   note for the lead if the SDK path is out of scope; otherwise under
   `apps/desktop/protocol.h` shared by both programs): channel messages with
   a version byte and opcodes `HELLO`, `CREATE_WINDOW(title, w, h)` with the
   client's surface fd attached, `DAMAGE(rect)`, `MOVE`, `CLOSE`, `PING`;
   desktop → client: `CONFIGURE(w, h)`, `KEY`, `MOTION`, `BUTTON`, `FOCUS`,
   `PONG`, `CLOSED`. Every message is validated (lengths, fd counts,
   geometry against the surface info); malformed messages close that
   client's window, never the desktop. A client that stops answering `PING`
   for 5 s is marked unresponsive; its window stays until its channel
   closes.
3. **Input**: the desktop reads the input grant (blocking read in its main
   loop with a timeout), tracks focus, moves the cursor, routes key/button/
   motion to the focused client or to the frame (drag, close), releases all
   remembered keys on RESYNC, and keeps working with no client at all.
4. **Lifecycle**: spawned by the supervisor with the grants and its own
   process group; it spawns nothing by itself in F2 except when the test
   selector is active (then it spawns `/bin/demo` clients as the probe
   requests through a kernel-provided argument). A client fault closes its
   channel: the desktop drops the window, releases the surface references,
   repaints; the desktop never exits on a client error.
5. **Demo client** (`apps/demo`): opens a window, draws a pattern, answers
   PING, and with `--fault=<kind>` performs the listed faults after sharing
   its surface and while a transaction is pending; used by `crash-isolation`
   (f2-05's probe gains the `server=desktop` variant through a flag the lead
   wires in f2-09).

## Host tests (mandatory)

`tests/host/desktop/`: protocol parsing and validation (every opcode,
malformed cases), damage-rectangle merging, frame layout arithmetic,
focus/routing state machine, RESYNC handling, portrait conversion
determinism (hash of the generated array equals the recorded value), built
natively with a fake surface/channel layer.

## Acceptance by the lead

Diff review; host tests; `make build-full` adds the payloads with hashes;
on QEMU the desktop appears with the portrait and tagline, QMP mouse/key
events move the cursor and type into the demo window, and `crash-isolation`
passes with `server=desktop` on the three profiles. Reply with: files,
protocol table, payload sizes, test output, and any contract problem found.
