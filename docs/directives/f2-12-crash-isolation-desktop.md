# Directive f2-12: `crash-isolation` with the actual ring-3 desktop (`server=desktop`)

- **Step:** F2. **Contracts:** `f2-acceptance.md` (`crash-isolation` row;
  "Desktop qualification and stretch": `server=standin` MUST NOT close the
  final F2 gate, the final run on LFB-equipped profiles and both laptops
  MUST use the actual ring-3 desktop with the approved Ciuki identity, the
  no-LFB boot runs stand-in isolation as an additional fallback case, and
  graphical/input interaction MUST be demonstrated by guest counters and
  external observation; `f2-desktop` suite row, 300 s per boot),
  `posix-subset.md` (surfaces, channels, grants, supervisor),
  `execution-abi.md` F2 extension, `test-architecture.md` and the design
  document that records the selector grammar (`rg "platform=e500" docs/design`).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-desktop-gate`, from `main` at or after `ec73de1`.
  Files: `src/kernel/probes/f2_probes_desktop.c`, `src/kernel/probes/selector.c`
  and its host test, `src/kernel/proc/supervisor.c` (a probe-facing spawn
  entry that reuses the production desktop spawn path, nothing else),
  `src/kernel/proc/grants.c` / `surface.c` / `src/kernel/drivers/fbdev*.c`
  only to expose the counters and the pixel digest the probe needs,
  `apps/demo/*` (summaries and fault kinds), `apps/desktop/*` only for the
  summaries the probe needs (never the identity assets, layout or copy),
  `tests/suites/f2-desktop.json`, `scripts/test/run.py` (post-fault input
  stimulus through QMP and screen observation), `tests/host/*`,
  `scripts/test/host_kernel_tests.sh`, one amendment each in
  `docs/design/f2-acceptance.md` and the grammar document.

## Observed on `main` (2026-10-11)

- `probe_f2_crash_isolation` runs only the embedded stand-in server
  (`CIUKI_DESKTOP_PAYLOAD_BIN`): every record says `server=standin`, the
  victims are the stand-in payload in modes 2–5, and the survivor's turns are
  read from the payload's result page.
- The normal boot already spawns `/bin/desktop` through
  `supervisor_spawn(..., desktop=true)` with the two grants, falling back to
  the stand-in when the payload is absent (`supervisor.c:111-117`).
  `apps/demo` accepts `--fault=bad-pointer|closed-peer|forged-fd|grant-fd|handler-fault`
  and answers PING.
- `tests/suites/f2-desktop.json` has nine cases (normal, no-LFB, safe on
  `qemu-t23`, `qemu-e500`, `qemu-min128`), all expecting the stand-in. The
  runner has `screendump` for `evidence_sink=screen` cases and the QMP input
  stimulus of `f1-input`.
- f2-08 planned the `server=desktop` variant "through a flag the lead wires
  in f2-09"; f2-09 did not wire it. This directive does.

## Required behaviour

1. **Server selection.** Selector grammar gains an optional key
   `server=desktop|standin` after `safe=1` (document it where the grammar is
   recorded, with a host test). Without the key the probe picks `desktop`
   when the framebuffer is ready with an LFB and `/bin/desktop` is present,
   otherwise `standin`; every record names the server actually used.
   `safe=1` and no-LFB boots keep the stand-in path unchanged.
2. **Desktop mode.** The probe spawns the real desktop through the
   production supervisor path (same payload, grants and process group as a
   normal boot), one survivor `/bin/demo` client and 100 victim `/bin/demo
   --fault=<kind>` clients cycling the five kinds. For each cycle the probe
   MUST verify what the stand-in path verifies today: distinct address
   spaces (CR3) and process groups, the expected termination signal, desktop
   and survivor PIDs unchanged, at least 100 request/reply turns for both
   and at least 100 timer ticks of progress after the fault, resource ledgers
   restored, `unauthorized_access=0`, `desktop_restarts=0`
   (`supervisor_desktop_deaths`). Define how the probe observes the survivor's
   and the desktop's turns for real processes (for example the bounded ASCII
   summary channel `libc-smoke` already uses, or a result page the demo
   publishes); do not read private process memory by address guesses.
3. **Post-fault interaction.** After the cycles the probe emits
   `event=ARM action=post_fault_input` with the current `presents`,
   `input_events` and pixel digest; the runner then injects, through QMP, a
   key press/release, a relative mouse move and a button press/release, and
   the probe waits for the desktop to consume them: `input_events` and
   `presents` MUST increase and the pixel digest MUST change, recorded as
   `case=interaction` with before/after values. The runner MUST take a
   `screendump` after the sequence and check externally that the desktop is
   on screen: the portrait region equals the deterministic conversion of the
   approved asset the desktop embeds (reuse the host portrait check) and the
   cursor or typed text changed the frame. Record the PPM digest; do not keep
   the PPM once the case passes.
4. **Suite.** `f2-desktop.json`: normal cases on `qemu-t23`, `qemu-e500`,
   `qemu-min128`, `qemu-desktop-1998` and `qemu-desktop-2002` expect
   `server=desktop` and the interaction case; no-LFB and safe cases expect
   `server=standin` as today. Each boot stays within 300 s under
   `-icount shift=1`: measure the host time of a desktop-mode boot on the
   host-side estimate and reduce per-cycle waits if needed without lowering
   the contract minimums.
5. **No identity change.** The desktop's portrait, tagline and layout are
   not touched; the image keeps 53 payloads plus whatever the suite needs.

## Host tests

Selector key parsing; probe record formatting for both servers; the demo's
summary/fault argument handling (native build like the desktop host tests);
runner: stimulus builder, screen observation (portrait region check on a
synthetic PPM, change detection), suite predicates for every new case.

## Acceptance by the lead

Host tests; kernel build; `make build-full`; on QEMU `f2-desktop` passes on
all five normal profiles with `server=desktop` and on the no-LFB/safe cases
with the stand-in; the `interaction` record shows the counters moving and the
screendump check passes. Reply with: files, the observation mechanism chosen
for turns, the record table for both servers, host-time per desktop-mode
boot, test output, and any contract problem found.
