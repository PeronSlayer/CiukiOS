# fx-desk-arrange: desktop Arrange Icons, Auto Arrange and system icon rename (DT-3, DT-6, DT-8)

- **Written:** 2026-10-29 09:24 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-desk-arrange`, branch `wt/fx-desk-arrange` from `main`.
- **Inventory:** the rows in `docs/design/os-feature-inventory.md`. Check each row against current `main` first.

## Required behaviour

Before coding, research the Windows 98 desktop behaviour: the Resource Kit text under `build/rskt/`, Microsoft documentation and contemporary manuals. Record the sources in the desktop design document.

1. **DT-3 and DT-6.**
   - The desktop context menu gets Arrange Icons ▸ by Name, by Type, by Size, by Date, and Auto Arrange (a check mark).
   - Each arrange key sorts the icons into the grid in that order. The system icons come first, in their fixed Windows 98 order.
   - With Auto Arrange on, icons stay packed in the grid after any add, delete, rename or drop.
   - The positions and the Auto Arrange state persist per user and survive a restart and a resolution change.
   - Line Up Icons keeps its current behaviour.
2. **DT-8.** The system icons that Windows 98 lets the user rename (My Computer, Network Neighborhood, and the others the research confirms) can be renamed with F2 and the context menu.
   - The name is stored per user and shown everywhere that icon's name appears (desktop, Files, Start menu, title bars).
   - An empty name restores the default.
   - Icons that Windows 98 does not let the user rename, such as the Recycle Bin, keep Rename disabled.

## Tests and evidence

- Add host tests in the desktop test modules: each sort key, Auto Arrange after add/delete/drop, persistence across a reload, and a rename of a system icon shown in the other views.
- Update the inventory rows and their evidence.
- Run the desktop host tests sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Do not change the Ciuki portrait or the Tango icons.
- Write the report to `build/codex/fx-desk-arrange-report.md`.
