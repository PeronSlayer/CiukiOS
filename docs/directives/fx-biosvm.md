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
