# fx-nativeicons: native programs show their own icon in Files and on the desktop

- **Written:** 2026-10-28 00:34 by the lead. Implementer: queued (Codex `gpt-6-luna` or a Claude Sonnet agent). Base: main 69f768794 or later.
- **Evidence:** the lead's QEMU run of image 27726d1d… (main 69f768794): `C:\Programs` in Files, Large Icons, shows the blank document icon for all 41 native programs (calc, netsurf, paint, …), while the Start menu draws each one's icon. `du_file_icon_candidate` (`apps/libdeskui/fileicon.c`) admits own icons only by extension, so an extensionless native executable never gets one. This completes the owner's fx-exeicons request ("per gli exe e simili … falli assegnare automaticamente").

## Windows 98 behaviour (research and cite)

Explorer shows every executable with its own icon (the first `RT_GROUP_ICON` of an EXE), and an executable without one with the default application icon (`shell32.dll` index 2), never the document icon. Cite Microsoft's `ExtractIcon`/`SHGetFileInfo` documentation and the Resource Kit in `build/rskt`.

## Scope

1. Files (every view, the tree and the Open/Save dialogs), the desktop, shortcuts to native programs and Add/Remove Programs show a native program's own icon from the same source the Start menu uses; find that source and resolve through libdeskui's file-icon cache, without a second icon catalogue.
2. An executable without an icon (native, DOS `.com`/`.exe`, Win32 `.exe`) shows the Tango application icon, not the document icon; data files keep today's type icons.
3. Host tests: a native program with an icon, one without, a Win32 EXE without resources, a DOS `.com`. The desktop, Files and libdeskui suites stay green; re-record goldens only where this change alone alters them, and list them in the report.

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-nativeicons` (branch `wt/fx-nativeicons`); never commit, merge or push; never run QEMU or image builds; run tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/fx-nativeicons-report.md` in the worktree. Leave changes uncommitted.
