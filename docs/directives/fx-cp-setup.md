# fx-cp-setup: Add/Remove Programs, Windows Setup tab and Startup Disk (CP-3, BT-9)

- **Written:** 2026-10-29 09:24 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-cp-setup`, branch `wt/fx-cp-setup` from `main`.
- **Inventory:** rows CP-3 and BT-9 in `docs/design/os-feature-inventory.md`. Check them against current `main` first.

## Required behaviour

Research the Windows 98 Add/Remove Programs applet first: the Resource Kit text under `build/rskt/` ("Add/Remove Programs", "Startup Disk", "Windows Setup") and Microsoft documentation. Record the sources in the Control Panel design document.

1. **Windows Setup tab.**
   - A component list with check boxes, sizes, a Details button for groups and a description.
   - Components are the optional parts the image already installs: games, accessories, the network client, multimedia samples, and others found in `config/cd-components.json` and the image manifest.
   - Removing a component deletes its files and its Start menu entries through the existing uninstall machinery.
   - Adding one copies the files from the Setup CD when it is present. When the CD is missing, ask for it, as Windows does.
   - Only administrators may change components; check the existing security policy census.
2. **Startup Disk tab.**
   - Create Disk writes a bootable rescue floppy containing CiukiDOS and the repair tools that the Setup CD Rescue page uses.
   - Ask for a disk in drive A:, format it when needed through the existing format path (FM-33), and show a progress bar.
   - An error leaves no half-written boot sector.
   - Administrators only.
3. Keep the existing Install/Uninstall tab unchanged.

## Tests and evidence

- Add host tests for:
  - the component list built from the manifest;
  - remove and re-add through a fake CD;
  - a refusal for a standard user;
  - the floppy image written to a host file, with its boot sector and files checked.
- Add the policy census rows for any new privileged action.
- Update the inventory rows.
- Run the Control Panel, security and desktop host tests sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Do not build, run or test the legacy floppy profile. A rescue floppy *image written by the OS* is in scope; the old floppy build is not.
- Write the report to `build/codex/fx-cp-setup-report.md`.
