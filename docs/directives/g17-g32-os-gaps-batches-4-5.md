# OS feature gaps, batches 4–5 (G17–G32 of the fx-20 inventory)

- **Written:** 2026-10-23 23:10 by the lead (owner: continuous development; use as many agents as the host allows to finish every feature sooner). Scope, rules and compile/ABI requirements are those of `g7-g12-os-gaps-batch-2.md` (header), binding here too; each item's scope is the inventory rows and work-item text of its G row in `docs/design/os-feature-inventory.md` ("Prioritised gap list"), which is the binding specification of the item. Every item updates the inventory rows it completes and lists the rest as gaps.
- **Implementers:** Codex gpt-6.1-sol (`launch-codex.sh`) and Claude Code CLI on the second account (`launch-claude.sh`), one worktree each from `main`; model and effort chosen per item by the lead (parsimonious: Sonnet medium/high for app and content work, Opus or Codex xhigh only for kernel/V86/storage).
- **Overlaps:** G19, G20 and G23 touch the desktop shell and theme also touched by G4 (taskbar and Windows keys); start them after G4 is merged, or keep edits minimal and list the overlap. G26 touches the network stack (fx-11), G29 the FAT repair code (G14 ScanDisk), G31 the LPT driver (f1-114).

| Item | Title |
| --- | --- |
| G17 | DOS window and program properties |
| G18 | Win32 shell integration |
| G19 | Desktop icon management beyond fx-19 |
| G20 | Taskbar and Start menu customisation |
| G21 | Startup programs |
| G22 | Multimedia completion |
| G23 | Display appearance and DPI |
| G24 | Regional Settings |
| G25 | Accessibility |
| G26 | Network user tools |
| G27 | Accessories completion |
| G28 | WordPad-class editor |
| G29 | Disk tools |
| G30 | System tools |
| G31 | Printing |
| G32 | Users, logon and passwords |

## Amendment 1 — continuation items (2026-10-23 23:46, lead)

G29 part 1 (branch `wt/g29`, commit `0ee6ca2f`) delivered removable Format, Disk Cleanup and the Files low-disk warnings; the rest of G29 continues in two items, each in its own worktree branched from `wt/g29`, with the same rules, and each updating the inventory rows it completes:

- **G29b — Disk Defragmenter and Drive Converter** (rows ST-10, ST-12; Codex gpt-6.1-sol xhigh: filesystem write paths). Defragmenter for FAT12/16/32 volumes as in Windows 98: analysis with the fragmented-file percentage, the cluster map view (legend, Show Details), Start/Pause/Stop, Settings (rearrange program files so they start faster, check the drive for errors first). The volume is held exclusively or the defragmenter restarts when another program writes to it (Windows 98 "Drive contents changed... restarting"). Every move is crash-safe in this order: copy the cluster data, flush, rewrite the FAT chain in both FAT copies, flush, update the directory entry; an interruption at any point leaves a volume that the existing FAT check accepts with no lost data (host tests interrupt after every write). Drive Converter: nondestructive FAT16 to FAT32 conversion of a fixed partition (cluster size per Microsoft's FAT32 table, root directory moved into a cluster chain, boot sector and FSInfo rewritten, partition type 0Bh/0Ch), with the Windows 98 warnings (no return to FAT16, older DOS and dual-boot systems cannot read the drive), refused for removable media, the boot volume while mounted read-write or volumes too small for FAT32; conversion runs at boot or with the volume exclusively locked and is restartable after a power loss (host tests interrupt it at every stage). Primary sources: Microsoft FAT32 File System Specification (fatgen103), Windows 98 Resource Kit chapters on disk tools (`build/rskt`).
- **G29c — Copy Disk, Backup, FDISK, unformatted media** (rows FM-33 remainder, FM-34, ST-8, ST-14; Claude CLI Sonnet high). Copy Disk for floppies with a single drive (source/destination swap prompts) and two drives; floppy low-level format with bad-sector marking and Copy system files; Files lists an unformatted removable medium and offers Format; an FDISK-like partition tool (create primary, extended and logical drives, delete, set active, large-disk support prompt, display information) built on the existing Setup partition code, refusing the boot disk's mounted partitions; Backup: backup jobs (files and folders, full or differential), a backup file with a catalogue and per-file checksums, verify, restore to the original or another location with the overwrite choices, as in Windows 98 Backup. Primary sources: Windows 98 Resource Kit (`build/rskt`), Microsoft KB articles on FDISK and Backup.
