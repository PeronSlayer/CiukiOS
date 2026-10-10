# Directive f2-15: the `app-gate` probe (Lua 5.4.8 gate in the guest) and the `f2-app` suite

- **Step:** F2. **Contracts:** `f2-acceptance.md` (`app-gate` row: upstream
  portable suite `lua -e "_U=true" all.lua` plus the CiukiOS supplement, both
  exit zero, no failed assertion, no unexpected fault, timeout or extra skip,
  desktop/survivor alive afterwards; evidence fields: application/version,
  source/tests/SDK/ELF hashes, `case=lua-basic` and supplement case results,
  argv/cwd, `final_ok=1`, `assertion_failures=0`, wait status, declared
  exclusions, console digest, heap/stack/resident high-water, survivor
  progress; `f2-app` suite row, 900 s per boot including both runs),
  `posix-subset.md` "Application gate and graphical stretch",
  `execution-abi.md` F2 extension (call 3 bounded reports).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-app-gate`, from `main` at or after the f2-12 merge.
  Files: new `src/kernel/probes/f2_probes_app.c` (registered with
  `CIUKI_F2_PROBE("app-gate", …)` last in the F2 order), `src/kernel/proc/supervisor.c`
  (gate spawn path only), `tests/suites/f2-app.json`, `scripts/test/run.py`
  only for the application-output framing f2-09 introduced, `tests/host/*`
  (probe order and deadline tests, predicate tests with records in the shape
  the probe really emits, a host build of the probe's parsing if useful),
  `scripts/build_image.py` or `apps/lua/build_lua.py` only if the supplement's
  image path must change, `tests/host/record_scope_test.py` allow-list.
  Do not touch the other suites or probes (f2-13 and f2-14 own them).

## Observed on image `a9550f04…` (commit `3619267`, single boot on `qemu-t23`, 2026-10-11)

```
event=READY table=f2 installed=8
event=ERROR status=not_run reason=missing_probe
```

The selector accepts `f2:app-gate` and the F2 table has eight probes:
no probe was ever registered for the gate. The supervisor already has
`supervisor_spawn_gate(bool supplement, …)` (upstream: `lua -e _U=true all.lua`
in `/system/tests/lua-5.4.8-tests`; supplement: `lua ciuki-f2.lua` in the
same cwd), the observation plumbing `supervisor_observe("app-gate", pid)`
with two bounded console captures (`final_ok` scan, `assertion_failures`,
digest, tail) and the call-3 report path the `libc-smoke` probe uses. The
image carries `/bin/lua`, `/system/tests/lua-5.4.8-tests/…` and
`/system/tests/ciuki-f2.lua` (`scripts/build_image.py:76-79`), so the
supplement's spawn cwd/argv and its image path must be reconciled.

## What to do

1. Implement `probe_f2_app_gate`: start the desktop through the production
   path first (the real desktop when the LFB and `/bin/desktop` are present
   and the boot is not safe, otherwise the stand-in, with the same selection
   rule as `crash-isolation`), record its PID; run the upstream suite, then
   the supplement, each through `supervisor_spawn_gate` with
   `supervisor_observe`; wait for exit; emit the contract's evidence as
   records: application and version, hashes of the Lua ELF and of the
   test tree/supplement/SDK as the build manifest records them, argv and
   cwd, `case=lua-basic` and the supplement's cases with their results,
   wait status, `final_ok`, `assertion_failures`, declared exclusions (the
   `_U` omissions named in `posix-subset.md`), console digest and bounded
   tail, heap/stack/resident high-water of each Lua process, and the
   desktop/survivor's liveness and progress (PID unchanged, presents or
   turns advanced) after both runs. `END status=PASS` only when both runs
   exit zero with `final_ok=1`, `assertion_failures=0`, no fault and the
   desktop alive.
2. Keep every record within 240 bytes and the boot within 900 s under
   `-icount shift=1`: estimate the upstream run's duration from the
   suite's own timing and say what you expect; if the limit is at risk,
   report it instead of trimming the suite.
3. `tests/suites/f2-app.json`: three profiles, predicates written against
   the records the probe really emits (host test with records produced by
   the probe's own formatting, not fabricated by hand), deadline 900 s.
4. Host tests: probe order (`.f2probes` with `app-gate` last), selector and
   runner deadline tables, record predicates, the supplement path check.

## Acceptance by the lead

Host tests; kernel build; `make build-full`; on QEMU `f2:app-gate` ends
`END status=PASS` on `qemu-t23` within the deadline, then `f2-app` passes on
the three profiles. Reply with: files, the record table, the expected
duration of each run, the reconciled supplement path, test output, any
contract problem found.
