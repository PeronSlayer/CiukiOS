# Third parts of the OS feature items (2026-10-24)

- **Written:** 2026-10-24 10:38 by the lead. Owner 2026-10-24 ~09:50: use Codex to the end of its quota. Base: branch `wt/preverify` (main plus the integration repair snapshot). Rules: those of `g7-g12-os-gaps-batch-2.md` (header) and the second-parts directive; Codex gpt-6.1-sol high. These items enter today's T23 image only if they are verified before its chain starts; otherwise the next image.

| Item | Scope (remaining gaps recorded by the first and second parts) |
| --- | --- |
| G22c | Native Media Player for MIDI and AVI through the existing MCI code (selection bar, Copy Object), native MIDI port and DOS MPU-401 in UART mode, preferred playback/recording/MIDI device routing, capture for the remaining driver families. |
| G26c | Browser printing through the G31 spooler, default programs and Open With integration from G8, Internet Options history management and security zones, automatic target=_blank windows. |
| G16b | The Wine comdlg32 file dialog crash (CD-8): reproduce on the host with the Wine sources the build fetches, fix it; Open/Save As multiple selection, Read Only box and large-icon view, the Font dialog Script box, in-place rename in Browse for Folder; CiukNote's Font and Paint's palette use the common dialogs; WordPad's `font_dialog()` switches to G16's dialog. |
| G27b | Calculator Statistics Box window, Paint printing (G31), Viewer saving rotated pictures, Terminal FORMAT (through G29's format service), PING (G26's raw ICMP), real RUN and REG, YMODEM, ZMODEM and Kermit file transfer in the serial terminal. |
| G30b | Boot-time use of the stored device disable/remove/driver choices and hardware profiles by the loader and kernel, scheduler idle and power conditions with the task's last result, System Monitor network counters, editable Performance tab settings (file system, graphics, virtual memory). |
| G29d | Disk Defragmenter's "rearrange program files so they start faster" (application start access pattern log and placement), conversion of the boot volume to FAT32 at the next start. |
