# f3-58 — CiukiDOS FreeCOM shell, known commands, launch, records and idle

- **Phase/model:** F3 shell integration; Codex gpt-6.1-sol, xhigh with research.
- **Status/worktree:** complete f3-54 implementation text; lead creates wt/f3-58
  after f3-55 toolchain and f3-56/f3-57 ABI are available. Shell-independent
  work may be split by the lead in separate worktrees, with one writer each.
- **Authority:** pinned FreeCOM0.85a fork, guest shell/control/helper modules,
  narrow native launcher/record/idle integration, canonical installation and
  source/license glue, host tests and validation. No native desktop redesign.
- **Dependencies:** source-built FreeCOM0.85a choice is confirmed. The owner
  has been asked for the GPL-2.0-or-later grant for ported CiukiDOS code and
  approval of GitHub plus a per-release corresponding-source archive/hash:
  **f3-58 does not start until the lead records both answers**, as for f3-56;
  f3-55 may proceed meanwhile. Audit actual linked-runtime/SUPPL rights before
  accepting the shell; if OW runtime cannot be combined, the lead pins gcc-ia16
  on request. Use `kernel=unified`/per-game choice 2 and DFV2's late binding.
  Shell/VM power control reuses the existing scoped interface, with no new
  capability or authority and no new port/ABI meanings.
- **Size:** complete batch-capable shell port and regression, not a banner patch.

## Sources and read-first

