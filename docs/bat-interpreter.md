# BAT Interpreter — contract (OPENGEM-002-BAT)

## Purpose
Contract surface for `shell_run_batch_file()` in
[stage2/src/shell.c](../stage2/src/shell.c). This is a strict subset of
`COMMAND.COM` with documented divergences. The interpreter runs
`AUTOEXEC.BAT` on startup, user-invoked `.BAT` files via the `run`
dispatch path, and scripts invoked through `CALL` from another batch.

## Limits
| Constant | Value | Meaning |
|----------|-------|---------|
| `SHELL_BATCH_MAX_LINES` | 256 | Maximum physical lines per batch file |
| `SHELL_BATCH_MAX_LABELS` | 128 | Maximum `:label` definitions |
| `SHELL_BATCH_MAX_STEPS` | 2048 | Hard cap on line-dispatch iterations per frame |
| `SHELL_BATCH_MAX_DEPTH` | 4 | Maximum nested `CALL` depth |
| `SHELL_BATCH_ARGV_MAX` | 10 | `%0`..`%9` positional arg slots |

## Supported keywords
| Keyword | Semantics |
|---------|-----------|
| `REM <text>` | Comment; line is skipped |
| `::<text>` | Alternate comment (starts with `:`, treated as a label line) |
| `:<label>` | Label definition; target for `GOTO` |
| `@<cmd>` | Suppresses echo for this one line only (even when `ECHO ON`) |
| `ECHO OFF` / `ECHO ON` | Toggle per-frame line echo |
| `ECHO.` | Print a blank line |
| `ECHO <text>` | Print text; sets `ERRORLEVEL=0` |
| `SET NAME=VALUE` | Assign environment variable |
| `PAUSE` | Prints `Press any key to continue . . .`, blocks on keystroke |
| `SHIFT` | Shifts `%1`..`%9` down; `%0` unchanged |
| `CALL <target> [args]` | Run command/script; flow returns after completion |
| `GOTO <label>` | Jump to `:label` |
| `GOTO :EOF` / `GOTO EOF` | Terminate the current batch cleanly |
| `IF [NOT] EXIST <path> <cmd>` | Conditional based on FAT path existence |
| `IF [NOT] "A"=="B" <cmd>` | Quoted string equality after env expansion |
| `IF [NOT] ERRORLEVEL N <cmd>` | Conditional on ERRORLEVEL threshold |

## Expansion rules
`shell_env_expand_line()` handles three forms, scanned left-to-right:

1. `%%` → literal `%`.
2. `%0`..`%9` → positional arg; `%0` is the current batch file path,
   `%1`..`%9` are args passed by `CALL` (or empty when absent).
3. `%NAME%` → environment variable (`shell_env_get`).

`%ERRORLEVEL%` is not a synthetic variable; set and read
`ERRORLEVEL` explicitly when needed (the interpreter preserves
environment set/get as-is).

## Frame save/restore
Each call to `shell_run_batch_file()` saves the caller's
`(argv, argc, echo, current-path)`, installs the new frame with
`argv[0] = <path>`, runs the body, then restores. Nested `CALL` relies
on the `.BAT` dispatch in `shell_execute_line()` to recurse into
`shell_run_batch_file()`, which in turn saves/restores again.

## Serial marker catalogue
All markers are emitted through `serial_write()` only (no text console
noise). Static gates under `scripts/` grep for these markers.

| Marker | Emitted when |
|--------|--------------|
| `[ bat ] enter <path>` | Frame push, at function entry |
| `[ bat ] exit <path>` | Frame pop, at function exit |
| `[ bat ] line: <expanded>` | When echo is ON for that line |
| `[ bat ] call <target>` | Before `CALL` dispatch |
| `[ bat ] return` | After `CALL` returns |
| `[ bat ] goto <label>` | After a successful `GOTO` |
| `[ bat ] goto :eof` | `GOTO :EOF` terminated the batch |
| `[ bat ] pause` | Before blocking on a keystroke |
| `[ bat ] shift` | After a `SHIFT` |
| `[ bat ] aborted max-steps` | Step cap hit (2048) |
| `[ bat ] gem.bat reached gemvdi invocation` | A batch whose basename matches `GEM.BAT` finished without early abort |

## Not supported (documented divergences from `COMMAND.COM`)
- `FOR %v IN (…) DO …` — planned, not in scope for Phase 2.
- `CHOICE` — not implemented; use `PAUSE` + external `CHOICE.COM` if
  ever needed.
- Pipes (`|`) and redirection (`>`, `<`, `>>`) — stage2 shell has
  limited redirection support outside of batch; not forwarded.
- `COMMAND /C` — not implemented; use `CALL` instead.
- `%*` — the "all args" form is not yet implemented; iterate with
  `%1` + `SHIFT`.
- Delayed expansion (`!VAR!`) — not implemented.
- Nested quoting in `IF "a"=="b"` — only single-level `"…"` tokens.
- Console echo of the expanded line under `ECHO ON` is emitted to
  serial only. A future UX polish may enable text-console echo.

## Error handling
- `GOTO <label>` with missing label → prints an error, sets
  `ERRORLEVEL=1`, breaks the frame.
- Malformed `IF` clause → skipped defensively (treated as "condition
  not met").
- Batch file not found → sets `ERRORLEVEL=1`, frame returns.
- Step cap reached → emits `[ bat ] aborted max-steps`, sets
  `ERRORLEVEL=1`.
- Depth cap reached → prints `Batch recursion limit reached.`, sets
  `ERRORLEVEL=1`.

## Validation gate
`make test-bat-interp` runs
[scripts/test_bat_interp.sh](../scripts/test_bat_interp.sh): a
host-side, static smoke gate that asserts:
- Stage2 source exposes the keyword implementations, frame state, and
  marker strings above.
- Fixture files exist under `tests/bat/` (`minimal.bat`, `args.bat`,
  `flow.bat`, `pause-skip.bat`).
- The `test-bat-interp` Makefile target is declared.
- Opt-in boot-log probe under `.ciukios-testlogs/stage2-boot.log`
  (skipped when absent).

## Related
- [docs/roadmap-opengem-ux.md](roadmap-opengem-ux.md) — Phase 2 spec.
- [docs/opengem-runtime-structure.md](opengem-runtime-structure.md) —
  runtime paths this interpreter must handle for `GEM.BAT`.
- [docs/int21-priority-a.md](int21-priority-a.md) — DOS compatibility
  baseline.
