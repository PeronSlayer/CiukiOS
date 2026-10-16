# f3-36 — leaving native full screen on the SuperSavage leaves the desktop without a display

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-36` from main `bde01cc0`.
- **Authority:** `src/kernel/drivers/supersavage_display.c` (f3-32 provider: save/restore, streams, mode readback), `src/kernel/drivers/display*.c` (lease return, desktop restore, fallback to the mediated VBE path), `src/kernel/vm/dosvm_fullscreen*.c` (exit transaction), host tests, `docs/validation/` f3-32 record (amend). Sources: X.Org `savage_driver.c`/`savage_streams.c` and Linux `savagefb_driver.c` in `build/downloads/savage/`, the IBM INF `Driver/D/A0400/SSAV1014.INF`.

## Evidence (T23, image a79f840c, third boot, Quake in native full screen; `legacy/local/physical/a79f840c/serial.log` from line 6668, read-only)

Entry is complete and correct (every `fullscreen_step` result=0, `provider=supersavage-native`, `fullscreen_first_frame … source=aperture-write`; the owner reports full screen "va benissimo"). Exit:

```
[dosvm] fullscreen_watchdog vm=1 generation=2606 reason=return-or-fault
[display] fullscreen_step=watchdog-return result=0 tick=200267
[display] fullscreen_step=guest-revoke result=0 … vga-capture result=0 … desktop-restore result=0 … native-restore-blank result=0 … native-restore-panel-clock result=0 … native-restore-vga result=0
[display] fullscreen_step=native-restore-streams result=0 tick=201291
[display] native_supersavage phase=restore result=-5 stage=native-restore-streams reg=158 expected=00000000 actual=00000008 mask=000000ff restore_us=156307
[display] native_supersavage lease_return_ms=1180 budget_ms=200 result=-5
[display] mode=280 1024x768x32 pitch=4096 result=-5
[display] fullscreen_step=desktop-restored result=-5 tick=201324
[dosvm] fullscreen=0 vm=1 generation=2606 result=-5 suspended=0
[console] [display] frames=19 damage_px=14942208 presents=0 lfb_bytes=0 … completed=0 overruns=19 dropped=86   (and so on: the desktop composes, nothing is presented)
```

The owner sees the last desktop image frozen while sounds prove the system is alive. The provider's restore readback of register 0x158 (streams) expects 0x00 and reads 0x08; the whole restore is reported failed (EIO), the lease return takes 1,180 ms against a 200 ms budget, the desktop mode set fails and the compositor keeps dropping presents because it never regains the display.

## Required

1. Explain register 0x158 bit 3 against the Savage/SuperSavage documentation in `build/downloads/savage/` (streams processor control, `SSTREAM/PSTREAM` enables, or the MM8158 "pixel format" bit the X.Org driver leaves set): decide whether the readback expectation is wrong (bit retained by hardware, or set by our own panel/clock sequence) or whether the write did not land; make the restore sequence and its readback mask match the chip's documented behaviour (X.Org `SavageStreamsOff/On`, `savagefb` restore order).
2. A failed native restore must never leave the desktop without scanout: on any restore failure fall back to the mediated VBE mode set (the f3-26 path) for the desktop mode, return the lease, and resume presents; the exit transaction reports both results. The 200 ms lease-return budget is either met by the native path or the budget is adapted with evidence (`restore_us`); the watchdog path (`return-or-fault`) must take the same exit sequence as a clean guest exit.
3. Host coverage: provider restore with a retained bit, with a failed write, with the VBE fallback; exit after a watchdog return; the compositor resumes presenting after the lease returns. Run the display/VM host tests, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`. Report in `build/codex/f3-36-report.md`. The lead runs vm-fullscreen on QEMU (known std-VGA readback red, f3-29) and the owner tests the T23.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No register programming that is not in the X.Org/savagefb sources or the INF.
