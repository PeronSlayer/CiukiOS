# integ-fix2 — Build errors on main after the 01:52 merges

- **Phase/model:** integration, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/integ-fix2` from `main`.
- **Evidence (lead, 2026-10-23 02:17, main `5ade83dd` and later):**
  1. **Apps build.** `make CAP= apps` fails because `apps/files/app.c:1162` `job_tick` has a 5220-byte stack frame, over the 4096 limit (`-Wframe-larger-than`). This came with f2-98c (`4e7e12b6`), the event-driven native apps.
  2. **Host build.** The host suite fails to compile `tests/host/contention_budget_test.c`, which includes `dosvm_perf_irq_test.c`, which includes `src/kernel/vm/dosvm_internal.h`. `ciuki/abi.h:521–524` redefines `WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED` and `WTERMSIG` after the host `stdlib.h`. This came with f3-64 (`e97c9971`), which added `#include <ciuki/abi.h>` to `dosvm_internal.h`.

## Required

1. **Stack frame.** Move the large local buffer of `job_tick` to bounded heap or static per-window state, keeping the f2-98c event-driven behaviour and the Files tests.
2. **Header.** Make `dosvm_internal.h` include only the narrow header f3-64 needs, or split those constants into a small header. Do not weaken `abi.h` with host-specific guards.
3. **Check every combination.** Rerun `make CAP= apps` (the target compile of all apps, if it runs in your sandbox; otherwise compile the changed app with the canonical target flags), the kernel latency tests (`PYTHONPATH=tests/host python3 -m unittest tests.host.test_kernel_latency`), the DOS VM host tests and the Files and desktop tests.
4. **Scan for more of the same.** List and fix any other target app whose frame exceeds 4096 bytes after f2-98c, and any other host test hit by the new include.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Host tests one at a time with `nice -n 19`. Write `build/codex/integ-fix2-report.md`.
