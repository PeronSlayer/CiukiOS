# fx-cp-setup round 2: lead decisions on the Windows Setup tab and Startup Disk

- **Written:** 2026-10-29 10:44 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-cp-setup`, branch `wt/fx-cp-setup`; round 1 is committed there.
- **Input:** your round-1 report `build/codex/fx-cp-setup-report.md`, section "Decisions needed".

## Lead decisions

1. **Startup Disk is deferred until after the 1.0.0 beta.** The tab stays as round 1 left it: Create Disk disabled, with a short explanation that the Setup CD's Rescue page is the recovery medium. BT-9 stays a recorded gap in the inventory, with this decision as its note.
   - Remove the fixture-only FAT12 floppy writer from production code if nothing else uses it. A test that proves "files preserved" on a non-bootable disk must not suggest a working Startup Disk.
   - Keep the safe "sector zero last" write helper only if FM-33 format uses it.
2. **CD layout.** The `COMPONEN` tree is accepted. The lead qualifies the new ISO with the canonical build's size and hash checks. Keep the measured headroom figure in the design document.
3. **System File Checker (Windows 98 model).**
   - Windows 98 SFC offers "Update verification information" for a changed or deleted file.
   - Removing a component through Windows Setup is a deliberate change. The SYSTEM-side job that removes the files (the existing system-services path) also marks those baseline entries as `removed-component` in the SYSTEM-owned baseline.
   - Re-adding the component restores the entries.
   - SFC reports `removed-component` files neither as deleted nor as changed, and shows the component name in its details.
   - The applet never gains baseline-editing privileges. Add the policy census rows for any new privileged action.
4. **Complete the coverage.** Expose the soundfont as its own removable group, with a warning that MIDI music falls back to the OPL synthesis. Do the same for any other optional group the manifest shows that round 1 left out. Dependencies follow the manifest: removing a group another selected group needs asks first, as Windows Setup does.

## Tests

- Host tests:
  - removal and re-add update the SFC baseline through the system service;
  - SFC output after a removal;
  - a standard user refused;
  - the soundfont group with its warning;
  - the dependency prompt.
- Update the inventory rows CP-3 and BT-9.
- Run the Control Panel, security, system recovery and desktop host tests sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-cp-setup-r2-report.md`.
