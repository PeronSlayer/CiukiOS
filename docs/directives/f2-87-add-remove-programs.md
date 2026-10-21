# f2-87 — Add/Remove Programs: one list for Win32, DOS and native programs, including the bundled games; removal with confirmation

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-20 by the lead (owner's 1.0 Beta goal: a manager of installed programs that can remove them, including the freeware/shareware games bundled in the image)
- **Worktree:** `../CiukiOS-wt-f2-87`, branch `wt/f2-87`

## Model

Windows 98's Control Panel "Add/Remove Programs" (Install/Uninstall tab) as documented in the owner's Windows 98 Resource Kit (`build/rskt/` in the main checkout, read-only, never copied) and in Microsoft's documentation of the `Uninstall` registry key (`DisplayName`, `UninstallString`, `DisplayVersion`, `Publisher`, `InstallLocation`, `EstimatedSize`, `NoRemove`/`NoModify`). The existing Control Panel already lists the Win32 `Uninstall` key through the f5-07A adapter (`apps/control/control.h:62-75`, `apps/control/main.c:345-351`, `apps/desktop/win32_adapter.h`): extend that applet into the complete "Add/Remove Programs" applet; do not create a second one.

## Required

1. **One list.** Entries from: the Win32 `Uninstall` keys of HKLM and HKCU (existing adapter); the DOS games bundled on the games disk (`scripts/build_games_disk.py` packages with their `UPSTREAM.json`: name, version, publisher, size, licence file); DOS programs the user added under `C:\Games`/`C:\Programs` with a `.ciuki` or the shell's launch record; native programs added by the user (the `/Programs` entries that are not system components). Columns: name, version, publisher, size on disk; sorted by name; a system-component entry (desktop, Files, Control Panel, Terminal, the bundled DOS kernels, w32srv, fonts) is listed as protected and cannot be removed.
2. **Remove.** Confirmation dialog in English naming the program and, for bundled games, stating that the game cannot be restored without reinstalling the system. Win32: run the `UninstallString` through the existing adapter (`uninstall_run`), wait for it, then re-read the key. DOS game bundled on the games disk: delete its directory inside `C:\Games\CIUKIGAM.IMG` through the kernel's FAT code (the image is a FAT volume the DOS VM code already reads; find the existing write path or the owner-side mount used by `dosvm_disk.c`/f3-47 and use it with the C: write gate rules), invalidate the f3-51c digest cache and any per-VM copy-on-write data of that game, remove the menu entry, icon and settings; user-added DOS or native program: delete its directory and launch record. Every removal is logged (`[control] remove name=… kind=… result=…`) to the system log.
3. **Menus and launchers refresh** after a removal without a reboot: Programs/Games submenus, desktop shortcuts pointing at the removed program become invalid and are removed with the same confirmation, the DOS Box refuses to launch a removed bundled game with a clear message.
4. **Add.** The tab's "Install" button opens the Files picker on removable media and the CD-ROM for a `SETUP.EXE`/`INSTALL.EXE`/`.BAT` and launches it through the existing shell launch path (Win32 or DOS); no new installer engine.
5. **Tests.** `tests/host/desktop` Control Panel tests (list composition with a fixture registry, a fixture games manifest and fixture directories; protected entries; removal of each kind on a fixture tree and a fixture FAT image; menu refresh; the confirmation and the log line), `make CAP= apps` once, `(cd tests/host && python3 -B -m unittest test_desktop_gate)`; no image build (the lead verifies the games-disk removal in QEMU).

## Constraints

UI copy in English; Tango icons; no change to the canonical image or its games disk at build time; the bundled games' licences stay in `/system/licenses`; `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`. Report `build/codex/f2-87-report.md`.

## Amendment 1 (lead, 2026-10-21, from the Codex cross-review `build/lead-scratch/f2-87-review.md` in the main checkout: verdict "do not merge until findings 1–3 are resolved")

Apply all ten findings of the review, each with a host test, in this order; the review text with file:line references is copied below.

Ranked findings:

1. **P1 — Both image callbacks always fail their seek.**  
   [dosvm_programs.c:26](src/kernel/vm/dosvm_programs.c:26), also line 35, pass `NULL` as `vfs_seek`’s position output. [vfs.c:973](src/kernel/fs/vfs.c:973) rejects that with `EINVAL`. Consequently GAME_QUERY/GAME_REMOVE cannot mount the image; the combined inventory fails, and the new CREATE preflight blocks bundled-game launches.  
   **Smallest fix:** pass a local `uint64_t` output in both callbacks; add a behavioral test through the actual owner provider.

2. **P1 — The existing DOSVM host harness gains an unresolved dependency.**  
   [dosvm_client.c:149](src/kernel/vm/dosvm_client.c:149) now calls `dosvm_game_program`. [dosvm_test.c:520](tests/host/dosvm_test.c:520) includes this implementation, but [host_kernel_tests.sh:337](scripts/test/host_kernel_tests.sh:337) links neither its implementation nor a fixture definition. The new compile-only check cannot catch this link failure.  
   **Smallest fix:** update both DOSVM harness link paths with an appropriate backend and add syscall behavior coverage.

3. **P1 — GAME_REMOVE has no caller authority check.**  
   [dosvm_client.c:141](src/kernel/vm/dosvm_client.c:141) handles removal before checking `dosvm_capability`. Any native process with a valid writable command buffer can invoke it when C: permits writes. **The desktop is not the only caller; Control Panel calls it directly.** Direct callers also bypass confirmation, applet logging and launcher cleanup. The volume write gate is not process authorization.  
   **Smallest fix:** require an explicitly delegated management capability or broker removal through an authorized service; document that authority in the execution contract.

4. **P2 — A running FreeDOS game is discovered after destructive mutation.**  
   [dosvm_programs.c:73](src/kernel/vm/dosvm_programs.c:73) excludes users of `CIUKIGAM.IMG`, but FreeDOS game VMs use `FREEDOS.IMG` ([dosvm_client.c:237](src/kernel/vm/dosvm_client.c:237)). Their matching COW remains open. Removal can delete the bundled directory and only then fail deleting that live COW at line 92.  
   **Smallest fix:** reserve removal against creation and refuse active matching game VMs across both kernels before the first write.

5. **P2 — Cleanup cannot reliably resume after failure or power loss.**  
   [dosvm_programs.c:85](src/kernel/vm/dosvm_programs.c:85) requires the directory to exist before reaching overlay cleanup. Once deletion finishes, a failure or power cut before COW/settings/shortcut cleanup leaves an absent game that cannot pass this preflight again. [programs.c:215](apps/control/programs.c:215) then omits it from the inventory, despite the UI advising “Refresh and try again.”  
   **Smallest fix:** make cleanup idempotent when the directory is absent and retain a recoverable pending-removal identity until all cleanup completes.

6. **P2 — PROGRAMS refresh does not refresh native programs.**  
   [client.c:772](apps/desktop/client.c:772) sets reload bits and the link deadline. However, [desktop.c:1312](apps/desktop/desktop.c:1312) calls `scan_programs` only when `settings_reload_wallpaper` is set, which this request never does. Removed native executables remain in the cached Programs/Developer menu.  
   **Smallest fix:** add an explicit native-inventory refresh flag and consume it independently of wallpaper refresh.

7. **P2 — External `.ciuki` records can be launchable but absent from Add/Remove.**  
   [programs.c:145](apps/control/programs.c:145) searches the candidate directory and menu/Desktop roots, but not sibling records under `/Games` or `/Programs`. Thus `/Games/Test.ciuki` targeting `/Games/Test/GAME.EXE` is discovered by the shell yet does not identify `/Games/Test` for the applet. The fixture supplies an internal record too, masking this case.  
   **Smallest fix:** include both launch-record roots when resolving ownership; test an external-only record.

8. **P2 — Bundled-game shortcut ownership depends on an exact command substring.**  
   [programs.c:127](apps/control/programs.c:127) recognizes only the literal `/Programs/dosbox` target and searches case-sensitively for the full catalog command. A valid DOOM shortcut without `-config CIUKI.CFG`, or one targeting `C:\Programs\dosbox`, survives removal.  
   **Smallest fix:** normalize the launcher target and parse the game executable argument independently of optional switches.

9. **P2 — Inventory limits hide entries or the entire list.**  
   [win32_registry.c:115](apps/desktop/win32_registry.c:115) silently caps registry results at 32; 32 HKLM records prevent any HKCU enumeration. Separately, [programs.c:261](apps/control/programs.c:261) returns `E2BIG` when protected entries exceed the combined capacity, and [app.c:1120](apps/control/app.c:1120) clears the whole displayed inventory. The buffers are bounded safely, but overflow is unusable.  
   **Smallest fix:** preserve bounded results with explicit overflow/pagination, reserving space for protected entries.

10. **P2 — DOS installer launch lacks VM delegation.**  
    [win32_launcher.c:45](apps/desktop/win32_launcher.c:45) spawns DOS Box from `shell-launch`. That helper receives no display/input grants, while [supervisor.c:139](src/kernel/proc/supervisor.c:139) delegates DOSVM authority only from the supervisor or a parent owning both grants. The new DOS installer route therefore reaches CREATE without authority.  
    **Smallest fix:** hand DOS launch preparation back to the desktop’s existing authorized launcher.

11. **P2 — New operations disagree with the version-error contract.**  
    [dosvm_client.c:145](src/kernel/vm/dosvm_client.c:145) folds version checking into `memcmp`, returning `EINVAL` for unsupported versions; existing operations return `EOPNOTSUPP`. Keeping version 1 is otherwise reasonable: the operations are additive and the record layout remains unchanged. Reusing CIUKLNK3 also preserves its encoding.  
    **Smallest fix:** check version explicitly, document operations 11/12 and their authority in `execution-abi.md`, and register/test their constants and malformed records.

**Merge verdict: do not merge until findings 1–3 are resolved, the removal lifecycle is corrected, and the affected host suites pass.**
Lead decisions: (3) GAME_REMOVE requires the management capability delegated by the supervisor to the desktop and the Control Panel only (document it in the execution contract); (4) refuse removal while any VM of that game runs, on either DOS kernel, before the first write; (5) a pending-removal record under C:\System makes cleanup idempotent and resumable; (10) the DOS installer route goes through the desktop's existing DOS launch path so the VM delegation holds.
