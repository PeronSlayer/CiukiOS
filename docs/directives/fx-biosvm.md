# fx-biosvm: an APM BIOS call must not disable display mode setting (T23 trial 5bb73dba)

- **Written:** 2026-10-26 16:31 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main be7852468.
- **Evidence:** owner's T23 (IBM ThinkPad, SuperSavage 5333:8c2e, desktop 1024x768x32). Private serial capture in the lead's checkout: legacy/local/physical/5bb73dba/serial.log. Read it; never copy it into the tree. The same failure is already in the c42f556e trial's disk BOOT.LOG.
  - At boot the BIOS VM runs INT 15h AX=5301 (APM real-mode connect). The ThinkPad BIOS executes `out 0xED` at f000:462d, and the BIOS VM denies the port: `[biosvm] step=failure int=15 fn=5301 error=-1 reason=port_denied ... vector=13 port=00ed width=1 write=1`, `[biosvm] step=fault bytes=e6ed247f count=15 state=4`.
  - From then on the whole BIOS VM is quarantined (`[biosvm] step=already_quarantined error=-5`). Every Win32 full-screen display lease fails: Quake III asks three times for `mode=274 1024x768x32`, the same mode as the desktop, and each time gets `[display] mode=274 1024x768x32 pitch=4096 result=-5` and `present_lease=grant ... result=-5`. The machine then hangs; that hang is a separate directive, fx-q3hang.

## Scope
1. **I/O-delay ports.** Ports 0x80 and 0xED are the conventional I/O-delay ports that BIOS code writes to. Research and cite sources: Ralf Brown's port list, OSDev, the Linux `io_delay` and its 0xED option, Intel PIIX/ICH documentation for 0x80 POST and 0xED. Let the BIOS VM treat writes to them as harmless delays, with reads returning the hardware-neutral value. Keep every other port policy unchanged. State whether real hardware access or a no-op is the right choice, and why.
2. **Scoped quarantine.** A failure in one BIOS service family must not quarantine the others. APM (INT 15h 53xx) must not disable VBE (INT 10h 4Fxx) or the other families. Keep quarantine for genuinely unsafe faults within the family that caused them, and record why.
3. **No mode set when none is needed.** A present lease for the mode already on screen (same width, height, depth and pitch) must not issue a mode set at all.
4. **Native mode setting first.** When the active display driver can set modes natively (the S3 Savage provider used by Warcraft II full screen since war2fs), Win32 full-screen leases use that path before the BIOS VM. Follow the display driver model contract; generic code stays free of family names (test_display_driver_names).
5. **Tests.**
   - A BIOS VM host case replaying the ThinkPad APM connect: `out 0xED` succeeds, no quarantine.
   - APM failure followed by a successful VBE call.
   - A same-mode lease with zero mode sets.
   - A Savage-native lease.
   - The existing biosvm, display, apm/power and Win32 display suites stay green.

Report to build/codex/fx-biosvm-report.md. Leave changes uncommitted.

## Lead amendment 1 (2026-10-26 16:58): power off, and BIOS VBE first on flat panels

Your work is committed as 50e7de9fa.
Amendment for fx-biosvm after its first round (owner trial 5bb73dba, shutdown):
Power off ended with "[power] no qualified soft-off provider=apm; devices stopped, safe to turn off" and the owner saw error 95 (EOPNOTSUPP), then had to switch off by hand. APM was not qualified because the BIOS VM was quarantined by the INT 15h 5301 port 0xED denial. Verify that after the fix APM connect (5301), version (530E) and soft-off (5307 BX=0001 CX=0003) qualify on the ThinkPad path in the host replay (an ACPI S5 fallback is implemented separately in wt/fx-acpioff).

Second item, Warcraft II full screen on the T23 panel (owner photo: the LCD shows washed-out vertical colour bands, the classic look of a flat panel driven with timings it cannot display): BOOT.LOG shows `[dosvm] fullscreen_request mode=257` (101h 640x480x8) through `[display] native_supersavage state ... cr31=09 cr3a=d5 cr58=13 cr66=89 cr67=d0 cr69=00 panel=02 sr12=cb sr13=d6 sr29=06 sr15=00 sr1b=80 sr54=1f sr56=1f sr59=10 sr5b=08`, `phase=enter result=0 stage=native-mode-readback`, then `fullscreen_watchdog reason=first-frame-timeout` and return. The native SuperSavage mode set does not drive the flat panel correctly. X.Org savage (2.3.9) defaults to UseBIOS and sets modes through VBE (SavageSetVESAMode) precisely because panel/FP programming (expansion, centering, IGA routing) is done by the video BIOS. CORRECTION to scope item 4 of this directive: on a flat-panel head the mode change goes through the BIOS VBE call in the (now unquarantined) BIOS VM first, as X.Org does; a native path is used only where it is proven on that head (CRT, or panel registers fully programmed from primary sources with a readback check of the panel state). Apply the same to the Win32 full-screen lease and the DOS full screen. Add a host replay of the logged register state.

Report as 'Review round 1'. Leave changes uncommitted.

## Lead amendment 2 (2026-10-26 21:46): review round 2

Committed as aa78be8f1. A Codex xhigh review found two P2 (full text build/codex/fx-biosvm-review2.md); fix both with regressions:
1. BIOS full-screen modes use the wrong DAC precision: biosvm.c:1357 sets the VBE mode without negotiating DAC width and dosvm_fullscreen.c:279 expands 6-bit guest colours to 8 bits unconditionally, but VBE resets the DAC to 6 bits on a mode set; carry the actual DAC precision into the palette upload (or negotiate and verify 8-bit DAC through 4F08 within the budget); integrated palette regression (Warcraft II 101h).
2. A fatal full-screen video fault can disable APM: entry sets fullscreen_video=true even after a mode-set execution fault (biosvm.c:1372), cleanup (:1385) exits without clearing it once video is quarantined, and APM admission (:795) returns EBUSY forever; distinguish retained quarantined video ownership from an active transaction; test fatal entry, cleanup, then a successful APM call.
Report as "Review round 2". Leave changes uncommitted.
