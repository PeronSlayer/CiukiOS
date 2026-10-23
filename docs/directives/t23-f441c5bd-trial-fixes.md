# T23 trial of preview image f441c5bd: seven fixes

- **Written:** 2026-10-23 21:54 by the lead, from the owner's trial of preview `f441c5bd…` (source `18dcfcb7`). Evidence (private, never committed): `legacy/local/physical/f441c5bd/serial.log` and `legacy/local/physical/f441c5bd/disk/` (`LOGS/BOOT.LOG`, `LOGS/NETSURF.LOG`, `settings/`, `Registry/`).
- **Rules:** as in `t23-e831e685-trial-fixes.md` (Windows 98 reference behaviour researched and written first; host tests that reproduce the trial's records and fail on the current code; complete fixes, gaps listed; no QEMU, no systemd-run, no image builds, no git mutations, no dev_diary edits; `date` before timestamps; English; report in `build/codex/<id>-report.md`). Before reporting, compile every touched app/SDK file with the SDK compiler (`/home/peronslayer/Desktop/CiukiOS/build/tools/ciuki-sdk/bin/ciuki-cc -O2 -Wall -Wextra -Werror …`, 4 KiB frame limit, CiukiOS errno set) and every touched kernel file with the `scripts/build_kernel.py` flags.

## fx-21 USB stick not recognised (regression)
`[usb] addr0 get_descriptor controller=usb-388 port=2 … result=-110 elapsed_ms=505` three times, then `[usb] quarantine device addr=0 reason=enumeration_transfer`; the same stick worked in the e831e685 trial. fx-17 changed UHCI bulk scheduling (transfers continue within the frame). Find what breaks control transfers at address 0, fix it without losing fx-17's throughput, and add a host test that enumerates a stick through the full UHCI schedule (control + bulk) and fails on the current code. Opus, xhigh.

## fx-22 System hang after Quake (full-screen return)
BOOT.LOG ends with `[dosvm] fullscreen_watchdog vm=1 … reason=return-or-fault cause=hotkey`, `[display] fullscreen_step=watchdog-return`, `guest-revoke`, `vga-capture` and nothing after: the return from full screen stops after VGA capture and the machine hangs (owner had to power off). Find the step that never completes (restore of the native mode, Savage engine re-enable, desktop grant, VM teardown when Quake closes during the return) and make every step bounded with a fallback so the desktop always comes back; host replay of this exact sequence. Opus, xhigh.

## fx-23 DOOM "not responding"; DOS Box busy; Files waits 19 s
`[launch-stage] name=dosbox pid=6 stage=vm-create window=DOOM` never completed; the owner saw "not responding"; `[launch] … error=-4` when closed; meanwhile `dosbox#53` used 35–47 % CPU and `storage-writer#15` 24–32 % with `device_overrun count=1271`; a Files launch waited 19.4 s for its window (`[desktop] first-frame name=files … window_ms=19396`). Find what DOS Box and the storage writer were doing (fx-4b background image check and its progress panel, fx-17b free-cluster count, Files listing waiting on a volume hold), fix the busy loop and the blocking so a DOS launch shows progress while staying responsive, and no other program waits on these background jobs. Opus, high.

## fx-24 NetSurf: CSS and images not shown, incomplete drawing, slow rendering
Owner: pages render slowly, look incomplete or badly drawn, CSS and images are not shown even though images download (`NETSURF.LOG`: 92 fetches, 90 ok; Wikipedia images fetched with 200). Audit the engine build against upstream NetSurf: which content handlers are compiled in (CSS via libcss, PNG, JPEG, GIF, BMP, ICO, SVG), whether stylesheets and images are attached to the document, the framebuffer plotters (bitmap plotting, clipping, alpha, scaling), font metrics; fix what is missing so pages look as in upstream NetSurf's framebuffer front end; profile and cut the layout time (`remote_max_us=3567000`). Lawful synthetic page fixtures with external CSS and PNG/JPEG/GIF images; host tests comparing rendered output. Opus, xhigh.

## fx-25 Battery still missing: APM installation check times out
`[apm] window ranges=9 source=fadt+chipset pm1_cnt=1004 …`, `[apm] io port=1004 … fn=5300 kind=pm1_cnt`, then `[apm] state=1 … error=-110 connected=0 reason=firmware bios_error=00`. Same implementer as fx-18. Opus, high.

## fx-26 Notifications, volume popups and window operations lag
Owner: notifications and the volume popup do not update instantly; window management still feels slow. Measure from the trial records (frame times, input-to-frame latency, popup update path, the desktop's CPU share while a DOS VM runs) and fix the causes; the volume popup and notifications update on the next frame after the event. Opus, high.

## fx-27 Quake (and DOOM) in a window are slow
Owner: Quake lags in a window; fx-16 measured DOOM spending 45 % of the CPU on VGA emulation in a window. Make windowed DOS graphics (VGA mode 13h, mode X, VESA linear) fast: dirty-region tracking through page protection or hardware-assisted change detection, direct conversion of only changed lines, no per-port emulation where the guest writes the frame buffer, presentation through the 2D engine. Opus, xhigh.