Read f3-54 shell/feature inventory, f3-17, f3-51 with amendments/f3-51c,
src/com/dosvm_shell.asm, COMMAND_COMPAT ranges of shell.asm, record/probe and
launch/idle code, and F085a shell/cmdtable/ver/batch plus cmd/if/for/call/goto.
Verify the pinned [FreeCOM package](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/freecom.zip),
[Microsoft command processor/process reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/)
and [RBIL 1680](https://fd.lod.bz/rbil/interrup/windows/2f1680.html).
Record primary-source links and source coordinates before implementation.

## Required

1. Build a FreeCOM0.85a source fork with user identity CiukiDOS, English boot
   banner/VER/help and retained upstream credits/version/license details.
   Preserve the known prompt shape with CiukiDOS SHELL identity and the tagline. Do not change numeric DOS
   compatibility solely for branding. Install SHELL.COM and the same shell's
   COMMAND.COM entry alias deterministically, COMSPEC/PATH/environment coherent.
   Replace the old line-at-a-time wrapper with proper permanent/top shell and
   /C,/K and Ciuki /W semantics (resolve upstream switch collisions explicitly).
   Freeze shell dates/times (`F:shell/ver.c:58`, __DATE__/__TIME__) and include
   configuration/source pins. Compare independent builds under different
   compiler-observed calendar dates and times with a fixed source epoch and
   an unfrozen control, as H55-02 specifies; mtimes alone are insufficient.
   Shell/installer loading follows binding/activation after init_kernel()
   returns and before DoInstall(), never at driver INIT.
2. Port every required builtin/alias row of the Ciuki inventory: HELP/VER/ECHO,
   CLS/CLEAR, CD/CHDIR/CD.., DIR, MD/MKDIR/RD/RMDIR, TYPE/COPY/DEL/ERASE,
   REN/RENAME/MOVE, PWD, RUN, WHERE and known executable lookup. Initialize
   known PATH but allow real PATH/SET changes for installers. Preserve argument
   tails, errorlevel, COM/EXE/BAT search and cwd semantics. Do not advertise
   external network/editor utilities as implemented builtins.
3. Preserve queued REBOOT/SHUTDOWN seconds,/T,STATUS,CANCEL through the existing
   scoped shell/VM-control interface, as the lead decided. Reuse its request/
   status/cancel semantics and authorization checks; request no new authority.
   Requests cannot execute host reset/APM instructions or escape VM authority.
   Child EXIT/QUIT returns status; top interactive exit requests one clean
   VM-window close. Failures leave no suspended launch or leaked process.
4. Use FreeCOM batch semantics: environment expansion/SET/PATH/PROMPT/COMSPEC,
   positional arguments/SHIFT, CALL/nesting/return, IF EXIST/ERRORLEVEL/string,
   FOR, GOTO/labels, echo modes, redirection/append/handle dup and pipelines.
   Test quoted/spaced paths and failure propagation. Existing known behavior
   is retained; new installer functionality is tested, not inferred from source.
5. Preserve DOSREADY from actual DOS version/MCB/drive queries, RUNREC exact
   results, D48 tags/shell-ready barrier and native record encapsulation/drop
   counts. Guest bytes cannot forge CIUKI_TEST envelopes. Preserve f3-51 launch
   arm -> normal INT21/4B -> first video/return/failure protocol; no unconditional
   ready banner or screen-text success. Keep kernel/base SHA namespace and
   cached digest invalidation from f3-51c; no per-launch full-disk reread regression.
6. Reconcile upstream INT28/HLT idle and 2F1680 with f3-17 behavior. Blocking
   character input and interactive shell loops yield with IF-enabled waits,
   recheck keys after callouts, and wake on relevant keyboard/timer/device IRQs.
   Preserve background/focus budgets and busy games' execution. No polling loop
   justified only by functional input passing.
7. Audit actual shell link map. Build mini-SUPPL from its supplied sources and
   preserve LGPL/GPL terms. Do not assume Watcom's compiler license grants a
   GPL-compatible runtime exception. If linked support is not distributable
   with this GPL shell, have the lead pin the supported gcc-ia16 Linux toolchain,
   libc and applicable runtime-exception texts before accepting the alternative
   shell build. Record all linked object origins; no opaque prebuilt .LIB.
8. Retire old embedded debug commands only into documented runtime probes or
   diagnostics with consumer/reachability proof; keep the selectable old guest.
   Preserve the exact approved Ciuki portrait/splash and Tango family.

## Acceptance and named host tests

| ID | Required evidence |
| --- | --- |
| H58-01 identity/build | Two clean shell builds match across roots/mtimes and different compiler-observed wall-clock dates/times, with fixed epoch plus unfrozen __DATE__/__TIME__ control. Correct identity/credits, COMSPEC/alias files, source/license read-back and actual runtime/SUPPL link-map closure; lead-pinned gcc-ia16 fallback if OW runtime cannot be combined. |
| H58-02 commands | Every retained command/alias/lookup path, absent target, invalid args, old known grammar and cwd/return status tested against source-built shell on production CPU+VFS. |
| H58-03 batch | Synthetic installer scripts covering all batch features above, nested failure/control-C, spaces/quotes, pipes/redirection/handle inheritance and restore; no host filesystem emulator as oracle. |
| H58-04 launch/records | Exact `kernel=unified`/choice2 profile and DFV2 active after init_kernel/before DoInstall; no INIT-time shell launch. Real DOSREADY/RUNREC/D48, malformed/injected/drop records, failed EXEC, early return/input/first video; existing scoped power request/status/cancel and unauthorized requests; stop/close/relaunch baseline ledger. |
| H58-05 idle | No-key wait, lost-wakeup race, queued key, armed timer/IRQ and focus switches; IF/FLAGS preserved; idle counters progress and busy controls remain busy. |
| H58-06 regression | Existing H50-05, DPMI16/32, DOS/4GW, DJGPP paths and launcher/hash-cache tests unchanged. Lead confirms window/input/video/audio on canonical image. |

Implement `tests/host/dos/run_unified_shell.sh` as a focused production-CPU
shell/driver/native harness with deterministic scripts and injected time/input,
integrated into existing host checks. Run at nice19 one at a time: that script,
DOS unittest discovery, run_unified_binding.sh, run_unified_file.sh,
run_hook_cpu.sh, run_file_api.sh and host_kernel_tests.sh. Launcher/public ABI
changes require regenerated API docs as appropriate and `nice -n 19 make CAP= apps`.
Keep sanitizer settings as provided by scripts, no external leak-disable override.

## Evidence/limits

Write build/codex/f3-58-report.md and durable source/command/ABI/idle validation
with exact commands, every H58 status, source/link map hashes and new tests.
State which title installers are still lead-owned; synthetic success is not
all-library acceptance. Codex runs no QEMU/systemd-run/OS image build, git
mutation or diary edits. No new panic, private corpus disclosure, game patch,
global default switch, memory increase or unrelated Win16/network redesign.
