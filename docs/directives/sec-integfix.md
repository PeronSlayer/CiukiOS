# sec-integfix: host-suite regressions of the security integration branch

- **Written:** 2026-10-28 08:48 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`. Base: branch `wt/sec-integfix` = `wt/sec-integ4` (main + sec-wave3fix + sec-own + sec-k07 + the diag_enqueue fix ce4e9adef + sec-user-05 + sec-user-03).
- **Evidence:** the lead's verification chain on `wt/sec-integ4`, logs in `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/chain-integ4/{desktop,win32,kernelsuite}.log`. The failures below are new against main's last run (`build/lead-scratch/chain-verify2/`); main's known failures are out of scope: `test_is5boot`, `test_format0_header_compatibility`, `test_census`.

## Scope

1. **libreg transport.** sec-k06 moved the registry broker client into `sdk/libreg/transport.c`, so `registry.c` now calls `reg_remote_load`, `reg_remote_commit` and `reg_remote_close`. Every host harness that links `sdk/libreg/registry.c` must also link `transport.c`, as `tests/host/desktop/test_system_sounds.py` already does. Known failures:
   - `test_firstboot` (two link lists);
   - `test_memory_footprint` (two lists);
   - `test_event_latency`;
   - `test_g27b`.

   Find every other list across `tests/host` too, including the kernel and security suites and any volume-resolution test.
2. **gdi32 `GetEnvironmentVariableA`.** sec-user-05's per-user print queue (`sdk/win32/gdi32/printing.c` `job_root`) calls `GetEnvironmentVariableA`. Every Win32 harness that links gdi32 without kernel32 now fails to link: ddraw, dialogs, gl, gdi32, media avi, user32 production and GL lease. `tests/host/win32/gdi32/printing_test.c` has a stub.
   - Give these harnesses one shared host definition: no environment, so the machine queue `CP_ROOT` is used, as without a profile.
   - Do not add one copy per harness, and do not change production code for the tests.
3. **Diagnostics test.** `tests/host/win32/diagnostics/kernel_test.c:133` still calls `diag_enqueue(&x)`. The lead's fix ce4e9adef changed it to `diag_enqueue(struct process *owner, const struct ciuki_w32_fault *)`, because a refusal queued during `w32_spawn` comes before `proc_find` can see the process. Pass a suitable owner.
4. **Empty-drive enumeration (real behaviour, research and cite).** `test_kernel32.Kernel32Files.test_drive_service_on_real_vfs` fails at `tests/host/win32/kernel32_files.c:164`. fx-emptydrive on main requires `FindFirstFile("E:\*.*")` on a CD drive without media to return `ERROR_NOT_READY` (21), with the call succeeding and no handle; it passes on main. In this branch it no longer does.
   - Find which security change alters the `W32_F_FIRST` path (`src/kernel/proc/win32/files.c`: sec-k03's `px_resolve_as_locked` classification, `protected_path`, or the find state). Make both behaviours hold, keeping the security check.
   - Windows reports `ERROR_NOT_READY` for a removable drive without media before any access check.
   - Add the case for an administrator and a Standard user.

## Acceptance

- The desktop suite (`python3 -B -m unittest discover -s tests/host/desktop -t .`), the Win32 suite (same with `-s tests/host/win32`), `bash scripts/test/host_kernel_tests.sh` and the security suite have no failure other than main's known ones above.
- Write the before/after counts in the report.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-sec-integfix` (branch `wt/sec-integfix`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/sec-integfix-report.md` in the worktree. Leave changes uncommitted.
