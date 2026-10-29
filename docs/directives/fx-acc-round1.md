# fx-acc-round1: acc-t23 failures on image 7c01ad46 (items 2, 3, 6–8, 10)

- **Written:** 2026-10-29 11:01 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Evidence:** the acceptance run of 2026-10-29 on image `7c01ad465503…` (main `c18eff9cb` plus the wine-upper refresh). The summary is `/home/peronslayer/Desktop/CiukiOS/build/test-runs/acc-t23/summary.txt`. Each case directory under `build/test-runs/acc-t23/<run>/` has `serial.log`, `result.json`, `qmp.log` and screens; read the PPMs with a host tool. The case steps and predicates are in `tests/suites/acc-t23.json` and `scripts/test/acceptance*.py`.
- **Related work, do not duplicate:** fx-acc-autoplay (item 4, AutoPlay) and fx-acc-serialcap (the `[cpu]`/`[mem]`/`[sched]` record flood behind the serial log cap) are separate directives.

Each part below is its own worktree and queue entry, from `main`:

| Part | Worktree id | Report |
|---|---|---|
| A | `fx-acc-accounts` | `build/codex/fx-acc-accounts-report.md` |
| B | `fx-acc-safemode` | `build/codex/fx-acc-safemode-report.md` |
| C | `fx-acc-dosfs` | `build/codex/fx-acc-dosfs-report.md` |
| D | `fx-acc-q3hung` | `build/codex/fx-acc-q3hung-report.md` |

For each part:
1. Prove the cause with the serial evidence and a host test on the production path.
2. Fix the product, not the predicate. Change a predicate only when the product's behaviour is correct by the Windows 98/XP model and the predicate is stale; in that case justify it with the source, change the case, and keep `tests/host/test_acceptance.py` green.
3. Run the affected host suites sequentially with `nice -n 19`.

## A. Item 10: first-boot autologon without the StartUp snapshot (run `7f8ea270`)

- Step `b1-session-startup` waits 30 s for `^(?:\[console\]|\[user \d+\]) startup: snapshot ready entries=\d+$`.
- In this first-boot run, session 1 runs at line 370 (`[logon] session 1 running uid=1000 start_ms=9061`), but no StartUp snapshot record appears.
- The other cases' boots (for example `73db3513` line 1358) log `[console] startup: snapshot ready entries=0`.

Find why the StartUp folder processing does not run, or does not log, for the first-boot autologon session. Per Windows, the StartUp folders (All Users and the user's) run once per interactive logon, including the first one, after the shell is ready.

## B. Items 6–8: Safe Mode (run `80285757`)

- The Safe Mode boot reaches its desktop: `display start mode=322 640x480x32 saved=0 safe=1`, and session 1 runs at line 317.
- Step `s1-display` then times out after 300 s. The serial log keeps printing only periodic `[mem]` snapshots until the end.

Find which step and input the case sends at that point (Restart from the Safe Mode desktop, through the Shut Down dialog). Find why the system neither restarts nor shows the next display start. Check the logon session's handling of the Shut Down and Restart requests in Safe Mode under the new user-security model: Safe Mode logs on to an administrator as Windows does, and the session may end and restart.

## C. Item 3: DOS games full-screen fallback record and exit (run `bc14f763`)

- `doom-fs-auto-entered` and `doom-fs-enter` are KNOWN_FAIL under QEMU (directive f3-29: full screen refused or transport fault, continuation windowed).
- `doom-fs-auto-fallback` then waits 60 s for `^\[console\] dosbox: fullscreen auto vm=<<doom_vm>> generation=<<doom_gen>> usage=graphics`, which never appears; `dosbox` is launched at line 1110.
- `doom-exit` times out waiting for `[dosvm] lifecycle stage=cleanup … result=0`.

Find whether the record moved to a `[user N]` prefix under the session model, which would make the predicate stale (as with the accounts regex, which accepts both prefixes). Otherwise find the real regression in the fallback or exit path. The four other titles show the same pattern; explain it for each.

## D. Item 2: the VISE installer is marked not responding (run `f627080e`)

- The Quake III Arena demo installer finishes (`exit pid=10 code=00000000`), and its files and the 20 menu links are present.
- But the desktop logged 164 `not-responding pid=10 … cause=ping` lines, and assertion `installer-never-not-responding` counts 87.

On 2026-10-25 the installer ran without hangs. Find whether the ping/hung detection follows the Windows rule: a window is hung after 5 s without retrieving messages (`IsHungAppWindow`). Check also whether the desktop or w32srv stopped servicing the installer's message queue during file copies: the merged fx-w32idle wait/timer changes and the session work are candidates. Fix the cause.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds; the lead reruns the cases.
