# Directive f2-18: the upstream Lua run stops in `files.lua` (gate environment and file I/O coverage)

- **Step:** F2. **Contracts:** `f2-acceptance.md` (`app-gate` row: the
  unmodified official portable suite with `_U=true` MUST exit zero in the
  guest), `posix-subset.md` ("Application gate", files and departures from
  full POSIX, declared `_U`/`_port` exclusions), the f2-06/f2-07 reports
  (newlib port, Lua port), directive f2-15.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-app-gate-run`, from `main` at or after `0147f99`.
  Files: `src/kernel/proc/supervisor.c` (gate environment), `src/kernel/probes/f2_probes_app.c`
  (metadata/env records only), `sdk/libciuki/*`, `sdk/patches/*` and
  `apps/lua/*` for libc/port gaps the audit finds, the kernel file
  syscalls (`src/kernel/proc/syscalls_file.c`, `posixpath.c`, `fdtable.c`)
  only for a contract-required behaviour the audit proves missing,
  `tests/host/*`, `sdk/tests/*`, `tests/suites/f2-app.json` only if a
  record changes. Never modify the upstream test files.

## Observed on image `d3db42dd…` (commit `0147f99`, runner case `app-gate-qemu-t23`, 2026-10-11)

The probe now runs both Lua programs in the guest. The supplement passes
(`case=lua-supplement exit=0 final_ok=1 assertion_failures=0`, 127.8 s).
The upstream run executes every file of `all.lua` up to and including
`verybig.lua` (68.7 s of guest time, memory high-water 34 MiB) and fails in
the last file:

```
***** FILE 'files.lua'*****
files.lua:8: assertion failed!
	[C]: in function 'assert'
	files.lua:8: in main chunk
	all.lua:196: in main chunk
case=lua-basic pid=4 exit=256 final_ok=0 assertion_failures=1
group=wait run_case=lua-basic pid=4 status=256 timeout=0 fault_vector=0 elapsed_ms=70057
```

`files.lua:8` is `assert(type(os.getenv"PATH") == "string")`; the gate's
environment (`group=metadata name=env`) is `LC_ALL=C TZ=UTC0 HOME=/home
TMPDIR=/tmp` without `PATH`. The serial log of the run is in
`build/f2records/app-gate/serial.log` of the worktree (application output
appears as `[console]` lines).

## What to do

1. Give the gate environment a `PATH` (the image has `/bin`), record it in
   the env metadata, and keep the rest of the environment as it is.
2. Audit `files.lua` (with `_U` set and `_port` unset exactly as the gate
   runs it) against the F2 POSIX subset, the newlib port and the Lua port:
   every `io.*`/`os.*` call it makes with the flags in force (`io.open`
   modes, `io.lines` formats, `read("n")`, `seek` beyond EOF, `tmpfile`,
   `os.tmpname`, `os.remove`, `os.rename`, `io.popen` is `_port`-only,
   locale-dependent parts, `os.date`/`os.time`/`os.clock`,
   `io.stdin`/`stdout`/`stderr` behaviour with the bounded captures). For
   each call that the guest cannot satisfy today, implement the missing
   behaviour in the libc/port or kernel within the contract, or prove it is
   covered by a declared exclusion. Add host tests for every behaviour you
   implement (the native libc harness of `sdk/tests` or the kernel file
   host tests).
3. Report the remaining risk list: the calls you could not verify on the
   host, so the lead's next guest run is interpreted quickly.

## Acceptance by the lead

Host tests; kernel build; `make build-full`; on QEMU the runner case
`app-gate-qemu-t23` ends with `case=lua-basic exit=0 final_ok=1
assertion_failures=0` and `END status=PASS` within the 900 s deadline (the
upstream run needs ~70 s plus `files.lua`, the supplement ~128 s). Reply
with: files, the audit table (call → guest behaviour → fix or exclusion),
test output.
