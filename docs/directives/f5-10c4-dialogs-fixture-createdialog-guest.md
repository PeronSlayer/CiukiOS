# f5-10C4 — dialogs fixture: `CreateDialogParamA` from resource 600 returns NULL in the guest

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-20 by the lead from the first full guest run of `f5:w32-dialogs` on image `b3f31c5e…` (QEMU qemu-t23 profile)
- **Worktree:** `../CiukiOS-wt-f5-10c4`, branch `wt/f5-10c4`

## Evidence

After f5-10C3 the fixture `dialogs-exercise.exe` starts in the guest (pid 6, loader warnings `newer-subsystem-version major=6` only) and exits with code 1. The probe records `case=dialog_teardown live_windows=0 live_menus=0 exit=1 errors=3 first_error=274`, every measurement case at zero (`dialog_units`, `dialog_controls`, `dialog_keyboard`, `dialog_menu`, `dialog_msgbox`, `font_bridge`, `font_budget`), `ledger objects_equal=1 processes_equal=1`, `END status=FAIL reason=fixture_exit`. `src/probes/win32_dialogs/exercise.c:274` is `CHECK(errors, dialog)` after `dialog = CreateDialogParamA(env->instance, RESOURCE(600), owner, dialog_proc, 0)`: `RegisterClassA` and the owner `CreateWindowExA` succeeded, the dialog from the resource template did not. The host harness (`tests/host/win32/dialogs`) passes the same code, so the difference is in the guest: the PE resource lookup through the real loader (`FindResource`/`LoadResource`/`SizeofResource` for `RT_DIALOG` 600 in `exercise.rc` as compiled by the fixture build), the dialog template parser on the real bytes, the dialog font creation through the w32srv font bridge (f5-10C2) in the guest, or the class/`WS_VISIBLE` path of the dialog window itself.

## Required

1. **Diagnosable fixture.** Each `CHECK` that fails prints one line to stderr `dialogs-exercise: step=<name> line=<n> last_error=<GetLastError()>` (the guest routes stderr to the serial console), and the dialog creation is split into checked sub-steps: `FindResourceA`, `LoadResource`, `LockResource`+`SizeofResource` (size and the DLGTEMPLATEEX signature), `CreateDialogIndirectParamA` on the locked bytes, and, separately, the font the template names (`CreateFontIndirect` through the bridge) with its `GetLastError`. The probe's `first_error` keeps the source line.
2. **Root cause and fix in production**, not in the fixture: whichever sub-step fails in the guest. Candidates to check against the host model: the resource section read through the kernel-side PE loader (`src/kernel/proc/win32/`), `user32` dialog template parsing of the compiled `.rc` bytes (`sdk/win32/user32/dialog*.c`), the font bridge's refusal paths (`W32R_FONT_*` 120/5/2) and `w32srv` running with the C: write gate open.
3. **Tests.** The host harness gains a case that loads the dialog from the *compiled PE's* resource section (not from in-memory template bytes) through the same loader path the guest uses, and a case for the font-bridge-unavailable path returning a typed failure with the right `GetLastError`. `tests.host.win32.dialogs`, `tests.host.win32.test_kernel_loader`, the fixture build, `make CAP= apps` once.

## Constraints

`rg`/`sed` only; no QEMU, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`; the lead runs the guest probe afterwards. Report `build/codex/f5-10c4-report.md`.
