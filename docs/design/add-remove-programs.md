# Add/Remove Programs (f2-87)

The existing Control Panel applet owns one sorted list and confirmation. It
combines the copied HKLM/HKCU registry adapter, bundled game provenance already
installed in `/system/licenses/*-UPSTREAM.json`, and user program directories.
System components remain visible and protected. No installer engine is added.

Research (rechecked 2026-10-21):

- Microsoft, [Uninstall registry key](https://learn.microsoft.com/en-us/windows/win32/msi/uninstall-registry-key):
  DisplayName, DisplayVersion, Publisher, InstallLocation, EstimatedSize and
  UninstallString are installer-owned metadata. Re-read after the process exits;
  never delete a registry entry to pretend an uninstaller succeeded.
- Microsoft, [ARPNOREMOVE](https://learn.microsoft.com/en-us/windows/win32/msi/arpnoremove)
  and [ARPNOMODIFY](https://learn.microsoft.com/en-us/windows/win32/msi/arpnomodify):
  NoRemove prohibits removal, including when UninstallString is absent.
  NoModify alone does not prohibit removal. Protected entries remain visible
  as required by the directive, rather than using older Windows' hiding behavior.
- Windows 98 Resource Kit, chapter 25, `wrkc25_2.htm`, Installing Applications
  Locally / Using Add/Remove Programs with Win32-based Applications, read in
  place from the lead checkout's `build/rskt/`: Add/Remove finds Setup/Install
  on selected drives; DOS applications normally run their own Setup.exe.
  `wrkc31_10.htm` describes stale entries after manually deleting a program.
  The licensed book is not copied or redistributed.

The bundled image is a logical FAT volume. Removal must use the existing kernel
FAT driver with exclusive access to the backing file, retaining the C: write
gate and refusing while a VM holds its read/deny-write handle. All errors,
including failed durability barriers and partial cleanup, must be reported.
Licences remain in /system/licenses. Shortcut cleanup matches path components,
not display-name substrings, and is covered by the same confirmation.


Implementation decisions (f2-87 continuation, 2026-10-21):

- Keep the existing applet and copied registry adapter. Read both hives;
  preserve the adapter's 32-record ceiling and use a 96-entry combined list.
  The applet sorts names case-insensitively. Win32 size is the installer-owned
  EstimatedSize in KB; directory/native sizes come from allocated stat blocks,
  and bundled sizes include the FAT clusters of files and directories.
- A user `.ciuki` file uses the shell's existing CIUKLNK3 record format. There
  was no separate `.ciuki` launch schema in the repository. Discover these
  records and shell `.lnk` records for `/Games` and `/Programs`; keep the
  executable, arguments and working directory in that established format.
- Protected native identities, DOS kernels, w32srv and fonts are visible.
  NoRemove and SystemComponent prohibit removal; NoModify alone permits it.
  The confirmation names the selected application and includes shortcut
  removal. Bundled-game confirmation explains the system-reinstall requirement.
- GAME_QUERY (12) and GAME_REMOVE (13) extend DOSVM version 1 with fixed game
  IDs only. They grant no VM/display capability. A single owner mount uses the
  existing VFS sharing and C: write authority, a logical block provider, and
  the production FAT driver. Traversal is bounded to depth 16 and 16,384
  entries, with a read-only preflight before removal and FAT ownership
  validation before write enable. Queries do not enable writes. Kernel and
  SDK compiler frame limits remain unchanged.

  Amendment 3 keeps f3-52 WAIT_PRESENT at operation 11 and assigns management
  operations 12/13. The kernel header is also the SDK's public ABI source;
  the layout dump discovers constants from it, while the independent host
  oracle fixes their expected values. Registry metadata and removal semantics
  above are unchanged by this numbering correction.
- Remove clears the digest cache after attempted FAT mutation and deletes
  matching game/kernel COW files. A live backing-file read/deny-write handle
  prevents removal. Preserve every installed licence/provenance file. Report
  I/O, sharing, bounds and partial-cleanup failures instead of claiming success.
- The Win32 adapter re-resolves protection and UninstallString. Its existing
  launch helper can report the child's exit status before returning; the
  adapter waits for that result and the applet re-reads registry metadata.
  A surviving key is an incomplete removal, not a fabricated registry deletion.
- Shortcut cleanup compares normalized path components, including DOS C:
  spellings. It checks shell Programs, both Desktop locations, and external
  `.ciuki` records under Games/Programs. Native FAT has no symlink ABI; host
  fixtures reject symlinks during tree preflight. Forget removed shortcut
  positions in desktop.cfg and bundled per-game launcher preferences.
- DESK_SETTINGS_PROGRAMS (9) requests native inventory and launcher refresh
  after settings persistence. The existing shell discovery worker queries
  bundled presence; Games omits removed IDs. DOS Box's create path also checks
  presence and explains that a removed bundled game requires reinstallation.
- Install uses the shared file dialog and its removable/CD Places. The
  launch-owned Control Panel sends an existing DESK_LAUNCH request named
  `install`; the desktop validates a SETUP.EXE/INSTALL.EXE/BAT under /mnt/d..z
  and hands it to the existing launch worker. No installer engine is added.
  BAT classification follows the existing DOS route; mounted-media DOS paths
  translate to their drive letter. Ordinary clients retain their launch rights.

Validation belongs to `build/codex/f2-87-report.md`. The changed Add/Remove
frames were visually inspected in both existing themes before updating their
pixel baselines. Fixture FAT operations never modify the canonical games disk.


Continuation validation notes: the device-manager render fixture still lacked
its existing Programs page filename/count, causing a UBSan bounds failure.
After repairing that fixture, three older reference frames (System category,
Display and Fonts) were reproduced with the unmodified HEAD Control Panel
sources and matched the current renderer exactly. They were visually reviewed
and their stale hashes refreshed, alongside the new Programs frame. This did
not change those three production views. The Add/Remove references were also
reviewed in Ciuki Blue and Platinum. Detailed commands/results are in the
implementation report.

For uninstall keys without InstallLocation, a private uninstaller's containing
directory is used only to identify shortcuts. Shared/system directories are
never inferred as ownership of all their shortcuts. Record I/O errors propagate
as incomplete cleanup; malformed records are not treated as executable launch
records. User preference writes keep the existing asynchronous settings writer.


Amendment 1 (2026-10-21) supersedes the inventory ceiling and removal-lifecycle
notes above:

- Rechecked Microsoft's [Uninstall registry key](https://learn.microsoft.com/en-us/windows/win32/msi/uninstall-registry-key),
  [NoRemove](https://learn.microsoft.com/en-us/windows/win32/msi/arpnoremove) and
  [NoModify](https://learn.microsoft.com/en-us/windows/win32/msi/arpnomodify)
  documentation against the copied adapter. Preserve installer-owned fields
  and the existing protection behavior; retain separate HKCU identity even
  when HKLM fills the requested buffer. Bounded results explicitly flag
  omissions; uninstall completion checks the selected key directly instead
  of inferring absence from a truncated list. The combined list preserves
  protected rows by displacing
  removable rows, and explains the limit without clearing the inventory.
- Read the owner's Resource Kit in place: `wrkc25_2.htm` (Installing
  Applications Locally), `wrkc31_10.htm` (stale uninstall registration), and
  `wrkc05_7.htm` (irreversible removal confirmation). Keep Setup/Install
  discovery and the installer-owned uninstall path. No private text or
  media is copied into tracked files.
- The owner-side block callbacks supply the mandatory seek-position output.
  Host validation mounts an independent FAT image inside an outer FAT/VFS
  fixture, exercising the real owner provider, rather than only the tree walker.
- The execution contract now specifies management-only delegation, matching
  game reservation across both DOS kernels, and the durable pending identity.
  Cleanup is retryable after an absent subtree or a settings/COW failure.
  Icon ownership is collected and durably saved before shortcut deletion;
  the marker is removed only after those steps succeed.
- Native inventory refresh has a distinct flag, consumed independently of
  wallpaper/settings reload. External-only launch records in `/Games` and
  `/Programs` establish ownership. Bundled shortcuts normalize DOS Box paths
  and match executable tokens independently of optional switches and case.
- DOS installer preparation returns to the existing desktop launch mailbox.
  Its main thread calls `desk_task_launch` for DOS Box, preserving supervisor
  delegation. The unprivileged shell helper refuses a DOS spawn.

fx-4 (2026-10-23) amends the owner provider: since f3-63 the titles live in
`C:\SYSTEM\DOS\UNIFIED.IMG`, whose reference MBR holds one active FAT16
partition of type `0Eh` at LBA 63. The provider recognised only type `06h`,
mounted the MBR as a BPB and refused every query, launch and removal with
`EINVAL` (DOOM and Quake `error=-5` on the T23 trial of image `9a60215f`).
It now selects the partition with `partition_dos_primary()`, the rule the
VM uses for C: (`docs/design/dos-dpmi-contract.md`, fx-4 section, with the
Microsoft and FreeDOS sources), and reports the failed step and the FAT
diagnostic for the creation refusal record. Host evidence:
`test_actual_owner_provider_partitioned` in
`tests/host/desktop/test_add_remove.py`.

Focused host evidence and environmental sanitizer limitations are recorded in
`build/codex/f2-87-report.md`. Guest evidence remains the lead's responsibility.
