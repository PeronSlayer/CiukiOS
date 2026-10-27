# fx-classaudit: audit the defect classes found on the T23 on 2026-10-27

- **Written:** 2026-10-27 12:05 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Base: main 89fe14d42 or later.
- **Owner decision 2026-10-27:** a defect found on hardware triggers an audit of its whole class, not a single fix.

## Classes (each found once on the T23 today)

1. **Validators stricter than Windows 98 and Linux.** The ISO 9660 mount refused a real CD whose volume sequence number is 0 (fixed in e8b347473; Linux isofs accepts any value). Audit every parser of external data the user brings: ISO 9660/Joliet/El Torito, FAT12/16/32 (BPB fields, FSInfo, odd geometries from period tools), NTFS read-only, PE/NE headers in the Win32 loader, INF/INI, registry files, BMP/ICO/WAV/MIDI/AVI, ZIP/CAB used by installers, AUTORUN.INF. For each refusal condition, compare with the Windows 98 behaviour (Resource Kit in build/rskt, Microsoft documentation) and the Linux/Wine implementation; a refusal that those systems do not make becomes a logged diagnostic unless it protects memory safety. List every case examined in the report.
2. **Temporary states saved as preferences.** Safe Mode's 640x480 became the saved display mode (directive fx-dispmode owns that fix). Audit every setting the desktop, Control Panel, drivers and services persist (`desktop.cfg`, registry, `System/settings/*`) for values written while in Safe Mode, a game's full-screen lease, a DOS full screen, a preview, a failed boot or a fallback driver; none may overwrite the user's normal-mode preference.
3. **Host harnesses that hide kernel limits.** The kernel heap limit is now enforced on host (fx-r3dalloc). Look for the other host substitutes whose behaviour is more permissive than the kernel: page allocator contiguity and zones, kernel stack size and frame limits, user-copy faults, timer granularity, IRQ-off sections, firmware/BIOS VM buffers (2 KiB scratch tails like the VBE state one fixed in fx-biosvm2), and fixed-size tables. Make the host substitute fail where the kernel fails, from shared definitions.

Fix what you find with host regressions. Do not touch files owned by running tasks unless the fix is a one-line change you list in the report: fx-dispmode (apps/desktop display preference, power restart path), fx-doomwin (src/kernel/vm DOS window paths), fx-q3vise (Win32 COM/installer, display lease policy), fx-is5boot (Win32 loader NE handling), fx-accept, fx-selftest.

Report to build/codex/fx-classaudit-report.md with a table per class. Leave changes uncommitted.
