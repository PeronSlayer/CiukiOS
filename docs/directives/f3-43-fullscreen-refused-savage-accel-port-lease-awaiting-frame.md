# f3-43 — the full-screen hotkey does nothing: the quarantined Savage accelerator keeps the VGA port lease, and after a refused entry the display waits 10 s for a desktop frame

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-43` from main (after `7ef68c1d`).
- **Authority:** `src/kernel/drivers/savage2d.c` (attempt marker, port claim lifecycle, quarantine), `src/kernel/drivers/supersavage_display.c` (claims), `src/kernel/core/registry.c` (quarantined leases), `src/kernel/core/display.c` (`awaiting_frame`, `display_frame_complete`, windowed continuation), `apps/desktop` compositor present/generation handshake, host tests `tests/host/display_test.c`, `supersavage_display_test.c`, `dosvm_fullscreen_test.inc`, accel tests.

## Evidence (T23, image eca4d773, `legacy/local/physical/eca4d773/BOOT1.LOG` 579–730, menu L, first boot after writing the image; read-only)

```
[dosvm] fullscreen_request vm=1 generation=23414 mode=19
[display] fullscreen_step=… request, grant-suspend, desktop-revoke, native-claim result=0
[registry] conflict: supersavage-mode wants 0:3b0-3e0 held by savage-vga reason=overlap
[display] native_supersavage phase=claim result=-22 stage=claims …
[accel] savage quarantined reason=bci_test_readback_failed us=324091599 … status=07e00000 status_valid=1
[display] mode=280 1024x768x32 … fullscreen_step=desktop-restored … fullscreen=0 continuation=windowed error=-22
[dosvm] fullscreen_request … (×3)  → [display] fullscreen_refused reason=busy switching=0 active=0 awaiting_frame=1 presenting=0
[display] native_supersavage desktop_frame_timeout        (10 s later; then the next request reaches native-claim and fails again with -22)
```

- Owner: pressing the full-screen combination does nothing (f3-39 turned the failure into a silent windowed continuation).
- `savage2d.c:930-949`: the BCI attempt is skipped only when the on-disk attempt marker says `previous_attempt`; a freshly written image has no marker, so the first boot attempts the test, fails (`:722 bci_test_readback_failed`) and `:758 registry_quarantine(port_claim)` keeps `savage-vga` (3B0–3E0) quarantined instead of released; `supersavage_display.c` then cannot claim the range (`-22`). Later boots are blocked by the marker, which is why full screen worked on second boots of earlier images.
- After the refused entry the restore sets `awaiting_frame=true` with a 10 s deadline (`display.c:475`); `display_frame_complete` (`:240`) accepts only `generation == fbdev_get()->generation`; the desktop presented (`[console] [display] frames=2 presents=2`) but the flag stayed set until `desktop_frame_timeout`, so every request in those 10 s was refused as busy.

## Required

1. A quarantined or declined accelerator never retains the VGA port range: on any failure after `port_claim` (test, activation, register claim) the driver quiesces and releases the port lease (`:806-810` path) so the display provider can claim it; if release is impossible the lease is marked for takeover and `supersavage_display_prepare` may reclaim a quarantined `savage-vga` range. Host test: accel quarantine → native entry succeeds.
2. The BCI self-test runs only when the accelerator is otherwise qualified (hardware check verdicts) and its marker is persisted **before** the test on the boot volume (so a power cut or a fresh image does not retry forever); the test must not touch 3B0–3E0 unless it needs them (document which registers it uses).
3. A refused or failed entry (windowed continuation) leaves the display immediately available: `awaiting_frame` is set only when the desktop was actually revoked and must repaint, and `display_frame_complete` accepts the desktop's next present after the restore (generation handshake: the desktop must learn the new generation through the existing query/damage path; fix whichever side is stale). Host test: refused entry → a second request within 100 ms is accepted; restore → the desktop's first present clears `awaiting_frame` within one frame.
4. Host suites: display, SuperSavage, VM full-screen, accel, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`.
5. Report `build/codex/f3-43-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No new register programming; the SuperSavage recipes of f3-36/f3-37/f3-39 stay.
