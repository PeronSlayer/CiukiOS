# Desktop applications and shortcuts — validation, 29 September 2026

Image `build/full/ciukios-full.img` (copy `build/tests/apps/image-apps2.img`),
SHA-256 `019bfa88b810e7886499259bde8dd9416263bdee121683e4bbad8df7ff13e36f`,
built by a plain `scripts/build_full.sh`. QEMU with KVM; nothing here
qualifies physical hardware. Design and features:
[desktop-apps-2026-09-29.md](../../desktop-apps-2026-09-29.md).

Complete profile `build/tests/vm-window-profile-2026-09-29g`: **25/25 pass**
(`profile-summary.json`, `profile-artifacts.sha256`). That includes the new
lanes:

- **`desktop-apps`** (`desktop-apps.json`), which covers:
  - every shortcut;
  - Files: new folder and rename in place, copy/paste, cut/paste, delete
    with confirmation, right-click menu, new text document, properties,
    drag and drop onto a folder, and a click in the Folders tree;
  - Notepad: typing, save, Replace All, Undo twice, Find, Go To,
    Time/Date, Open, `.LOG` stamp;
  - Tasks: all four tabs, and End Task.

  After shutdown an independent FAT parser checks the disk: copy, move,
  drag-and-drop move, recursive delete, the saved text, the `.LOG` stamp,
  and that Time/Date wrote today's date.
- **`native-media`** (`native-media.json`): Files on the floppy, a USB BIOS
  disk and a CD-ROM. It browses `NESTED/DEEP`, previews `README.TXT`, and
  imports `PAYLOAD.BIN` byte-exact. The source images stay unchanged, and an
  ejected floppy reports an error.

Also run on the same code: `scripts/qemu_test_wallpaper_import.py
--make-fixtures` (`wallpaper-import.json`). The tiles are copied with Files,
the valid one is applied, the damaged one is rejected, and the choice
survives a cold boot.

Screenshots: `files-context-menu.png`, `files-properties.png`,
`files-dropped.png` (Places and Folders tree), `notepad-saved.png`,
`tasks-performance.png`.

Changes also covered by this profile:
- the kernel reads the date and time from the RTC;
- the Jemm Ctrl+Alt+Del patch;
- the new CVSESSION operation VMM_LIST (48h).
