# fx-dispmode: a saved 640x480 desktop mode, "Display mode unavailable (5)" after a restart, and a restart that did not happen (T23, 2026-10-27)

- **Written:** 2026-10-27 11:20 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Base: main e8b347473 or later.
- **Evidence:** owner's T23, image 6c3b7800 (main 21fa0e975). Private serial capture in the lead's checkout: `legacy/local/physical/6c3b7800/serial.log` (read it with `strings`; never copy it into tracked files). Boot 4 starts at the line `Ciuki VMM F0 build` near line 27, boot 5 at the `L:STAGE menu` near line 1240.

## Findings

1. **Boot 4** set `[display] mode=274 640x480x32 pitch=2560 result=0` just before `[bootstat] start complete boot=4`: this matches `display_start` (apps/desktop/desktop.c ~1310) applying a saved `cfg.display_mode` of 274 (640x480x32), although `desktop.cfg` held `display_mode=280` (1024x768x32) earlier the same morning. Something in the session before boot 4 persisted 274; the only saving path is `desk_display_frame_commit` (apps/desktop/client.c ~486), which is meant to save only a confirmed Display Properties change.
2. **The owner chose Restart in boot 4 and it did not restart**: boot 4 has no `[power] reboot requested` record (only a black-box replay from the previous session), and the next boot follows without a shutdown sequence.
3. **Boot 5**: `display_start` applied 274 again and the kernel refused it (`[display] mode=274 1024x768x32 pitch=4096 result=-5`; the record prints the current geometry); the desktop then showed `desk_shell_notify_error("Display mode unavailable", -5)`, which the owner saw as "I/O error (5)".

## Scope

1. Find from the code what persisted 274 (Win32 `ChangeDisplaySettings` leases, DOS full screen, the Quake III carrier lease, the restart screen, Display Properties preview/confirm) and make the saved preference change only on a Display Properties confirmation, as the comment in `desk_display_frame_commit` intends. Host regression for the path you find.
2. Windows 98 behaviour when the saved mode cannot be set at startup: research it (Windows 98 Resource Kit in build/rskt, Microsoft KB articles on invalid display settings at startup) and implement the equivalent: start in the current working mode, reset the stored preference, and say so in words the user understands, never a bare errno.
3. Why 640x480x32 fails with -5 at desktop start on the panel while it succeeded in boot 4: read `src/kernel/core/display.c` and the BIOS-VBE-first policy for flat panels (fx-biosvm amendment 1) and report the cause; fix it if it is outside `src/kernel/vm/biosvm.c` (fx-biosvm2 owns that file; describe any change needed there).
4. Why Restart in boot 4 produced no `[power] reboot requested`: trace the desktop Start > Shut Down > Restart path to the kernel power request at a non-default mode (640x480) and fix it. Host regression.
5. The desktop, display, power and Win32 display suites stay green.

Report to build/codex/fx-dispmode-report.md. Leave changes uncommitted.

## Lead note 1 (2026-10-27 11:25): the session before boot 4 was Safe Mode

The owner started that session in Safe Mode. Safe Mode runs the desktop at 640x480, and `display_start` copies the current mode into `cfg.display_mode` before returning, so the Safe Mode mode became the saved preference and the next normal boot applied it. Windows 98 Safe Mode never changes the normal-mode display settings: Safe Mode must neither save nor overwrite the display preference (nor other per-mode desktop settings that depend on it), and a normal boot keeps the user's mode. Item 1 of the scope is therefore this path; still check the others listed there.
