# f3-39 — one failed native full-screen session refuses every later one with EIO; audio lag in native full screen

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-39` from main `e7bdd513`.
- **Authority:** `src/kernel/drivers/supersavage_display.c` (poisoned policy), `src/kernel/core/display.c` (fallback result), `src/kernel/vm/dosvm_fullscreen.c` (guest-visible result), `src/kernel/vm/dosvm_sb16.c` / `src/kernel/audio/` (service cadence under native full screen), host tests, `docs/validation/f3-32-native-fullscreen.md` (amend).

## Evidence (T23, image d598bf4c, owner report 2026-10-17; code in main)

- Owner: Quake and Wolf3D "full screen → I/O error"; DOOM full screen works (sound lags when shooting; back in the window it is fine). On image a79f840c Quake full screen worked.
- `supersavage_display.c:20` `static bool poisoned;` set at `:120`, `:124` (registry claim failure) and `:190` (`if (e) poisoned=true;` after any mode error, including a restore readback mismatch that the f3-36 VBE fallback then recovers); `:87`, `:141`, `:185`, `:198` return `-EIO` while poisoned. The first session's failure therefore refuses every later full-screen request of the boot, and the guest sees an I/O error. The serial capture of this test was lost (adapter re-enumerated); the disk log will name the first mismatch (`native_supersavage phase=restore … stage=`): read it in `legacy/local/physical/d598bf4c/` when present, otherwise reason from the code.

## Required

1. Replace the per-boot poison with a bounded policy: a native entry failure or a restore mismatch that the mediated VBE fallback recovered counts one strike; native entry stays available until three strikes in a boot, each with its own `[display] native_supersavage strike=n stage=… reg=… expected=… actual=…` record; registry claim failures (resources) still refuse until the leases are released again, not for the whole boot. The guest-visible result of a refused request is the windowed continuation (the DOS window keeps running), never an I/O error to the program.
2. Identify from the disk log which readback still mismatches after f3-36 and fix it with the X.Org/savagefb sources (`build/downloads/savage/`), as f3-36 did for SR58.
3. Audio under native full screen: measure (host model + the `vm_perf_service audio_us` and native refill records) why sound lags when the game is busy; the SB16 virtual DMA advance and the native refill must keep their cadence when the VM thread owns the display; fix the scheduling (priority/quantum of the refill and VM audio service during full screen) with evidence, no new grants.
4. Host coverage: strike policy, fallback result visible to the guest as windowed, audio cadence under a busy VM; run display/VM/audio host suites, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`. Report in `build/codex/f3-39-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No register programming not in the X.Org/savagefb sources or the INF.
