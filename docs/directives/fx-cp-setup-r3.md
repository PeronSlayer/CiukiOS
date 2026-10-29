# fx-cp-setup round 3: the component sources must fit the Setup CD

- **Written:** 2026-10-29 11:23 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `medium`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-cp-setup`, branch `wt/fx-cp-setup`; round 2 is committed (`7f1724c77`).
- **Input:** your round-2 report. The expanded `COMPONEN` catalogue is 1,898 files and 318,808,838 bytes. The measured CD headroom is 196,007,936 bytes. The soundfont group alone is 167,125,842 bytes.

## Lead decision

The soundfont is **not** a removable component. This reverses point 4 of round 2 for the soundfont only.
- Windows 98 Setup had no equivalent group.
- A second copy of it on the CD does not fit.
- MIDI quality must not depend on the CD being present.

Every other group from round 2 stays: DOS support, games, accessories, the network client, multimedia samples. So do their warnings, dependencies and the SYSTEM-owned SFC entries.

## Scope

1. Remove the soundfont group, its warning and its `COMPONEN` sources. The soundfont stays installed and protected as a system file.
2. Measure the catalogue again with your inventory script. It must fit the measured headroom with at least 20 MiB to spare after ISO alignment, directory and catalogue overhead (estimate them from the ISO builder's rules). Record the figures in the design document. If it still does not fit, stop and report the per-group sizes; do not compress or drop other groups on your own.
3. Update the host tests (component list, warnings and the inventory rows CP-3 and BT-9) and run the same suites as round 2, sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds; the lead builds and qualifies the ISO.
- Write the report to `build/codex/fx-cp-setup-r3-report.md`.
