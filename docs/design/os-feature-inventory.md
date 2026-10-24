# Operating-system feature inventory (fx-20)

- **Written:** 2026-10-23 18:00 CEST for directive fx-20
  (`docs/directives/t23-e831e685-trial-fixes.md`). This is read-only research on
  source state `e5d40038` (branch `wt/fx-20`). No code or tests were changed.
- **Purpose:** the owner asked for the complete feature set of a Windows
  95/98-class desktop operating system, written down at the level of the
  behaviours a user can see and the system services behind them, and checked
  against what CiukiOS has. Later directives quote this inventory when they
  say what "complete" means for an area. When a later directive closes a row,
  it updates that row's status and evidence in the same change.
- **Method:**
  - The reference behaviour comes from the owner's copy of the *Microsoft
    Windows 98 Resource Kit* (Microsoft Press, 1998), extracted read-only at
    `build/rskt/` and never redistributed, and from Microsoft's Win32
    documentation on learn.microsoft.com. Quotations are kept short;
    everything else is paraphrased.
  - The CiukiOS status comes from reading the code and the host tests of the
    canonical F0 build: `apps/*`, `src/kernel`, `sdk/*`, `tests/host`.
    `src/apps` and most of `src/com` are the 0.8 line, which the F0 image does
    not build, so they do not count as implementations.
  - The status is about whether the code exists and is host-tested. It says
    nothing about hardware qualification: rows marked done can still have T23
    defects, and those are tracked in the trial directives.
- **Status words:**
  - **done:** the Windows 98 behaviour exists in substance. Naming or layout
    differences are noted.
  - **partial:** something exists. The cell says exactly what is missing.
  - **absent:** nothing in the build provides the behaviour. "(scope)" marks a
    feature that an existing contract or owner decision deliberately leaves
    out; it still counts as absent.
- **Paths** are relative to the repository root, with `file:line` where the
  line matters. The line numbers belong to source state `e5d40038`.

## Reference keys

Windows 98 Resource Kit sections are cited by chapter or appendix and section
title. The file in brackets names the page in the extracted copy.

| Key | Source |
| --- | --- |
| RK-start | Ch. 5, "Troubleshooting Setup", Table 5.15 *Startup menu options* and Table 5.16 (`wrkc05_8.htm`); "Windows 98 Startup Process" (`wrkc05_3.htm`); "Using a Windows 98 Startup Disk" (`wrkc05_6.htm`) |
| RK-tools | Ch. 27, "Troubleshooting Tools", Table 27.1 (`wrkc27_2.htm`); "Troubleshooting Procedures" (`wrkc27_3.htm`) |
| RK-keys | App. G: "Shortcuts for Objects, Folders, and Windows Explorer" (`wrkappg_1.htm`), "General Keyboard-only Commands" (`wrkappg_2.htm`), "Accessibility Shortcuts" (`wrkappg_3.htm`), "Microsoft Natural Keyboard Keys" (`wrkappg_4.htm`) |
| RK-comp | App. D, "Msbatch.inf Sample File", section `[OptionalComponents]`: the list of shipped components (`wrkappd_3.htm`) |
| RK-cpl | Ch. 23, "Configuration Tools in Windows 98", Table 23.1 *Control Panel tools* (`wrkc23_2.htm`) |
| RK-power | Ch. 30, "Overview of Power Management" (OnNow, ACPI, APM 1.2, power schemes) (`wrkc30_4.htm`) |
| RK-acc | App. H, "Configuring Accessibility Features Using Control Panel" (`wrkapph_3.htm`), "Using the Accessibility Features" (`wrkapph_5.htm`), "Configuring … with the Accessibility Wizard" (`wrkapph_4.htm`) |
| RK-disk | Ch. 10: "Understanding FAT32" (`wrkc10_2.htm`), "Overview of Disk Utilities" (`wrkc10_3.htm`), "Disk Management" (incl. "Managing the Recycle Bin") (`wrkc10_5.htm`), "File System Management" (`wrkc10_7.htm`), "Technical Notes on Long File Names" (`wrkc10_8.htm`) |
| RK-print | Ch. 11: "Printing Support in Windows 98" (`wrkc11_1.htm`), "Imaging Support" (`wrkc11_2.htm`), "Font Support" (`wrkc11_3.htm`) |
| RK-mm | Ch. 12: "Overview of Multimedia" (MCI device types, file formats) (`wrkc12_1.htm`), "Recording, Editing, and Playing Audio" (`wrkc12_2.htm`), "Recording and Playing Digital Video" (`wrkc12_3.htm`) |
| RK-net | Ch. 15, "Microsoft TCP/IP Protocol" (`wrkc15_3.htm`); Ch. 18, "Logging on to Windows 98" and "Peer Resource Sharing" (`wrkc18_2.htm`, `wrkc18_4.htm`); Ch. 19, Dial-Up Networking (`wrkc19_2.htm`–`wrkc19_9.htm`); Ch. 20, Internet Explorer (`wrkc20_3.htm`); Ch. 21, modems and communications tools (`wrkc21_1.htm`); App. F, HOSTS (`wrkappf_1.htm`) |
| RK-apps | Ch. 25: "Configuring Applications" (file types, MS-DOS program properties, MS-DOS mode) (`wrkc25_3.htm`), "Using Applications" (`wrkc25_4.htm`), "Technical Notes" (`wrkc25_5.htm`) |
| RK-users | Ch. 7, "How User Profiles Work" (`wrkc07_3.htm`); Ch. 9, "Passwords" (`wrkc09_4.htm`) |
| RK-reg | Ch. 31, "Getting Started with the Registry Tools" (Registry Checker, ScanReg) (`wrkc31_2.htm`) |
| RK-dev | Ch. 24, "Overview of Device Management" and "Configuring Device Settings" (`wrkc24_1.htm`, `wrkc24_3.htm`) |
| RK-perf | Ch. 26: "Optimizing the Swap File" (`wrkc26_3.htm`), "Tracking Performance with System Monitor" (`wrkc26_9.htm`) |
| RK-intl | App. A, "Specifying International Settings" (Regional Settings, keyboard languages) (`wrkappa_3.htm`) |
| RK-tb | Ch. 6, "Configuring the Active Desktop" (taskbar toolbars, Quick Launch) (`wrkc06_2.htm`); Ch. 3, "Using the New Windows 98 Tools to Customize Installations" (Show Desktop, Quick Launch) (`wrkc03_2.htm`) |
| RK-mobile | Ch. 19, "Using Windows 98 Mobile Computing Features" (Briefcase, deferred printing, power) (`wrkc19_10.htm`) |
| RK-vmm | Ch. 28, "Virtual Machine Manager" (`wrkc28_4.htm`) and "User Interface" (`wrkc28_9.htm`) |
| MS-syscmd | [WM_SYSCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand): window menu commands SC_MOVE/SIZE/MINIMIZE/MAXIMIZE/RESTORE/CLOSE/TASKLIST/SCREENSAVE/MONITORPOWER |
| MS-winfeat | [Window Features](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features), [WM_GETMINMAXINFO](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-getminmaxinfo), [Multiple Document Interface](https://learn.microsoft.com/en-us/windows/win32/winmsg/multiple-document-interface) |
| MS-arrange | [CascadeWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-cascadewindows), [TileWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-tilewindows), [FlashWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-flashwindow) |
| MS-taskbar | [The Taskbar](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar), [Shell_NotifyIcon](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyicona) |
| MS-shell | [Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links), [File Types and associations](https://learn.microsoft.com/en-us/windows/win32/shell/fa-intro), [Managing the File System](https://learn.microsoft.com/en-us/windows/win32/shell/manage), [SHFILEOPSTRUCT](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shfileopstructa) (FOF_ALLOWUNDO sends deletions to the Recycle Bin), [Context menu handlers](https://learn.microsoft.com/en-us/windows/win32/shell/context-menu-handlers), [SHBrowseForFolder](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shbrowseforfoldera) |
| MS-dnd | [WM_DROPFILES](https://learn.microsoft.com/en-us/windows/win32/shell/wm-dropfiles), [DoDragDrop](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-dodragdrop) |
| MS-clip | [Clipboard](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard), [Standard Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats) |
| MS-cdlg | [Common Dialog Box Library](https://learn.microsoft.com/en-us/windows/win32/dlgbox/common-dialog-box-library) |
| MS-end | [WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession), [ExitWindowsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-exitwindowsex) |
| MS-spi | [SystemParametersInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfoa) (screen saver, drag full windows, accessibility, mouse, keyboard settings), [STICKYKEYS](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-stickykeys) |
| MS-ssave | [Screen Saver Library](https://learn.microsoft.com/en-us/windows/win32/lwef/screen-saver-library) |
| MS-power | [WM_POWERBROADCAST](https://learn.microsoft.com/en-us/windows/win32/power/wm-powerbroadcast) |
| MS-cursor | [About Cursors](https://learn.microsoft.com/en-us/windows/win32/menurc/about-cursors) |
| MS-kbd | [About Keyboard Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input) |
| MS-print | [Print Spooler API](https://learn.microsoft.com/en-us/windows/win32/printdocs/print-spooler-api) |
| MS-time | [GetTimeZoneInformation](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-gettimezoneinformation), [SetLocalTime](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-setlocaltime), [National Language Support](https://learn.microsoft.com/en-us/windows/win32/intl/national-language-support) |

## Layout note

The CiukiOS shell does not copy the Windows 98 layout one for one. The Ciuki
(Start) button, the tray and the clock sit in a 32 px top bar (`DESK_BAR`). The
bottom bar holds only the window buttons (`docs/desktop.md`,
`apps/desktop/compositor.c:184`). The rows below judge behaviour, not
placement: a feature is done when the user can do the same thing.

## 1. Boot, startup menu and recovery

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| BT-1 | Boot loader that starts the OS from the hard disk | RK-start | done | MBR → `src/boot/ciukldr.asm` (+ `ciukldr/*.inc`) → FAT32 kernel; `tests/host/test_loader_model.py`, `test_boot_modes.py` |
| BT-2 | Startup menu opened by holding Ctrl (F8 still works) or shown automatically after a failed start. Choices: Normal, Logged (`BOOTLOG.TXT`), Safe Mode, Step-by-Step Confirmation, Command Prompt Only, Safe Mode Command Prompt Only, Previous version of MS-DOS | RK-start Table 5.15 | partial | `ciukldr/menu.inc:590-700` shows a menu for about 3 s with N (Normal), S (Safe), L (Log), H (Hardware check) and P (Probe); in a normal boot only a one-line hint is on screen (`menu.inc:760`). Missing: F8/Ctrl hold, the menu shown automatically after a failed boot, Step-by-step confirmation, a command-prompt-only boot (Terminal or DOS session without the desktop), and the choice of safe mode with or without storage |
| BT-3 | Safe Mode: only mouse, keyboard and VGA drivers, bypassing startup files and the registry; entered automatically when the previous start failed | RK-start | partial | `CBI_F_SAFE_MODE` skips ATA, accelerated video, audio, network, ACPI and APM (`src/kernel/core/init.c:141,172`, `drivers/net.c:320`, `acpi/platform.c:217`); `tests/host/test_safe.py`, `tests/suites/f1-safe.json`. Missing: safe mode skips ATA, so there is no C: and no desktop configuration to repair, which is the main use Windows makes of it; there is no automatic safe boot after a failed start |
| BT-4 | Boot splash with the logo while the system starts | RK-start | done | Photograph, tagline and progress bar in `src/kernel/core/console.c`; `tests/host/test_splash_progress.py` |
| BT-5 | Startup sound and logon sound | RK-mm | done | `apps/desktop/desktop.c:2577` plays `SOUND_STARTUP` then `SOUND_LOGON`; `tests/host/desktop/sounds_test.c` |
| BT-6 | Boot log on request (`Logged` option writes `BOOTLOG.TXT`) | RK-start | done | Always-on `C:\SYSTEM\LOGS\BOOT.LOG` with rotation (`src/kernel/core/bootlog.c`), black box (`core/blackbox.c`); L adds verbose screen output; `tests/host/bootlog_test.c`, `blackbox_test.c`. Naming differs, behaviour exceeds |
| BT-7 | Registry Checker: daily compressed backup of the registry and configuration files at every start, automatic restore of a good copy when the registry is damaged, `ScanReg /Restore` from the command prompt | RK-reg, RK-tools | partial | Journalled atomic saves with crash recovery (`sdk/libreg/storage.c:80-140`, `reg_recover`); `tests/host/registry_test.c`. Missing: the set of dated backups, the restore choice at boot, and a user tool |
| BT-8 | ScanDisk runs at the next start after an improper shutdown and asks before it runs | RK-disk ("File System Management") | partial | The FAT mount repairs a dirty volume before enabling writes (`src/kernel/fs/fat.c:1935-1960`, `mount.c:813,1062`, `reason=dirty_recovered`). Missing: any user-visible ScanDisk screen, prompt or report (see ST-9) |
| BT-9 | Startup disk: create a bootable rescue floppy from Add/Remove Programs | RK-start, RK-cpl | absent | The Setup CD with its Rescue page (`apps/setup/rescue.c`) is the only recovery medium. There is no in-OS tool to create one |
| BT-10 | Automatic Skip Driver Agent: devices that stop the start are skipped on the next boot | RK-tools | partial | Drivers that fail are quarantined per boot (for example `accel=0`, the SuperSavage quarantine in `docs/release-notes-1.0-alpha.md`). Missing: a persistent skip list and a UI to re-enable a driver |
| BT-11 | System Configuration Utility (msconfig): turn startup items and configuration files on or off with check boxes, enable the boot menu by default | RK-tools | partial | G21: System > Startup lists Startup-folder and machine/user registry startup entries with a persistent selected-item enable checkbox; changes apply at the next desktop start (`apps/desktop/startup.c`, `apps/control/app.c`; `test_startup.py`, `test_game_properties.py`). Missing: configuration-file line switches and boot-menu default selection (G14). The Logs page still controls the serial log in `BOOT.CFG`. |
| BT-12 | System File Checker: verify system files, restore originals from the CD | RK-tools | absent | `build-manifest.json` hashes exist at build time but nothing checks them at run time |
| BT-13 | Setup: install to the hard disk with hardware detection, a setup log, Safe Recovery after a failed setup, unattended scripts (`msbatch.inf`) | RK-start, RK-comp | done | `apps/setup/*` wizard, `sdk/libsetup/txn.c` (`SETUP.LOG`), `apps/setup/inf.c` (`SETUP.INF`), hardware page `apps/setup/hw.c`; `tests/host/setup/run.py`. Limits: whole disk 0 only, no GPT (`sdk/libdisk`) |
| BT-14 | First-run welcome ("Welcome to Windows 98": registration, tour, maintenance) | RK-start | partial | Seven-step "Finishing your installation" wizard (`apps/desktop/firstboot.c`, `firstboot_native.c`); `tests/host/desktop/firstboot_test.c`. Missing: a welcome or tour screen and Tip of the Day on the desktop (tips exist only in Setup, `apps/setup/catalog.c:52-59`) |
| BT-15 | Uninstall the OS (Add/Remove Programs → "Uninstall Windows 98") | RK-start ("Removing Windows 98") | absent | Out of the current Setup design |

## 2. Shutdown, restart, standby and session end

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| SD-1 | Shut Down dialog: Stand by, Shut down, Restart, Restart in MS-DOS mode | RK-power, RK-apps | partial | `apps/desktop/shell.c` offers Restart, Shut Down and Cancel; G3: both ask every program first (`desk_session_begin`), and Restart is the default button when the dialog comes from a second Ctrl+Alt+Del. Missing: Stand by (see PW-6) and an equivalent of Restart in MS-DOS mode (see DOS-9) |
| SD-2 | Orderly shutdown: every program is asked (WM_QUERYENDSESSION), may prompt to save and may veto; programs that do not answer are offered for termination; then WM_ENDSESSION | MS-end | done | G3 (`docs/design/desktop-shell.md` "End of session"): `desk_session_begin`/`desk_session_tick` in `apps/desktop/client.c` ask every native (`QUERY_END_SESSION`), DOS and Win32 program in turn, topmost first (Win32 through the w32srv HOLD session, `apps/w32srv/session_impl.h`); a program's own save prompt is waited for with no deadline while it answers PINGs; "This program is not responding" with End Task/Cancel after the 5 s HungAppTimeout; any veto or Cancel sends END_SESSION(0)/WM_ENDSESSION(FALSE) and keeps every program; only then END_SESSION(1)/WM_ENDSESSION(TRUE) and the fx-5 power sequence. Log Off (action 3) runs the same queries without power. Host tests: `tests/host/desktop/end_session_test.h`, `tests/host/win32/shell/shell_test.c` |
| SD-3 | Flush caches, then soft power-off (ACPI S5 / APM), or "It's now safe to turn off your computer" | RK-power | done | Kernel call 73 (`src/kernel/core/power.c:318-372`), ACPI S5 or APM 5307h, message at `client.c:1427-1431`; `tests/host/power_test.c`, `storage_power_test.c` |
| SD-4 | Restart (warm reboot) | — | done | `src/kernel/core/reboot.c` (8042 pulse, then triple fault) |
| SD-5 | Ctrl+Alt+Del: Close Program dialog (task list, End Task, Shut Down); pressed again, restarts | RK-keys, RK-tools | done | Ctrl+Alt+Del opens Task Manager, or brings it to the front; pressed again with Task Manager in front it opens Shut Down with Restart the default (G3: the orderly restart, so nothing unsaved is lost, unlike Windows 98's immediate restart); Task Manager File > Shut Down; End Task asks the program to close and offers "not responding" End Task (`client.c` `desk_task_end`); End Process warns about unsaved data (`apps/tasks/tasks.c`) |
| SD-6 | Log Off *user* (Start menu), returning to the logon dialog | RK-users | absent | No users (see SS-1) |
| SD-7 | Power-button press runs the configured action (shut down, standby) | RK-power | partial | It always shuts down (`shell.c:1173-1175`); there is no choice |

## 3. Sessions, logon, users and security

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| SS-1 | Logon dialog (user name and password), Microsoft Family Logon, per-user profiles (Desktop, Start Menu, My Documents, Favorites, settings) | RK-users, RK-net (`wrkc18_2`) | absent | The desktop opens straight to the shell. Setup's "Your name" is stored in `System\Identity` (`apps/control/configuration.c:30`) and used nowhere; Win32 `GetUserName` returns "Ciuki" (`apps/w32srv/registry.c:396-398`) |
| SS-2 | Passwords applet: change the Windows password, remote administration, enable user profiles | RK-cpl | absent | — |
| SS-3 | Screen saver password and locking | RK-users, MS-ssave | absent | G10 supplies savers; passwords and locking remain G32, explicitly deferred by the G10 work item. No secure-lock claim |
| SS-4 | Users applet: create and manage users | RK-cpl | absent | — |
| SS-5 | StartUp folder and the `Run`/`RunServices` registry keys start programs at logon; `RunOnce` once | RK-apps | done | G21: each ordinary desktop start snapshots HKLM RunServices/RunServicesOnce, HKLM/HKCU Run/RunOnce and `/WINDOWS/Start Menu/Programs/StartUp`; a background worker uses the shared native/DOS/Win32 launcher. Shift bypasses the session batch; Safe Mode/test boots suppress it. One-shot values are compared and deleted/flushed before dispatch; disabled entries remain registered. Bound: 128 entries; CiukiOS `.lnk` format and regular executables. See `docs/design/startup-programs.md`; host `test_startup.py`, `test_game_properties.py`, `test_window_model.py`. Existing virtual-restart handling remains. |
| SS-6 | System policies (Policy Editor) | RK-comp, ch. 8 | absent (scope) | Enterprise feature; no counterpart planned |

## 4. Window manager

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| WM-1 | Create, show and hide windows; focus and activation follow a click; active and inactive caption colours | MS-winfeat | done | `apps/desktop/client.c:~690-720`, `input.c:100-137`, `theme.c:26-47,666-731`; `tests/host/desktop/interaction_test.c` |
| WM-2 | Z-order, with stale pixels repainted correctly on every change | MS-winfeat | done (host) | `desk_stack_damage`; `tests/host/desktop/occlusion_test.c` compares every frame with a full composite. fx-15 audits every other path and the T23 symptoms |
| WM-3 | Always-on-top (topmost) windows | MS-winfeat | partial | Win32 `HWND_TOPMOST` band (`client.c:1466-1495,1641-1662`); `tests/host/desktop/v3_test.c:220`. Missing for native clients; the shell-over-v3 order is an open lead decision |
| WM-4 | Modal dialogs block their owner; owned windows stay above their owner | MS-winfeat | done | `desk_modal_target` (`input.c:74`), forbidden cursor over a blocked owner (`input.c:180`); Win32 `sdk/win32/user32/dialog.c:769-858` |
| WM-5 | New windows cascade from the previous one | MS-winfeat | done | `client.c:709-710` (8 slots, 18 px). It does not track windows the user moved |
| WM-6 | Move by dragging the caption, with full window contents ("Show window contents while dragging") | MS-syscmd, MS-spi (SPI_SETDRAGFULLWINDOWS) | done | `input.c:405-414,475-490`. Missing: the outline-drag alternative as a setting (Windows 98 Effects tab) |
| WM-7 | Resize from eight borders and corners with resize cursors, honouring minimum and maximum track sizes | MS-winfeat (WM_GETMINMAXINFO) | done (host) | Native: `input.c:149-165,199-205`, `client.c:296` with limits and steps; `interaction_test.c`. G4: Win32 `WM_GETMINMAXINFO` ptMinTrackSize/ptMaxTrackSize go to the desktop as `DESK_V3_LIMITS` (`sdk/win32/user32/desktop.c` `u32_limits`, `client.c` `v3_apply_limits`); `taskbar_test.c` `v3_zoom_limits`, `user32_test.c` `test_zoom_limits`. ptMaxSize and ptMaxPosition are not honoured (the desktop maximises to the work area within the maximum track size) |
| WM-8 | Maximise and restore (button and caption double-click) filling the work area | MS-syscmd | done (host) | `desk_zoom` (`client.c`), caption double-click (`input.c`). G4: the maximised state is the placement/configure state 3 for Win32 windows: `ShowWindow(SW_MAXIMIZE)` zooms the window, the desktop's zoom sets `WS_MAXIMIZE` (`IsZoomed`) and sends `WM_SIZE` with `SIZE_MAXIMIZED`, restore sends `SIZE_RESTORED` |
| WM-9 | Minimise to the taskbar; click the button to restore; click the active button to minimise | MS-taskbar | done | `client.c:1027`, `shell.c:2619-2625`; `desktop_test.c:581-592` |
| WM-10 | Close button with a cooperative close (WM_CLOSE) | MS-syscmd | done | `client.c:1051` (`desk_request_close`); Win32 WM_CLOSE (`user32/desktop.c:256`) |
| WM-11 | Window menu (Alt+Space or a caption icon click): Restore, Move, Size, Minimize, Maximize, Close, with keyboard Move and Size by arrows | MS-syscmd, RK-keys | done (host) | `shell.c` `SH_MENU_TASK`, keyboard geometry `input.c`; G4: access keys (R M S N X C), caption icon click opens the menu and a second click closes (`desk_shell_caption_icon`, `desk_caption_icon_box`; drawn in the themes with a caption icon, not Platinum); `tests/host/desktop/taskbar_test.c` (`mnemonics`, `caption_icon`). Open: Win32 programs' own system menus are unreachable because Alt+Space is taken first |
| WM-12 | Taskbar context menu: Cascade Windows, Tile Windows Horizontally, Tile Windows Vertically, Minimize All Windows, Undo Minimize All, Properties | MS-arrange, MS-taskbar | done (host) | G4: `SH_MENU_TASKBAR` on the empty bottom or top bar (`shell.c`), `desk_arrange`/`desk_minimize_all`/`desk_arrange_undo` (`client.c`); Win32 `CascadeWindows`, `TileWindows`, `ArrangeIconicWindows` (`sdk/win32/user32/arrange.c`); `taskbar_test.c`, window-model rows 31-33, `user32_test.c` `test_arrange`. `Properties` opens the Control Panel until G20's Taskbar Properties; Toolbars is G20's |
| WM-13 | Show Desktop (Quick Launch button, Win+D) / minimise all | RK-tb | partial | G4: `desk_show_desktop` (Win+D toggles), Win+M, Shift+Win+M, Alt+M, the taskbar menu. The Quick Launch button is G20's (no Quick Launch yet): it will call `desk_show_desktop` |
| WM-14 | Minimise and restore animation (zoom rectangles) | MS-spi (SPI_SETANIMATION) | done | `desk_animate` (`client.c:335-360`), a 150 ms outline; `interaction_test.c` |
| WM-15 | MDI (multiple document windows inside a frame, Ctrl+F4, Ctrl+F6) | MS-winfeat (MDI) | absent | No `DefFrameProc` or `CreateMDIWindow` in `sdk/win32/user32` |
| WM-16 | Hung-program detection ("Not responding"), end a hung task | RK-tools | done | PING/PONG with a 5 s mark, "Not responding" caption and busy cursor (`client.c`, `compositor.c`); G3: closing a window that does not answer for 5 s, End Task on a closing or hung window, and a hung program during Shut Down get "This program is not responding" with End Task (SIGKILL) and Cancel (`desk_end_task_open`, `DESK_INTERNAL_END_TASK`); no close deadline otherwise |
| WM-17 | Taskbar button flashes when a background window asks for attention | MS-arrange (FlashWindow) | absent | No flash state in the protocol or in `client.c` |
| WM-18 | Full-screen programs (DOS Alt+Enter, DirectDraw exclusive) and switching back | RK-apps | done (host) | `DESK_DOS_FULLSCREEN` (`shell.c:798-806`), v3 lease (`client.c:1518-1556`); `v3_test.c:145`. T23 defects are tracked in fx-16 |
| WM-19 | Multiple monitors (Windows 98 supports up to nine displays) | RK-vmm, MS-winfeat | absent (scope) | `MonitorFromWindow` returns one constant handle (`sdk/win32/user32/upper.c:258-285`) |
| WM-20 | Cursors: arrow, I-beam, hourglass, AppStarting, resize, move, hand, no-drop, help | MS-cursor | done | 22 shapes (`apps/desktop/protocol.h:67-77`), routing `input.c:166-218`; `tests/host/desktop/cursor_set_test.c` |
| WM-21 | Pointer schemes (large, inverted, animated cursors) chosen in Mouse properties | RK-acc | absent | One fixed set in `assets/cursors/` |
| WM-22 | Window opening is fast: the frame appears promptly after a click, with the AppStarting cursor in between | RK-perf | partial | AppStarting from click to the first frame (`input.c:190-197`); `[desktop] launch … spawn_ms` records (`desktop.c:566-581`). The T23 still reports slow opening; fx-15 measures and fixes it |

## 5. Keyboard shortcuts (system-wide)

Each row is one shortcut from RK-keys. Shortcuts that belong to one
application are in that application's section.

| ID | Shortcut: Windows 98 action | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| KB-1 | Alt+Tab / Alt+Shift+Tab: switcher with icons | RK-keys | done | `shell.c:1612-1675,2996-3000`; `interaction_test.c` |
| KB-2 | Alt+Esc: next window | RK-keys | done | `shell.c:3009-3037`. It does not cycle minimised windows |
| KB-3 | Alt+F4: close the window; with nothing focused, the Shut Down dialog | RK-keys | done | `shell.c:3002-3008` |
| KB-4 | Ctrl+Esc and the Windows key: Start menu | RK-keys | done | `shell.c:2947-2972,3052` |
| KB-5 | Alt+Space: window menu; Alt+Hyphen: MDI child menu | RK-keys | partial | Alt+Space done (WM-11); no MDI |
| KB-6 | Alt+Enter: properties of the selected object; full screen in a DOS window | RK-keys | partial | G6: Alt+Enter and context Properties in Files and for desktop files/folders open the shared sheet; existing DOS/v3 full-screen behavior preserved. Virtual Computer/Recycle Bin and general shortcut property pages remain separate shell features. |
| KB-7 | F10 / Alt: menu bar mode; Alt+letter: menu; Shift+letter or letter: menu item | RK-keys | done | `apps/libdeskui/widgets.c:1193-1270`; Win32 `user32/menu.c:1514-1570` |
| KB-8 | Shift+F10 and the Application key: context menu for the selection | RK-keys | partial | G4: desktop icons, the desktop and the taskbar focus (`desktop_menu_key`, `taskbar_keys`; `taskbar_test.c` `context_keys`). List and tree widgets as before (`widgets.c:1230-1236`). Missing: text fields, Win32 `VK_APPS` |
| KB-9 | Alt+F6: next window of the same program; Ctrl+F4 / Ctrl+F6: MDI | RK-keys | partial | G4: Alt+F6 (`shell.c`, `taskbar_test.c` `alt_keys`). Ctrl+F4/Ctrl+F6 need MDI (WM-15) |
| KB-10 | Print Screen: screen to clipboard; Alt+Print Screen: active window to clipboard | RK-keys | partial | G5: Print Screen captures the composed desktop, Alt+Print Screen the active frame, as a DIB on the system clipboard (`apps/desktop/clipboard.c`); `tests/host/desktop/test_system_clipboard.py`. Exclusive full-screen framebuffer capture still missing |
| KB-11 | F1: Help; Shift+F1: What's This? | RK-keys | partial | F1 in CiukNote, Files, Calculator and CiukIDE opens a static message box (`apps/files/app.c:1335`, `apps/accessories/calc_view.c:126`), not Help topics. No What's This? |
| KB-12 | Win+E Explorer, Win+F Find, Win+Ctrl+F Find Computer, Win+R Run, Win+M Minimize All, Shift+Win+M Undo, Win+Tab cycle taskbar buttons, Win+F1 Help, Win+Break System Properties, Win+D desktop | RK-keys (`wrkappg_4`) | partial | G4: every chord (`logo_chord`, `shell.c`) from PS/2 set 2, the firmware set-1 decoder and USB HID (tests assert 0x15b/0x15c/0x15d/0x200). Win+F, Win+Ctrl+F and F3 open a placeholder message until G7 (`desk_shell_find`) |
| KB-13 | Alt+M with focus on the taskbar or desktop: minimise all; Ctrl+Esc, Esc: focus the taskbar, then Tab through it | RK-keys (`wrkappg_2`) | done (host) | G4: taskbar keyboard focus (`taskbar_set`/`taskbar_keys`), Alt+M, Alt+S; `taskbar_test.c` `taskbar_focus` |
| KB-14 | Ctrl+Alt+Del: reserved for the system (Close Program) | RK-keys | done | Task Manager (SD-5) |
| KB-15 | Shift while inserting a CD: bypass AutoPlay | RK-keys | absent | `apps/desktop/autoplay.c` has no modifier check |
| KB-16 | Ctrl+Tab / Ctrl+Shift+Tab: next or previous tab in a property sheet | RK-keys | done | `widgets.c:1209-1216` (first tab control) |
| KB-17 | Accessibility hotkeys: Shift×5 StickyKeys, right Shift 8 s FilterKeys, NumLock 5 s ToggleKeys, Left Alt+Left Shift+NumLock MouseKeys, Left Alt+Left Shift+PrintScreen High Contrast | RK-keys (`wrkappg_3`) | absent | See AC-* |

## 6. Desktop

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| DT-1 | System icons: My Computer, My Documents, Recycle Bin, Network Neighborhood (and optionally Internet Explorer, Outlook Express, My Briefcase) | RK-vmm ("User Interface"), RK-comp | partial | Computer, Documents, Recycle Bin (G2: full and empty picture, `shell.c` icons_build, `folder.c` bin state), optional Pictures. Missing: Network Neighborhood (no file sharing, see NW-12) |
| DT-2 | Files and folders stored on the desktop | MS-shell | done | `/Documents/Desktop` projected as icons (`shell.c:1446-1455`, `folder.c`); `tests/host/desktop/desktop_folder_test.c` |
| DT-3 | Icon drag to any position; Arrange Icons by Name, Type, Size, Date; Auto Arrange; Line Up Icons | MS-shell | partial | Free positioning and Line Up (`shell.c:719-746,2465-2490`, `settings.c:238-247`). Missing: the four sort keys (Arrange only clears positions) and Auto Arrange |
| DT-4 | Rubber-band selection, Ctrl/Shift multi-select, Ctrl+A on the desktop | RK-keys | absent | `icon_selected` is a single index (`apps/desktop/shell.h`). Directive fx-19 |
| DT-5 | Keyboard on the desktop: arrows, Enter, F2, Del, Shift+Del, Ctrl+X/C/V, Alt+Enter, F5, type-ahead | RK-keys | partial | G6 adds Alt+Enter on desktop icons, routing real file/folder objects to Files Properties. Existing arrows, Enter, F2, Delete, Ctrl+A/X/C/V and F5 retained. G2 adds Shift+Delete with permanent-delete confirmation. Remaining type-ahead and Home/End belong to G19. |
| DT-6 | Desktop context menu: Arrange Icons ▸, Line Up Icons, Refresh, Paste, Paste Shortcut, Undo, New ▸ (Folder, Shortcut, registered document types), Properties | MS-shell | partial | `shell.c:422-442` has Arrange, Line Up, Refresh, Paste, New ▸ Folder and Text file, Properties (opens Display). Missing: Paste Shortcut, Undo, New ▸ Shortcut, New ▸ Bitmap (implemented as `DF_BITMAP`, `folder.c:399`, but not on the menu) |
| DT-7 | Icon context menu: Open (bold default), Explore, Open With, Send To ▸, Cut, Copy, Create Shortcut, Delete, Rename, Properties | MS-shell | partial | `shell.c:426-463`: Open, Open With, Cut, Copy, Rename, Delete, Properties (desktop entries only). Missing: Explore, Send To, Create Shortcut, Paste into a folder icon |
| DT-8 | Rename an icon in place | RK-keys | partial | F2 inline rename of desktop entries (`folder.c:60-70`); system icons cannot be renamed |
| DT-9 | Wallpaper: picture centred, tiled or stretched; patterns; Browse | RK-cpl (Display) | partial | Center, stretch, tile, fit, fill; PNG, JPEG and BMP (`apps/desktop/wallpaper.c`, `settings.h:23`). Missing: Browse (the list only scans `/system/wallpapers`, `apps/control/main.c:286-295`) and "Set as Wallpaper" from Paint, the Viewer and the browser |
| DT-10 | Active Desktop (HTML desktop, web items, channel bar) | RK-tb | absent (scope) | No HTML desktop planned |
| DT-11 | Drag and drop between the desktop, windows, folders and the Recycle Bin, with copy/move/link rules | MS-dnd, RK-keys | partial | See the DnD section. Directive fx-19 |

## 7. Start menu

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| SM-1 | Top level: Windows Update, Programs, Favorites, Documents, Settings, Find, Help, Run, Log Off, Shut Down | RK-tb | partial | `shell.c:333-341`: Programs, Documents, Settings, Help, Run, Shut Down. Missing: Find, Log Off, Favorites (Windows Update is out of scope, `docs/design/win32-personality-contract.md:89`) |
| SM-2 | Programs: a tree of folders built from the Start Menu folder, with Accessories, System Tools, Games and StartUp | RK-tb | partial | Compiled-in catalogue (`shell.c:233-259`) with Accessories, System Tools, Games and Developer; up to 8 Win32 shortcuts listed flat (`shell.c:349-351`). Missing: Win32 `Start Menu\Programs` folders as submenus, a StartUp folder |
| SM-3 | Start menu customisation: drag items into the menu, Taskbar & Start Menu properties (Add, Remove, Advanced, Clear Documents), Show small icons | RK-tb | absent | No Taskbar & Start Menu page |
| SM-4 | Documents: the 15 most recent documents, cleared from the properties | RK-tb | partial | `shell.c:397-405`, 8 items from `cfg.recent`. The apps' own `recent.lst` (`apps/libdeskui/recent.c`) is never read by the shell, although the header says it is |
| SM-5 | Settings: Control Panel, Printers, Taskbar & Start Menu, Folder Options, Active Desktop | RK-cpl | partial | `shell.c:407-416`: Control Panel, its categories, Task Manager. Missing: Printers, Taskbar & Start Menu, Folder Options |
| SM-6 | Find ▸ Files or Folders, Computer, On the Internet, People | RK-keys | absent | See FM-30 |
| SM-7 | Help opens Windows Help | RK-keys | partial | The Help submenu has Keyboard Shortcuts and About (`shell.c:417-421`); the Help application is under Accessories |
| SM-8 | Run dialog: command line with history drop-down, Browse; accepts programs, documents, folders and URLs | RK-keys | partial | `shell.c:2297-2315`, `desktop.c:1632-1662`: only names in `/Programs` with up to six arguments. Missing: history, Browse, paths, documents, folders, URLs, Win32 and DOS programs |
| SM-9 | Keyboard: arrows, Enter, Esc, and underlined letters in menus | RK-keys | done (host) | `shell.c`: arrows, Home/End, Enter, Space, Esc; G4: access keys and underlines in the fixed shell menus, type-ahead in the others (`access_keys`, `menu_letter`); `taskbar_test.c` `mnemonics` |

## 8. Taskbar and notification area

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| TB-1 | One button per top-level window, with icon and title; the active one pressed; tooltip with the full title | MS-taskbar | done | `shell.c:1129-1159`; `tests/host/desktop/bars_test.c`, `tray_layout_test.c` |
| TB-2 | Taskbar Properties: Always on top, Auto hide, Show small icons in Start menu, Show clock | RK-tb | absent | Fixed bars (`compositor.c:184`) |
| TB-3 | Move the taskbar to any screen edge and resize it | RK-tb | absent | — |
| TB-4 | Toolbars: Quick Launch (with Show Desktop), Address, Links, Desktop, New Toolbar from a folder | RK-tb | absent | — |
| TB-5 | Notification area: programs add icons with tooltips and menus (Shell_NotifyIcon) | MS-taskbar | partial | Fixed system icons for network, volume, removable media, notifications and battery (`shell.c:1179-1226`). Missing: a protocol message for program icons; Win32 `Shell_NotifyIconA` only logs (`sdk/win32/shell/execute.c:104`) |
| TB-6 | Clock: time in the tray, date in the tooltip, double-click opens Date/Time properties | RK-cpl | done | G1: the tray clock shows local time; a double-click opens Date/Time Properties (`shell.c`); date tooltip and right-click entry unchanged |
| TB-7 | Volume icon: click for a master slider and mute; double-click for Volume Control | RK-mm | partial | Slider, mute, tooltip, media keys (`shell.c:1234+,2528`; `desktop_test.c:914`). Missing: double-click to open the mixer |
| TB-8 | Battery meter in the tray with tooltip (percent, time left), a click for the power meter | RK-power | partial | Icon, caution state and AC mark (`shell.c:1198-1224`). Missing: tooltip, percent; no data on the T23 (directive fx-18) |
| TB-9 | Network and removable-media icons | — | done | `shell.c:923,1040-1049,1104`; `tests/host/desktop/network_tray_test.c`, `media_tray_test.c` |
| TB-10 | Balloon notifications from programs | MS-taskbar | partial | One shell-internal balloon slot (`shell.c:1327-1400`); no client API and no queue |
| TB-11 | Keyboard-language indicator in the tray (when more than one layout is installed) | RK-intl | absent | See IN-3 |

## 9. File manager (My Computer and Windows Explorer)

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| FM-1 | Views: Large Icons, Small Icons, List, Details, Thumbnails, as Web Page | MS-shell, RK-keys | partial | Large Icons, List and Details (`apps/libdeskui/deskui.h:187`, `apps/files/app.c:72,497`). Missing: Small Icons and Thumbnails (Web view is out of scope) |
| FM-2 | Details columns Name, Size, Type, Modified (Attributes optional); click to sort; drag to resize; double-click to auto-fit | MS-shell | partial | G6: optional Attributes column (View menu), R/H/S/A values and sorting in `apps/files/app.c` / `model.c`; G1 local dates retained. Existing column widths still reset on navigation; auto-fit remains missing. |
| FM-3 | Arrange Icons by Name, Type, Size, Date; Auto Arrange; Line Up Icons | MS-shell | partial | "Sort by" in the View menu (`app.c:74-75,1421`); no Auto Arrange or Line Up |
| FM-4 | Address bar with drop-down history and autocomplete | RK-keys (F4) | partial | Typed path, Enter, Esc, F4 (`app.c:1504,1560-1564`); no history or autocomplete |
| FM-5 | Explorer two-pane view (folder tree) with F6 to switch panes, keypad +, − and * in the tree | RK-keys | partial | Places and Folders trees with splitter (`app.c:283-396`). Missing: hiding the tree (separate "Open" and "Explore" windows), keypad expand/collapse-all |
| FM-6 | Back, Forward, Up (Backspace), with history lists on Back and Forward | RK-keys | partial | `app.c:1389-1397,1506`, 32 entries (`model.h:13`); no drop-down lists |
| FM-7 | My Computer: drives with icons, type, total size and free space; Control Panel, Printers, Dial-Up Networking and Scheduled Tasks folders | MS-shell | partial | G6: Computer lists Capacity and Free Space using fx-17b MEDIA_GET_SPACE; calculating is explicit while the FAT census is pending. Existing icons/type/labels retained. Control Panel remains in Places; Printers, Dial-Up Networking and Scheduled Tasks folders remain absent. |
| FM-8 | Status bar: object count, selection size, free disk space | MS-shell | done | G6: `apps/files/app.c:files_update` shows object count, selection bytes and current volume free space (or calculating); `files_properties_test.c` checks the status text. |
| FM-9 | Toolbar: Back, Forward, Up, Cut, Copy, Paste, Undo, Delete, Properties, Views, Map/Disconnect Drive | MS-shell | partial | `app.c:186-190`; no Undo, Map or Disconnect |
| FM-10 | Menus: File, Edit, View, Go, Favorites, Tools, Help; View ▸ Toolbars, Status Bar, Explorer Bar, Folder Options | MS-shell | partial | File, Edit, View, Go, Help (`app.c:1618-1622`); no Favorites or Tools menu, no Folder Options |
| FM-11 | New ▸ Folder, Shortcut and registered document types | MS-shell | partial | Folder and Text Document (`app.c:1257-1268`); no Shortcut or Bitmap |
| FM-12 | Rename in place (F2, slow second click), with a warning when the extension changes | RK-keys | partial | `app.c:569-600`; no extension-change warning |
| FM-13 | Delete to the Recycle Bin with confirmation; Shift+Delete deletes at once | RK-keys, RK-disk | done | G2 (`docs/design/recycle-bin.md`): Files and the desktop send the selection to the bin after the Windows 98 confirmation; Shift+Delete, removable media and "do not move files" delete at once; `recycle_ui_test.c`, `desktop_recycle_test.h` |
| FM-14 | Cut, Copy, Paste files (Ctrl+X/C/V), shared with the desktop; cut items dimmed | RK-keys | done | `/system/settings/files.clip` (`apps/files/transfer.c:4`, `model.c`); `tests/host/desktop/files_transfer_test.c` |
| FM-15 | Paste Shortcut, Create Shortcut | MS-shell | absent | — |
| FM-16 | Copy To / Move To with a folder browser | MS-shell | absent | — |
| FM-17 | Undo (Ctrl+Z): undo the last rename, move, copy or delete | RK-keys | absent | No undo in `apps/files` or `apps/desktop/folder.c` |
| FM-18 | Select All (Ctrl+A), Invert Selection, Ctrl/Shift-click, Shift+arrows, Ctrl+Space, type-ahead | RK-keys | done | `model.c:427-483`, `app.c:1445-1446,1481-1516` |
| FM-19 | Rubber-band (marquee) selection | MS-shell | absent | A press on empty space only clears the selection (`list.c:501`). Directive fx-19 |
| FM-20 | Copy progress dialog: animation, file names, time remaining, Cancel | MS-shell | done | `apps/files/transfer_ui.h:13-34`; `files_transfer_test.c`. No animation |
| FM-21 | Confirm File Replace (Yes, Yes to All, No, Cancel, with sizes and dates); Confirm Folder Replace | MS-shell | done | `transfer_ui.h:61-79`, `model.c:747-840` |
| FM-22 | Long file names with 8.3 aliases; "MS-DOS name" shown in Properties | RK-disk (LFN) | done | G6: the file-info ABI returns the existing FAT 8.3 alias; General displays MS-DOS name. Native FAT12/16/32 and UI tests cover it. No alias is fabricated for volume roots or synthetic nodes. |
| FM-23 | Recycle Bin: deleted items keep their original location; Restore, Empty Recycle Bin, per-drive store, Properties (size limit %, "do not move files", confirmation), full and empty icons, drag onto it | RK-disk ("Managing the Recycle Bin"), MS-shell (FOF_ALLOWUNDO) | done | G2: `RECYCLED` + INFO2 per fixed volume (`apps/files/recycle.inc`), the bin as a Files folder (Original Location and Date Deleted columns), Restore (recreating folders), Empty, Delete, Properties (per drive or global size %, "do not move files", confirmation), full and empty icon, drops on the icon and on the bin window, Win32 `SHFileOperation` with `FOF_ALLOWUNDO` (`sdk/win32/shell/fileop.c`). Gaps: dragging out of the bin, Undo Delete (G8), the size limit uses an assumed 512 MiB capacity until G6 reports volume capacity, Win32 deletes do not trim the bin |
| FM-24 | File attributes Read-only, Hidden, Archive, System: shown and set in Properties; "Show all files", hide system files, hide known extensions (Folder Options ▸ View) | RK-disk, MS-shell | done | G6: R/H/A/S check boxes, mixed-selection edits, effective file Read-only, Attributes column, View / Folder Options (show hidden, hide system, hide known extensions). Browser filtering preserves raw recursive operations. The versioned shared view-settings file persists preferences for later windows; existing windows retain their own view (no live broadcast). |
| FM-25 | File Types (Folder Options): registered types with icon, actions (open, print, edit), default action, "Confirm open after download" | RK-apps ("Configuring Applications"), MS-shell | absent | Associations are a compile-time table (`apps/desktop/catalog.h:11-24`) |
| FM-26 | Open With dialog, with "Always use this program" | MS-shell | partial | Files offers only "Open with CiukNote" and "Play in Music Player" (`app.c:60,1291-1292`); the desktop has an Open With submenu (`shell.c:426-433`). No dialog |
| FM-27 | Double-click opens the document in its program, or runs a program (Win32, DOS, batch) | MS-shell | partial | Native ELF programs and catalogued documents (`app.c:994-1045`). Missing: `.exe`, `.com`, `.bat` and `.pif` (no PE or MZ route; they end in the "No program is registered" box) |
| FM-28 | Properties sheet: General tab (type, location, size, size on disk, MS-DOS name, created/modified/accessed, attribute check boxes); folder contents; program Version tab | MS-shell | done | G6: shared toolkit sheet, General metadata/size on disk/alias/local dates, worker-counted folder contents, editable attributes with Apply/OK/Cancel and read-only refusals. Version page reads bounded PE32 fixed and string resources. Host tests: `files_properties_test.c`, `files_version_test.c`, native `files_test.c`, G1 `localtime_test.c`. Version pages for NE/Win16 resources are not provided (Win16 is after the beta). |
| FM-29 | Drive properties: used/free pie, capacity, label edit; Tools tab (Check Now = ScanDisk, Backup Now, Defragment Now); Sharing tab | RK-disk | partial | G6: drive General with used/free/capacity, integer pie (suppressed while counting) and FAT label editing; Tools page explains unavailable utilities. Check Now/Backup Now/Defragment Now need G29 engines; Sharing needs file sharing. Free-space service currently covers FAT12/16/32, not ISO/NTFS. |
| FM-30 | Find: Files or Folders (named, containing text, look in with subfolders, date, type, size), Find Computer; F3 | RK-keys | absent | Only type-ahead selection (`model.c:465`) |
| FM-31 | Send To ▸ Floppy, Desktop (as shortcut), My Documents, Mail Recipient, Briefcase; user-extensible SendTo folder | RK-mobile | absent | — |
| FM-32 | Quick View (preview a file without opening its program) | RK-comp | absent | — |
| FM-33 | Format a floppy or removable disk (capacity, Quick/Full/Copy system files, label, summary) | RK-disk | absent | `sdk/libdisk/format.c` (FAT32) is used only by Setup; `fdc_format` is reached only from the DOS VM (`src/kernel/vm/dosvm_floppy.c:50`) |
| FM-34 | Copy Disk (floppy to floppy) | RK-disk | absent | — |
| FM-35 | Eject or safely remove removable media | RK-dev | done | `app.c:447-467,1374-1388` with a list of blocking programs; `tests/host/desktop/files_ui_test.c` |
| FM-36 | Map Network Drive / Disconnect; Sharing tab | RK-net | absent | No file sharing (NW-12) |
| FM-37 | Favorites menu and folder | RK-tb | absent | — |
| FM-38 | Shift+click on Close closes the folder and all its parents; Ctrl+double-click opens in a new window | RK-keys | absent | Files is one window per path |

## 10. Drag and drop

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| DD-1 | Drag between Files windows, the desktop and folders | MS-dnd | done | `client.c:747-754`, `apps/desktop/folder.c:219-231`, `apps/files/app.c:1682-1697`; `files_ui_test.c:354-379` |
| DD-2 | Default action: copy to another drive, move on the same drive; Ctrl forces copy, Shift forces move, Ctrl+Shift creates a shortcut; programs are linked by default | RK-keys (`wrkappg_1`) | partial | Move unless Ctrl is held, whatever the drive (`apps/files/app.c:1542`, `shell.c:2472`). Directives fx-17 and fx-19 |
| DD-3 | Right-drag shows a menu: Move Here, Copy Here, Create Shortcut(s) Here, Cancel | MS-dnd | absent | `list.c:488,500` |
| DD-4 | Drag image, cursor feedback (+ for copy, arrow for link, no-drop), drop-target highlight, auto-scroll, Esc cancels | MS-dnd | absent | `DU_EV_DRAG` fires only on release (`list.c:511,527`) |
| DD-5 | Drop files onto programs (Notepad, Paint, media player, DOS window) and onto Win32 windows (WM_DROPFILES) | MS-dnd | absent | `protocol.h:103` ("Files only"); `DragAcceptFiles` only sets a style (`sdk/win32/shell/utility.c:34-37`) |
| DD-6 | Drop onto the Recycle Bin, onto a printer, onto a taskbar button (the window comes forward after a hover) | MS-dnd | partial | Recycle Bin: the desktop icon and the bin window accept a move (confirmation, never a copy; CDs and the bin's own items refused), G2. Taskbar button hover is fx-19. Printer: absent (no printing, see G31) |
| DD-7 | A move whose source cannot be deleted keeps both copies and says so | MS-shell | absent | Directive fx-17 |

## 11. Clipboard

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| CB-1 | One system clipboard shared by every program, with several formats at once (text, bitmap, files, private formats) | MS-clip | partial | G5: one desktop-owned store with several formats, global sequence, owner, exclusive open, watchers and delayed render (2 s deadline) (`apps/libdeskui/clipstore.c`, `apps/desktop/clipboard.c`, `docs/design/system-clipboard.md`); `tests/host/desktop/test_system_clipboard.py`. DOS INT 2Fh AX=1700h programmatic clipboard still missing |
| CB-2 | Cut, Copy and Paste in every text field | RK-keys | done | `widgets.c:360-369`, `textarea.c:495-507`; `tests/host/desktop/text_selection_test.c` |
| CB-3 | Text between native and Win32 programs | MS-clip | done | G5: native UTF-8/LF to Win32 UTF-16/CRLF, ANSI/OEM synthesis and CF_LOCALE (`sdk/win32/user32/system_clipboard.c`); code pages beyond 1252/437/850 missing |
| CB-4 | Win32 formats CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT, CF_LOCALE, CF_BITMAP, CF_DIB, CF_HDROP, CF_METAFILEPICT, CF_PALETTE, registered formats, delayed rendering | MS-clip (Standard Clipboard Formats) | partial | G5: CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT, CF_LOCALE, CF_DIB, CF_BITMAP (eager), CF_HDROP, registered and private formats, WM_RENDERFORMAT/WM_RENDERALLFORMATS (`sdk/win32/user32/system_clipboard.c`). Missing: CF_PALETTE, CF_METAFILEPICT, CF_ENHMETAFILE, CF_OWNERDISPLAY, delayed CF_BITMAP |
| CB-5 | Copy and paste in DOS windows (toolbar Mark, Copy, Paste; QuickEdit) | RK-apps | partial | G5: DOS window menu Mark/Copy/Paste and QuickEdit, text and graphics selection, paced CP437 paste (`apps/dosbox/clipboard.c`, dosvm TEXT/MARK operations). Missing: DOS window toolbar and persisted properties (with G17) |
| CB-6 | Bitmaps between Paint and other programs | MS-clip | done | G5: Paint copies and pastes DIBs through the system clipboard, palette preserved (`apps/paint/paint.c`); `paint_clipboard_test.c`. RLE DIBs refused |
| CB-7 | Print Screen and Alt+Print Screen put a bitmap on the clipboard | RK-keys | partial | G5: see KB-10 (full-screen capture missing) |
| CB-8 | Clipboard Viewer accessory (view and save the clipboard as `.clp`) | RK-comp | done | G5: Clipboard Viewer accessory with text, image and file previews and Windows 3.1/9x `.clp` Open/Save As (`apps/accessories/clipbrd_main.c`, `apps/libdeskui/clipfile.c`); `clipboard_viewer_test.c` |

## 12. Common dialogs, message boxes and controls

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| CD-1 | Message boxes with icon, default button, sound (MessageBeep) | MS-cdlg | partial | Native `du_message` (`apps/libdeskui/dialog.c:234`) with icon sounds; Win32 `MessageBox` (`sdk/win32/user32/msgbox.c`). Missing: Win32 `MessageBeep` is a stub that fails (`user32/system.c:416-423`) |
| CD-2 | Open and Save As: Look in list, Up, New Folder, view modes, Files of type filter, default extension, long names, overwrite confirmation, read-only check box | MS-cdlg, RK-keys | partial | Native `du_file_dialog` (`dialog.c:375`) with Look in, Up, Places and name; `tests/host/desktop/dialog_places_test.c`. Missing in the toolkit: type filter, default extension, overwrite confirmation (apps do it themselves, inconsistently), New Folder, views, multi-select |
| CD-3 | Print and Page Setup dialogs | MS-cdlg | absent | No printing (PR-*) |
| CD-4 | Font dialog (family, style, size, effects, sample) | MS-cdlg | absent | CiukNote's Font box lists one face (`apps/ciuknote/app.c:468-469`) |
| CD-5 | Color dialog (basic colours, custom colours, RGB/HSL) | MS-cdlg | absent | Paint has a fixed palette |
| CD-6 | Find and Replace dialogs | MS-cdlg | partial | CiukNote has its own find and replace bar; no reusable dialog |
| CD-7 | Browse for Folder | MS-shell (SHBrowseForFolder) | absent | — |
| CD-8 | Win32 comdlg32 working for real programs | MS-cdlg | partial | Wine's comdlg32 is staged (`config/wine-upper.json`); host adapter run: filedlg crashed, finddlg 18 failures out of 43 (`wine-upper.json:2538+`); no guest run |
| CD-9 | Controls: button, check, radio, edit, list box, combo box, tree view, list view, tabs, progress, trackbar, up-down, tooltips, status bar, toolbar, rich edit, scroll bars, property sheets, wizards | RK-vmm ("User Interface") | partial | Native toolkit has buttons, check, radio, edit, text area, list view, tree, tabs, progress, status bar, toolbar and scroll bars (`deskui.h:187-205`). Missing natively: combo box, trackbar, up-down, tooltips, rich edit, property sheet and wizard frameworks. Win32: USER classes in `sdk/win32/user32/controls.c`; comctl32 and riched20 come from Wine with host-only evidence |
| CD-10 | Single-level undo in edit controls | MS-cdlg | partial | Text area and Win32 edit have one level (`textarea.h:45`, `controls.c:971`); the single-line native edit has none |

## 13. Fonts

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| FT-1 | TrueType rendering for the screen and programs | RK-print (Fonts) | done | FreeType 2.13.3 (`docs/design/fonts-contract.md`), `apps/libdeskui/text_ttf.c`, Win32 `sdk/win32/gdi32/font.c`; `tests/host/font/*` |
| FT-2 | Raster fonts (`.FON`: MS Sans Serif, System, Fixedsys, Terminal, Courier) | RK-print | partial | Emulated with Liberation faces (`fonts-contract.md:245-300`); `.FON`/`.FNT` are refused |
| FT-3 | Fonts folder: view, preview, install (File ▸ Install New Font), delete, "Hide variations", "List fonts by similarity" | RK-print, RK-cpl | partial | Read-only list with preview (`apps/control/fonts.c`). Missing: install and remove from the UI (Win32 `AddFontResource` works, contract lines 354-435) |
| FT-4 | Font smoothing ("Smooth edges of screen fonts", Effects tab) | RK-cpl | done (host) | G23 Effects toggles bitmap and FreeType coverage; Win32 SPI and GDI share the saved smoothing policy (`appearance.h`, `text_ttf.c`, `gdi32/text.c`) |
| FT-5 | Large Fonts / custom DPI | RK-acc | done (host) | G23: 96/120/custom 48–240 dpi; logical toolkit surfaces, presentation and input scale together; desktop frames, fonts, icons and spacing scale; reopened windows use the new DPI. `test_deskui.py`, `appearance_test.c`, fx-15 rows 43–48 |
| FT-6 | Font fallback and symbol fonts | RK-print | absent | No fallback family (`fonts-contract.md:63`) |
| FT-7 | Character Map: any font, full character set, copy | RK-comp | partial | `apps/accessories/charmap_view.c`: ASCII 32–126 in one face. Missing: font choice, Latin-1 and other ranges (Italian accented letters) |

## 14. Printing

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| PR-1 | Printers folder with Add Printer wizard and a default printer | RK-print, RK-cpl | absent | — |
| PR-2 | Printer drivers (universal driver, PostScript, PCL) | RK-print | absent (scope for Windows binaries) | Windows printer drivers are excluded (`docs/design/win32-personality-contract.md:85`); no native driver model |
| PR-3 | Spooler with EMF spooling, background printing, print queue window (pause, cancel, purge), deferred printing when offline | RK-print, RK-mobile, MS-print | absent | — |
| PR-4 | Print and Page Setup from every program (Notepad, WordPad, Paint, Imaging, browser) | RK-print | absent | No print code in `apps/` |
| PR-5 | Win32 printing (`CreateDC` for a printer, `StartDoc`, `EnumPrinters`) | MS-print | absent | `StartDocW` returns SP_ERROR (`sdk/win32/gdi32/upper.c:298-309`); winspool is stubs |
| PR-6 | Parallel port (ECP/EPP) and printing from MS-DOS programs, spooled | RK-print | partial | LPT driver with SPP/PS2/EPP/ECP (`src/kernel/drivers/lpt.c`); DOS INT 17h and virtual LPT (`src/kernel/vm/dosvm_lpt.c`); `tests/host/lpt_test.c`. Missing: spooling, print to file, hardware qualification |

## 15. Input devices and keyboard languages

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| IN-1 | PS/2 and USB keyboard and mouse, wheel | RK-dev | done (host) | `src/kernel/drivers/i8042.c`, `src/kernel/usb/usb_hid.c`; `tests/host/i8042_test.c`, USB tests |
| IN-2 | Keyboard properties: repeat delay, repeat rate, cursor blink rate | RK-cpl | absent | Fixed `INPUT_REPEAT_DELAY_MS 500` (`src/kernel/include/ciuki/input.h:28`); no typematic programming |
| IN-3 | Keyboard languages: several layouts, Left Alt+Shift or Ctrl+Shift switching, tray indicator | RK-intl, RK-cpl | partial | US and Italian layouts (`apps/desktop/keymap.c`, Win32 HKL 0x0409/0x0410 in `sdk/win32/user32/layout.c:129-176`) chosen in Control Panel. Missing: switching hotkey, indicator, more layouts |
| IN-4 | Dead keys and AltGr | MS-kbd | partial | AltGr done (`keymap.c:43`); no dead-key composition |
| IN-5 | Caps Lock, Num Lock and Scroll Lock LEDs | MS-kbd | partial | USB keyboards (`usb_hid.c:181,293,339,391`); no PS/2 LED command (0xED) |
| IN-6 | Mouse properties: swap buttons, double-click speed with test area, pointer speed, pointer trails, pointer schemes | RK-cpl, RK-acc | partial | Pointer speed and double-click time (`apps/desktop/settings.h:27-37,60`). Missing: button swap, trails, schemes, wheel lines; Win32 SPI values are per process and never saved |
| IN-7 | Game Controllers: add, calibrate, test | RK-cpl | done | `apps/control/joystick.inc` with three-point calibration; `tests/host/joystick/*` |
| IN-8 | Touchpad tapping and scrolling | RK-dev | partial | i8042 touchpad support (`tests/host/i8042_touchpad_test.inc`); no settings page |

## 16. Accessibility

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| AC-1 | Accessibility Options applet (Keyboard, Sound, Display, Mouse, General tabs) | RK-acc, RK-cpl | absent | Only an icon name (`apps/libdeskui/icon_map.c:292`) |
| AC-2 | StickyKeys, FilterKeys (SlowKeys, RepeatKeys, BounceKeys), ToggleKeys | RK-acc, MS-spi | absent | — |
| AC-3 | SoundSentry and ShowSounds | RK-acc | absent | Win32 `SPI_SETSHOWSOUNDS` is stored per process only (`sdk/win32/user32/system.c:250,352`) |
| AC-4 | High Contrast colour schemes and hotkey | RK-acc | absent | The Blue theme makes no accessibility claim (`docs/validation/f2-62-blue-theme.md:15`) |
| AC-5 | MouseKeys (numeric keypad moves the pointer) | RK-acc | absent | — |
| AC-6 | SerialKeys and alternative input devices | RK-acc | absent | — |
| AC-7 | Accessibility Wizard, Magnifier, large and inverted pointers, scalable interface elements | RK-acc | absent | — |
| AC-8 | Automatic reset after an idle time and the status indicator in the tray | RK-acc | absent | — |

## 17. Display

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| DS-1 | Display properties tabs: Background, Screen Saver, Appearance, Effects, Web, Settings | RK-cpl | partial | Background, Appearance and Settings (`apps/control/app.c:1203-1245`) |
| DS-2 | Background: wallpaper with Browse, pattern, desktop colour | RK-cpl | done (host) | G23: picture list/Browse, five placements, RGB colour and eight patterns use the durable preview/decode path (`control/app.c`, `settings.c`, `wallpaper.c`) |
| DS-3 | Screen Saver: list of savers, Settings, Preview, Wait minutes, password protection; started by the idle timer or SC_SCREENSAVE | MS-ssave, MS-syscmd, RK-comp (screen savers) | partial | G10: Blank, approved Ciuki portrait and Starfield, input-fed idle timer, full-screen Preview, enabled/wait controls, shared Win32 SPI and SC_SCREENSAVE (`docs/design/screen-saver.md`); host window-model coverage. Password protection remains G32; built-in savers have no extra options |
| DS-4 | Energy Star monitor power-off (from the Screen Saver tab) | RK-power, MS-syscmd (SC_MONITORPOWER) | partial | G10: Power-page monitor interval, SC_MONITORPOWER, serialized VBE 4F10 capability query/on/standby/off, retryable wake and full repaint. Host-tested through BIOS VM; physical panel/firmware qualification remains with lead |
| DS-5 | Appearance: colour schemes; per-item colour, size and font (title bars, menus, icons, message text); save and delete schemes | RK-acc ("scalable user interface elements") | done (host) | G23 Appearance editor: 20 elements, colours/gradients, sizes, built-in font faces, point size and styles; six built-ins and eight named user schemes with Save As/Delete; native paint and USER colour/metrics/NONCLIENTMETRICS queries share the saved value |
| DS-6 | Effects: desktop icon pictures, large icons, all-colour icons, show window contents while dragging, smooth fonts, animate menus | RK-cpl, MS-spi | done (host) | G23 Effects: large icons, Tango Documents/Recycle Bin picture choices, full-window/outline drag, smoothing and animated menus. The approved Ciuki identity remains pinned. Full-colour icons retain the existing ARGB renderer |
| DS-7 | Settings: resolution and colour depth from the driver's modes | RK-cpl | done | `apps/control/display.c:133-160`; `tests/host/desktop/control_modes_test.c`, `display_modes_test.c` |
| DS-8 | Apply a new mode with a 15-second "Keep this setting?" confirmation and automatic revert | RK-cpl | done | `display.c:10,220-241`; `control_display_wait_test.c` |
| DS-9 | Advanced: refresh rate, monitor type (EDID), font size (DPI), adapter and driver change, hardware acceleration slider | RK-cpl, RK-perf ("Setting Graphics Compatibility Options") | partial | G23 adds monitor identity and detailed timing capabilities through copied/validated VBE-DDC EDID, additive ABI queries and the layout oracle; refresh stays explicitly firmware default when actual timing is unknown. Advanced 96/120/custom DPI is implemented. Arbitrary refresh programming, adapter/driver change and hardware acceleration slider remain unqualified |
| DS-10 | Desktop Themes (wallpaper, sounds, pointers, screen saver and icons as one package) | RK-comp | done (host) | G23 Desktop Themes: built-ins and four named user packages, Save As/Delete, appearance/wallpaper/sounds/pointers/saver/icon pictures and sizes restored together through Apply/Cancel. Native package persistence in desktop.cfg; external Microsoft .theme file interoperability is not claimed |
| DS-11 | Display drivers for the common adapters of the period, with software fallback | RK-dev | done (host) | `docs/design/display-driver-model.md`, `video-driver-families.md`, `tests/host/*driver_test.c` |

## 18. Multimedia

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| MM-1 | Sounds applet: events list, sound per event with Browse and preview, named schemes (save, delete) | RK-cpl, RK-comp ("Multimedia Sound Schemes") | partial | 18 events with preview, Default and No sounds (`apps/desktop/sounds.h:20-24`, `apps/control/app.c:1262-1286`); `sounds_control_test.c`. Missing: Browse and per-event assignment, Custom and named schemes |
| MM-2 | System sounds played on events (start, exit, errors, minimise, menus, Recycle Bin) | RK-mm | done | `apps/desktop/sound_runtime.c`, `docs/design/system-sounds.md`; `sound_runtime_test.c`. G2: Empty Recycle Bin plays `empty_recycle` when it finishes, from the desktop and from Files |
| MM-3 | Volume Control: master plus Wave, MIDI, CD Audio, Line In, Microphone, with a Recording control set | RK-comp, RK-mm | partial | Master volume, balance and mute (`apps/accessories/mixer_view.c`, kernel `MIXER_GET/SET_MASTER`, `abi.h:1416-1419`). Missing: per-source channels and recording controls |
| MM-4 | Multimedia applet: preferred playback, recording and MIDI devices, CD volume, device list | RK-cpl | absent | Only a "Test audio hardware" button (`app.c:1285`) |
| MM-5 | CD Player: play, pause, stop, previous/next, eject, track list, time display, random and repeat, disc database | RK-comp | partial | Win32 MCI program `apps/desktop/cdplayer.c` (78 lines) with transport buttons; `tests/host/win32/media/cd_test.c`. Missing: track list, time display, play modes; it runs only through the Win32 personality |
| MM-6 | Media Player: WAV, MIDI, AVI, CD audio through MCI, with the selection bar and Copy Object | RK-comp, RK-mm | partial | Music Player plays WAV, MP3 and Ogg Vorbis (`apps/music/*`, `docs/design/music-player.md`). MIDI and AVI exist only as Win32 MCI for programs (`sdk/win32/audio/midi.c`, `mciavi.c`). Missing: a user program for MIDI and video |
| MM-7 | Sound Recorder: record from a microphone or line, edit, effects, save WAV | RK-comp, RK-mm | absent | No capture path for users: `waveInGetNumDevs` returns 0 (`sdk/win32/audio/wave.c:260`) |
| MM-8 | Audio compression (ACM) and video codecs (VfW) for programs | RK-comp, RK-mm | partial | ADPCM, Cinepak, MS RLE and MS Video 1 in `sdk/win32/audio/mciavi.c`; no MPEG |
| MM-9 | MIDI synthesis for Windows and DOS programs | RK-mm | partial | Win32 TinySoundFont synth (`sdk/win32/audio/synth.c`), DOS OPL3 (`src/kernel/audio/opl3.c`). No MPU-401 for DOS, no system MIDI port for native programs |
| MM-10 | DirectX (DirectDraw, DirectSound, DirectInput, Direct3D) for games | RK-mm | partial | DirectDraw, DirectSound (playback) and DirectInput on the host (`sdk/win32/ddraw`, `audio/dsound.c`, `input/directinput.c`); Direct3D absent (F7) |

## 19. Power management

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| PW-1 | APM 1.2 with the BIOS in charge (battery status, suspend requests, standby only when supported) | RK-power | partial | `src/kernel/core/apm.c` connects only when no ACPI RSDP exists (`apm.c:68,96,104,184`); `tests/host/apm_test.c`. Directive fx-18 extends it to firmware-owned ACPI machines (the T23) |
| PW-2 | ACPI with the OS in charge (OnNow), power and sleep buttons, thermal zones | RK-power | partial | `src/kernel/acpi` (ACPICA), events, `_PSR`, `_BST`, S5 (`docs/design/acpi-events-power.md`); ownership refused when unsafe (T23) |
| PW-3 | Power Management applet: power schemes (Home/Office Desk, Portable/Laptop, Always On) with monitor-off, disk-off and standby timers for AC and battery | RK-power | absent | The Power page is read-only status and a Shut Down button (`apps/control/app.c:1113-1118`) |
| PW-4 | Power meter (per battery, charge, time left) and the tray battery icon option | RK-power | partial | Text status (`app.c:1123-1160`) and a tray icon (`shell.c:1198-1214`); no graphical meter, no option, no tooltip |
| PW-5 | Low and critical battery alarms with action (message, sound, standby) | RK-power | absent | — |
| PW-6 | Standby (S1/S3 or APM suspend) from the Shut Down dialog, automatic after idle, resume restores devices | RK-power, MS-power | absent | "standby is unavailable" (`app.c:1114`); critical suspend leaves network, audio and USB stopped (`acpi-events-power.md:179-181`) |
| PW-7 | Hibernate (S4) where the BIOS supports it | RK-power | absent | — |
| PW-8 | Password on resume; power-button and lid-close actions | RK-power | absent | Lid state shown only (`acpi-events-power.md`) |
| PW-9 | Applications are told of power changes (WM_POWERBROADCAST) | MS-power | absent | Not in `sdk/win32/user32` |
| PW-10 | Soft power-off at shutdown | RK-power | done | SD-3 |

## 20. Date, time and regional settings

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| TM-1 | Date/Time applet: calendar and clock to set date and time | RK-cpl, MS-time (SetLocalTime) | done | G1: Date/Time Properties, Date & Time tab (calendar, clock, time edit, Apply = SetLocalTime through call 97, RTC written in local time); `apps/control/datetime.inc`, `tests/host/desktop/datetime_control_test.c`; [local-time.md](local-time.md) |
| TM-2 | Time Zone tab with a map and "Automatically adjust clock for daylight saving changes"; local time everywhere | RK-cpl, MS-time (GetTimeZoneInformation) | partial | G1: Time Zone tab (60 bundled IANA zones with their POSIX rules, "Automatically adjust clock for daylight saving changes"); the zone applies to the kernel, every launched program (`TZ`), libc, FAT stamps, kernel32 GetTimeZoneInformation/GetLocalTime and DOS INT 1Ah. No map |
| TM-3 | RTC kept in local time, with time synchronisation tools | RK-cpl | partial | G1: the RTC keeps local time (or UTC) with the bias recorded in clock.cfg version 2 and moved at every daylight-saving change, also after a change while switched off (`tests/host/proc/localtime_test.c`). No network time |
| TM-4 | Regional Settings: locale with number, currency, time and date formats, used by programs (GetLocaleInfo, GetDateFormat) | RK-intl, MS-time (NLS) | absent | Programs run with `LC_ALL=C`; Win32 locale code exists in `sdk/win32/kernel32/locale.c` but no user setting feeds it |

## 21. Storage and file systems

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| ST-1 | FAT12, FAT16 and FAT32 read and write, long file names | RK-disk | done | `src/kernel/fs/fat.c`; `tests/host/fs/test_fs.c`, `test_fat12.c`, `test_powercuts.c` |
| ST-2 | IDE/ATA disks with bus-master DMA, ATAPI CD-ROM | RK-dev | done | `src/kernel/drivers/ata.c`, `ata_dma.c`, chipset files; `tests/host/ata_test.c`, `atapi_test.inc`. ATAPI stays on PIO |
| ST-3 | Disk cache with write-behind, flushed at shutdown | RK-perf | done | `src/kernel/fs/cache.c`, `mount.c:679,949-964`; `tests/host/fs/test_cache_writeback.c`, `test_storage_power_flush.c` |
| ST-4 | CD-ROM file system (ISO 9660, Joliet) and audio CDs | RK-mm (CDFS) | done | `src/kernel/fs/iso9660.c`; `tests/host/fs/test_optical.c` |
| ST-5 | Floppy drive A:/B: read and write | RK-disk | done | `src/kernel/drivers/floppy.c`; `tests/host/floppy_test.c`, `floppy_fat_test.c` |
| ST-6 | USB mass storage, read-write when the medium allows it, hot plug, safe removal | RK-dev | partial | Driver done (`src/kernel/usb/usb_storage.c`), hot plug and removal (`usb_core.c:1038-1074`). Removable FAT volumes are published read-only (`src/kernel/fs/mount.c:420`) with no production read-write upgrade; a write error revokes writes (`fat.c:191`). Directive fx-17 |
| ST-7 | Drive letters, media change detection | RK-disk | done | `mount.c:407-413`, ATAPI and floppy media change; `tests/host/test_volume_resolution.py` |
| ST-8 | Fdisk: create and delete partitions and logical drives, set the active partition, large-disk support | RK-disk ("Disk Management") | partial | MBR/EBR read (`src/kernel/fs/partition.c`); Setup writes one whole-disk MBR (`sdk/libdisk`). Missing: a partition tool; partitions end at 2^28 sectors (`partition.c:13`) |
| ST-9 | ScanDisk: Standard and Thorough (surface scan), automatic fix, lost clusters saved as `FILEnnnn.CHK`, cross-linked files, report; Check Now on the drive Tools tab | RK-disk, RK-tools | partial | Automatic scan and repair on a dirty mount (`fat.c:1784-1815,1948-1967`). Missing: the program, any report, surface scan, `.CHK` salvage (lost clusters are freed), cross-link repair (the volume goes read-only) |
| ST-10 | Disk Defragmenter (with application-start optimisation) | RK-disk | absent | — |
| ST-11 | Disk Cleanup and the low-disk-space warning | RK-disk | absent | Only an ENOSPC message (`apps/files/app.c:133`) |
| ST-12 | Drive Converter (FAT16 to FAT32) | RK-disk | absent | — |
| ST-13 | DriveSpace 3 compression | RK-disk | absent (scope) | Unsupported by decision (`docs/design/cd-setup-plan.md:331`) |
| ST-14 | Microsoft Backup and System Recovery | RK-tools | absent | — |
| ST-15 | Virtual memory: swap file managed by the system, with a manual setting (System ▸ Performance) | RK-perf | partial | `src/kernel/fs/pagefile.c`, `C:/SYSTEM/PAGEFILE.SWP` (`mount.c:1094`); `tests/host/fs/test_pagefile.c`. No UI |
| ST-16 | Read other file systems (NTFS read-only is a CiukiOS goal, not Windows 98) | — | done | `src/kernel/fs/ntfs_ro.c`; `tests/host/fs/test_ntfs.c` |

## 22. Networking and Internet

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| NW-1 | Network adapter drivers (NDIS) | RK-net | partial | Native e100, rtl8139, ne2000, e1000, pcnet, tulip, 3c90x, rhine (`src/kernel/drivers/net.c:99-102`). The NDIS host has no qualified miniport (`src/kernel/ndis/profiles.c:7`) |
| NW-2 | TCP/IP with DHCP or a static address, mask, gateway, DNS | RK-net | done | lwIP (`src/kernel/net/lwip/stack.c`), `net/settings.c`; `tests/host/lwip/stack_test.c`, `net/settings_test.c` |
| NW-3 | Network applet (adapters, protocols, clients, services, bindings, identification) | RK-net | partial | Adapter, TCP/IP and Status tabs (`apps/control/network.inc:67-140`). Missing: clients, services, bindings, WINS |
| NW-4 | WinIPCfg (address, lease, Release, Renew) | RK-net | done | The Status tab of the Network page |
| NW-5 | `ping`, `tracert`, `netstat`, `ipconfig` commands | RK-net | absent | `LWIP_RAW 0`; Terminal `NET` prints only the driver state (`apps/terminal/commands.c:216`) |
| NW-6 | HOSTS and LMHOSTS files | RK-net (App. F) | absent | `sdk/libciuki/netdb.c:34` only knows `localhost` |
| NW-7 | Winsock for Windows programs (`wsock32.dll`, `ws2_32.dll`) | RK-net | absent | `wsock32` is generated stubs that fail with error 10050 (`sdk/win32/offline/wsock.c`, `scripts/win32/stubs.py:25`) |
| NW-8 | Web browser with history, favourites, downloads, new windows, printing, Internet Options | RK-net (`wrkc20_3`) | partial | NetSurf 3.11 with back, forward, reload, stop, home, bookmarks and history (`sdk/netsurf/port/chrome.c:39-105,280-281`). Missing: downloads (`chrome.c:372-383`), more than one window, PNG/JPEG decoding (off in `sdk/netsurf/build.py:23`), JavaScript, printing, an Internet applet; real pages are rejected by parser admission limits (directive fx-14) |
| NW-9 | E-mail and news client (Outlook Express) and address book | RK-comp | absent | — |
| NW-10 | FTP and Telnet clients | RK-net (`wrkc20_4`) | absent (scope) | Out of scope in `docs/design/network-contract.md:805` |
| NW-11 | Dial-Up Networking (modem, PPP), Dial-Up Server, Direct Cable Connection, VPN | RK-net (ch. 19), RK-comp | absent (scope) | PPP excluded (`network-contract.md:32`) |
| NW-12 | Client for Microsoft Networks, File and Printer Sharing, Network Neighborhood, mapped drives | RK-net (ch. 16, 18) | absent | — |
| NW-13 | IPX/SPX and NetBIOS for games and LANs | RK-net | partial | Virtual NE2000 and IPX bridge for DOS VMs (`src/kernel/vm/dosvm_ipx.c`, `dosvm_ne2000.c`); no native IPX, no NetBIOS |
| NW-14 | Internet Connection Sharing (Windows 98 SE) | — | absent (scope) | Routing and NAT excluded (`network-contract.md:32`) |
| NW-15 | Network status in the tray | — | done | TB-9 |

## 23. MS-DOS programs

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| DOS-1 | MS-DOS Prompt in a window and full screen, Alt+Enter switches | RK-apps, RK-vmm | done (host) | `apps/dosbox/dosbox.c`, `src/kernel/vm/dosvm_client.c:401`; `tests/host/dosvm_fullscreen_test.inc`. Defects on the T23 are tracked in fx-16 |
| DOS-2 | DOS window toolbar: font size list, Mark, Copy, Paste, Full screen, Properties, Background, Font | RK-apps | absent | `dosbox.c:261-312` handles only focus, input, close and full screen. The window has a fixed 640×480 surface (`abi.h:2221`) |
| DOS-3 | Program properties (PIF) for any MS-DOS program: Program (command line, working folder, batch file, shortcut key, Run, Close on exit, Advanced), Font, Memory (conventional, EMS, XMS, DPMI, protected), Screen (usage, restore settings, fast ROM emulation, dynamic memory), Misc (screen saver, mouse QuickEdit and exclusive, background, idle sensitivity, termination warning, fast pasting, reserved Windows shortcut keys) | RK-apps ("Configuring Applications") | partial | Game Properties for catalogued games only (joystick, COM, LPT modes; `apps/control/game_properties.inc:179-207`, `docs/design/game-properties.md`). Memory and IPX settings have file readers but no UI (`apps/dosbox/settings.h:35-64`, `network_settings.h:23`). Missing: the sheet for any program, and the Font, Memory, Screen and Misc tabs |
| DOS-4 | Background execution and idle sensitivity | RK-apps | partial | Global `--pause-background` (`dosbox.c:255`); idle detection through INT 2Fh/1680h and HLT (`src/kernel/vm/dosvm.c:560-592`). No per-program setting |
| DOS-5 | Closing a running DOS program: "Windows cannot shut down this program automatically" | RK-apps | done | G3: the DOS window answers `CLOSE_ACK DOS_ACTIVE` (window close) or `DOS_ACTIVE` (Shut Down) while its program runs (`apps/dosbox/dosbox.c`); the desktop shows the Windows 98 warning with Yes (end it) and No (keep it, or cancel the shutdown). The per-program "Warn if still active" setting is not offered: every DOS window warns |
| DOS-6 | Sound Blaster, AdLib, MPU-401 and General MIDI for games | RK-mm | partial | SB16 (`src/kernel/vm/dosvm_sb16.c`), OPL3 (`dosvm_opl.c`); no MPU-401 or GUS |
| DOS-7 | EMS, XMS, UMB, DPMI | RK-apps, RK-vmm | done (host) | `dosvm_ems.c`, `dosvm_xms.c`, `dosvm_umb.c`, `src/kernel/vm/dpmi*.c`; `tests/host/dosvm_dpmi_test.inc`. No VCPI |
| DOS-8 | Long file names for DOS programs (INT 21h 71xxh) and MSCDEX CD-ROM access | RK-disk (LFN), RK-mm | partial | MSCDEX 2.23 audio and a read-only D: view (`dosvm_mscdex.c`, `docs/design/unified-dos-contract.md:220-228`); 71xxh only in the legacy LFN.COM path (`src/lfn/lfn.c`) |
| DOS-9 | MS-DOS mode (Windows unloads; a real-mode session with its own CONFIG.SYS/AUTOEXEC.BAT) and "Restart in MS-DOS mode" | RK-apps | absent | The design keeps every DOS program in a V86 VM under the VMM (`docs/design/dos-dpmi-contract.md`). A CiukiOS equivalent would be an exclusive full-screen session with a per-program configuration, not a real-mode takeover |
| DOS-10 | Mouse in DOS programs; copy/paste of text | RK-apps | partial | INT 33h (`dosvm_mouse.c`); the wheel is dropped (`dosbox.c:302`); no clipboard (CB-5) |
| DOS-11 | Printing from DOS through the Windows spooler | RK-print | partial | INT 17h to a file or a physical LPT lease (`dosvm_lpt.c`); no spooler |
| DOS-12 | Serial ports and modems for DOS programs | RK-net (ch. 21) | partial | `dosvm_uart.c`; no modem |

## 24. Windows programs (Win32)

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| W32-1 | PE loader with DLLs and imports | RK-vmm ("Application Support") | done (host) | `sdk/win32/ldr/*`; `tests/host/win32/test_loader.py`. No original Windows game, installer or application has run yet (`README.md:185-186`) |
| W32-2 | kernel32, user32, gdi32, advapi32 | RK-vmm | partial | `sdk/win32/{kernel32,user32,gdi32,advapi32}`, host-tested; the console is log-only (`kernel32/console.c:1`) |
| W32-3 | shell32, comctl32, comdlg32, ole32, riched20, shlwapi | RK-vmm | partial | Reused Wine DLLs (`config/wine-upper.json`) with host-adapter evidence only; the guest case `w32-upper-dlls-qemu-t23` has not run |
| W32-4 | Multimedia (winmm, MCI, DirectSound, DirectDraw, DirectInput, OpenGL) | RK-mm | partial | MM-10; software OpenGL 1.1 (`sdk/win32/opengl32`) |
| W32-5 | Installers (InstallShield, Wise, NSIS, IExpress) and the uninstall list | RK-apps ("Installing Applications") | partial | `sdk/win32/ldr/installer.c`, `apps/w32srv/progman.c`, `restart.c`, Add/Remove (`apps/control/programs.c`); fixtures only (`tests/host/win32/installers`), no original installer has run; MSI is refused |
| W32-6 | Console programs in a console window | RK-apps | absent | No interactive console window |
| W32-7 | Win16 programs | RK-vmm | absent (scope) | NE parser only (`sdk/win16/ne`); moved after the 1.0 Beta (owner decision 2026-10-20) |
| W32-8 | Crash: "This program has performed an illegal operation and will be shut down" with Details | RK-tools | done | `apps/desktop/client.c:1592-1613`, `shell.c:2336,2424`; `tests/host/desktop/v3_test.c:570` |
| W32-9 | Integration: taskbar, Alt+Tab, clipboard, file drops, tray icons, taskbar flashing, shell folders | MS-taskbar, MS-dnd | partial | Taskbar, Alt+Tab, z-order and hotkeys through protocol v3 (`tests/host/desktop/test_v3.py`). Missing: clipboard bridge (CB-3), WM_DROPFILES (DD-5), tray (TB-5), flashing (WM-17), SendTo and Recent shell folders (`apps/w32srv/defaults.c:35-42`) |
| W32-10 | Shortcut (.lnk) properties: Target, Start in, Shortcut key, Run (normal, minimised, maximised), Change Icon, Find Target | MS-shell (Shell Links) | partial | CIUKLNK3 format (`apps/desktop/shell_link.h`), Start in honoured (`win32_native.c:175,238-240`). Missing: the dialog, hotkeys (IShellLink stubs, `sdk/win32/shell/link.c:62-72`), the stored `show` value is never applied (`win32_native.c:137,191`), `.ico` icons, target repair |
| W32-11 | Real applications and games run | RK-apps | absent | Every row of `docs/design/win32-compatibility-matrix.md` is unqualified |

## 25. Control Panel applets

The applet list is RK-cpl Table 23.1. The CiukiOS Control Panel has seven
categories and eleven applets (`apps/desktop/settings_catalog.h:13-36`). Each
row's detail is in the area section it points to.

| ID | Windows 98 applet | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| CP-1 | Accessibility Options | RK-cpl | absent | AC-1 |
| CP-2 | Add New Hardware (detection wizard, manual driver choice) | RK-cpl, RK-dev | absent | Device Manager can only install a network driver from the driver pack (`apps/control/driver_pack_ui.c:8-14`) |
| CP-3 | Add/Remove Programs: Install/Uninstall, Windows Setup (add or remove components), Startup Disk | RK-cpl | partial | Install/Uninstall only (`apps/control/app.c:1296-1330`, `programs.c`); `tests/host/desktop/test_add_remove.py`. No Windows Setup or Startup Disk tab; 96 entries at most |
| CP-4 | Date/Time | RK-cpl | partial | TM-1, TM-2 |
| CP-5 | Display | RK-cpl | partial | DS-* |
| CP-6 | Fonts | RK-cpl | partial | FT-3 (a tab inside Display, `app.c:1357-1358`) |
| CP-7 | Game Controllers | RK-cpl | done | IN-7 |
| CP-8 | Internet (home page, history, security, connections, programs) | RK-cpl | absent | NW-8 |
| CP-9 | Keyboard (speed, language, type) | RK-cpl | partial | Layout only (`app.c:1258-1264`); IN-2, IN-3 |
| CP-10 | Modems | RK-cpl | absent (scope) | NW-11 |
| CP-11 | Mouse (buttons, pointers, motion, general) | RK-cpl | partial | IN-6 |
| CP-12 | Multimedia | RK-cpl | absent | MM-4 |
| CP-13 | Network | RK-cpl | partial | NW-3 |
| CP-14 | Passwords | RK-cpl | absent | SS-2 |
| CP-15 | Power Management | RK-cpl | partial | PW-3, PW-4 |
| CP-16 | Printers | RK-cpl | absent | PR-1 |
| CP-17 | Regional Settings | RK-cpl | absent | TM-4 |
| CP-18 | Sounds | RK-cpl | partial | MM-1 |
| CP-19 | System: General, Device Manager (properties, resources, driver, disable, remove, refresh, print), Hardware Profiles, Performance (file system, graphics, virtual memory) | RK-cpl, RK-dev | partial | General, Device Manager (read-only tree with properties, resources, drivers, leases), Hardware check, Logs, Drivers (`app.c:1062-1112`, `docs/design/device-manager.md`); `tests/host/desktop/device_manager_test.c`. Missing: disable, enable, remove and update a device; Hardware Profiles; the Performance tab |
| CP-20 | Telephony | RK-cpl | absent (scope) | NW-11 |
| CP-21 | Users | RK-cpl | absent | SS-4 |
| CP-22 | Scanners and Cameras, Infrared | RK-cpl | absent (scope) | No such device class in the hardware baseline (`docs/design/hardware-baseline.md`) |

CiukiOS has applets that Windows 98 lacks: Storage and removable devices
(AutoPlay choices, `app.c:1323-1350`), per-game properties, and the Logs and
Hardware check tabs.

## 26. Accessories and games

The component list is RK-comp `[OptionalComponents]`.

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| AX-1 | Notepad: New, Open, Save, Save As, Page Setup, Print, Undo, Cut/Copy/Paste/Delete, Select All, Time/Date (F5), Word Wrap, Set Font, Find, Find Next; offers WordPad for files too large | RK-comp | partial | CiukNote has New/Open/Save/Save As, Undo, Cut/Copy/Paste, Find, Replace, Go To, Select All, F5, Word Wrap, Font, Status Bar (`apps/ciuknote/app.c:20-32`); `tests/host/desktop/ciuknote_test.c`. Missing: Page Setup and Print, a font choice (Regular or Bold of one face), a route to a larger-file editor above the 61,440-byte limit (`apps/ciuknote/doc.h:11`) |
| AX-2 | WordPad: rich text (RTF), Word 6 `.doc`, Write `.wri` and text; fonts, bullets, paragraph formatting, tabs, Insert Object, print preview | RK-comp | absent | — |
| AX-3 | Paint: tools (free-form and rectangle select, eraser, fill, pick colour, magnifier, pencil, brush, airbrush, text, line, curve, rectangle, polygon, ellipse, rounded rectangle); Image ▸ Flip/Rotate, Stretch/Skew, Invert Colors, Attributes, Clear; edit colours; BMP, GIF and JPEG; Set As Wallpaper; print; clipboard | RK-comp | partial | CiukPaint has the 15 tools, flip/rotate, invert, attributes, undo, zoom and grid, clipboard (`apps/paint/paint.h:10-11`, `paint.c:812`); `tests/host/desktop/paint_test.c`. Missing: Stretch/Skew, Edit Colors (fixed 144-colour palette, `io.h:5`), saving GIF/JPEG (saves BMP8, BMP24, PNG, `io.c:153-154`), Set As Wallpaper, print, the shared clipboard (CB-6) |
| AX-4 | Calculator: Standard and Scientific views, memory, digit grouping, copy and paste | RK-comp | partial | Standard only, memory, copy/paste, grouping (`apps/accessories/calc_view.c:86-130`); no Scientific view |
| AX-5 | Character Map | RK-comp | partial | FT-7 |
| AX-6 | Imaging (Kodak): view, rotate, zoom, annotate, scan, print, page thumbnails | RK-comp, RK-print (Imaging) | partial | Image Viewer: PNG, JPEG, BMP and GIF, zoom, fit, next and previous (`apps/accessories/viewer_*`); no rotate, annotation, print or Set As Wallpaper |
| AX-7 | Phone Dialer | RK-comp | absent (scope) | No modem support |
| AX-8 | HyperTerminal (serial and modem terminal with transfer protocols) | RK-comp | absent | Native COM ports (`src/kernel/drivers/com.c`, `com_native.c`) and virtual DOS UARTs (`src/kernel/vm/uart16550.c`) exist, but there is no terminal program for them |
| AX-9 | Sound Recorder, Media Player, CD Player, Volume Control | RK-comp | partial | MM-3, MM-5, MM-6, MM-7 |
| AX-10 | Clipboard Viewer | RK-comp | absent | CB-8 |
| AX-11 | Games: Solitaire, FreeCell, Hearts, Minesweeper | RK-comp ("Games") | absent | No card or board games in `apps/` |
| AX-12 | Briefcase (synchronise files with a portable computer) | RK-comp, RK-mobile | absent | — |
| AX-13 | Address Book, Outlook Express, NetMeeting, FrontPage Express, Chat | RK-comp | absent (scope) | Not in the 1.0 Beta goals |
| AX-14 | Windows Scripting Host (`.vbs`, `.js`) | RK-comp | partial | CiukiOS runs Lua scripts in the Terminal (`apps/terminal/lua.c`); there is no Windows Script Host |
| AX-15 | Clock (a Windows 3.x leftover, not in Windows 98) | — | done | `apps/accessories/clock_*`; local time of the system zone since G1 |
| AX-16 | MS-DOS Prompt | RK-comp | done | DOS-1 |
| AX-17 | Command prompt for the native system | — | partial | Terminal with built-in commands and Lua (`apps/terminal/commands.c`, `docs/terminal.md`). Missing: pipes, redirection, batch files, and the `FORMAT`, `CHKDSK`, `ATTRIB`, `LABEL`, `PING` and `IPCONFIG` commands; `RUN` and `REG` are stubs (`commands.c:304`) |

## 27. System tools

The tool list is RK-tools Table 27.1 plus the System Tools menu of RK-comp.

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| SY-1 | System Information (MSInfo): System summary, Hardware Resources (conflicts, DMA, I/O, IRQ, memory), Components, Software Environment, Tools menu | RK-tools | partial | Control Panel System tabs and the Hardware Report app with export (`apps/hwreport`, `docs/design/hardware-report.md`); `tests/host/hwreport_test.c`. Missing: a browsable resources and components tree in one tool |
| SY-2 | System Monitor (counters: CPU, memory, file system, network, swap) | RK-perf | partial | Task Manager Performance tab: CPU history and memory (`apps/tasks/view.c:61-64,196-231`). Missing: selectable counters, disk and network |
| SY-3 | Resource Meter (System, User, GDI resources in the tray) | RK-comp | absent | — |
| SY-4 | Close Program / task list with End Task | RK-tools | done | Task Manager (`apps/tasks`), SD-5 |
| SY-5 | Device Manager | RK-dev | partial | CP-19 |
| SY-6 | Registry Editor (find, edit, import and export `.reg`) | RK-reg | done | `apps/regedit/app.c`; `tests/host/desktop/regedit_test.c`. It edits the CiukiOS registry tree; the Win32 HKLM view goes through `apps/w32srv/registry.c` |
| SY-7 | Scheduled Tasks (Task Scheduler) and Maintenance Wizard | RK-tools | absent | — |
| SY-8 | Dr. Watson: logs every program fault with a snapshot | RK-tools | done | G13: a native fault is logged (`[fault] native ...` in BOOT.LOG) and written as `/system/logs/CRASH0-7.TXT` by the desktop (`apps/desktop/win32_crash.c`, rotation of eight, registers, fault address, stack words); Win32 faults keep `/WINDOWS/CRASH/CRASH0-7.TXT`; both are listed by the Log Viewer (`docs/design/fault-reporting.md`) |
| SY-9 | System File Checker, Signature Verification, Version Conflict Manager | RK-tools | absent | BT-12 |
| SY-10 | Registry Checker | RK-reg | partial | BT-7 |
| SY-11 | System Configuration Utility | RK-tools | absent | BT-11 |
| SY-12 | Windows Report Tool and MS-DOS Report Tool (collect a system report for support) | RK-tools | done | Hardware Report export (`apps/hwreport/report.c`) and the Setup Rescue copy of the logs to USB (`apps/setup/rescue.c:64-95`) |
| SY-13 | Disk tools (ScanDisk, Defragmenter, Cleanup, Drive Converter, Backup) | RK-disk | partial | ST-9 to ST-14 |
| SY-14 | Windows Update | RK-tools | absent (scope) | `docs/design/win32-personality-contract.md:89` |
| SY-15 | Developer tools (not in Windows 98): C compiler, IDE | — | done | `apps/tcc`, `apps/ciukide`; `tests/host/desktop/ciukide_test.c` |

## 28. Help

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| HP-1 | Windows Help: Contents, Index, Search, with topics for every component and troubleshooters | RK-tools ("Troubleshooting Procedures") | partial | CiukiOS Help with 10 topics (`apps/accessories/help_topics.c`); `tests/host/desktop/help_test.c`. Missing: index, search, troubleshooters, topics for each program |
| HP-2 | F1 in any program opens that program's help | RK-keys | partial | KB-11 |
| HP-3 | What's This? (Shift+F1, the "?" caption button in dialogs) | RK-keys, MS-syscmd (SC_CONTEXTHELP) | absent | — |
| HP-4 | Tooltips on toolbar buttons and controls | RK-vmm | partial | Taskbar and tray only (`shell.c:1017-1039`); none in `apps/libdeskui` |
| HP-5 | WinHelp/HTML Help for Windows programs (`WinHelp`, `.hlp`, `.chm`) | RK-apps | absent | No WinHelp engine |

## 29. Error reporting and logging

| ID | Feature: Windows 98 reference behaviour | Ref | Status | CiukiOS evidence / what is missing |
| --- | --- | --- | --- | --- |
| ER-1 | A faulting program shows a dialog ("…has performed an illegal operation", Details) and the system keeps running | RK-tools | done | G13: `native_fault_report` (`src/kernel/proc/win32/diagnostics.c`) queues the fault for the desktop, which shows "This program has performed an illegal operation and will be shut down." with Close and Details (`apps/desktop/client.c` `desk_w32_fault_dialog`, `fault_text.h`); Win32 programs: W32-8 |
| ER-2 | Fatal system error screen with the faulting module, and an option to continue or restart | RK-vmm | done | G13: `panic`, `panic_frame` and the double fault draw the Windows 98 text ("A fatal exception 0E has occurred at cs:eip in task ...") and wait for Enter or Ctrl+Alt+Del (restart through `kernel_reboot`) or Esc (stay) (`src/kernel/core/fatal.c`); the PS/2 decoder handles set 1 and set 2. USB-only keyboards have no choice and the screen stays |
| ER-3 | Desktop (shell) crash recovers by restarting the shell | RK-vmm ("User Interface") | done | G13: `supervisor_poll` restarts the bootstrap desktop after an abnormal death once the old grants are free, at most three times a minute, then falls back to the console (`src/kernel/proc/supervisor.c`); the new desktop shows the crash dialog of the old one. Other programs' windows end with the desktop (their channel closes); `TaskbarCreated` re-registration is not modelled |
| ER-4 | Startup and setup logs (`BOOTLOG.TXT`, `SETUPLOG.TXT`, `DETLOG.TXT`) | RK-start | done | BT-6, BT-13 (`SYSTEM/SETUP.LOG`) |
| ER-5 | Log viewer for the user | RK-tools | done | G13: Log Viewer accessory (`apps/accessories/logview_*.c`, Start > Programs > System Tools): boot log, NetSurf log, Setup log and the native and Win32 crash files, last 60 KB of large files, refresh, copy |
| ER-6 | Low-resource and low-disk warnings | RK-disk | absent | ST-11 |

## Summary of the inventory

There are 344 rows. The Control Panel table (22 rows) mostly points back to
rows in the area sections, so the counts are given with and without it.

| Status | All rows | Without the Control Panel table |
| --- | ---: | ---: |
| done | 64 | 63 |
| partial | 148 | 138 |
| absent | 132 (15 of them out of scope by an existing decision) | 121 (12 out of scope) |
| **total** | **344** | **322** |

The pattern across areas:

- **Strong:** the kernel services under the desktop. That means storage,
  caching, the FAT, ISO and NTFS file systems, display modes with the revert
  timer, TCP/IP and DHCP, the DOS memory managers and DPMI, the window
  lifecycle, the cursors, Alt+Tab and crash isolation.
- **Weak:** the shell services a user meets in the first minutes. That means
  local time, the Recycle Bin, property sheets, Find, Undo, Send To, the
  taskbar menus and Windows-key shortcuts, a single clipboard, the screen
  saver, and power policy.
- **Missing:** whole subsystems. Printing, accessibility, users and logon,
  regional settings and file sharing do not exist yet.

## Stale statements found while surveying

These statements contradict the code. The directive that next touches each
area should correct it.

- `apps/desktop/shell.c:1468`: an unreachable branch of `icon_open` says
  "CiukiOS does not keep deleted files yet: Files deletes them permanently".
  Every built-in icon is `SH_ACT_LAUNCH`, so this text never shows. The desktop
  does move items to `/Documents/Trash` (`folder.c:95-113`), while Files still
  deletes permanently.
- `docs/desktop.md:1002` says the clipboard is per process. The toolkit has
  used a shared file-backed text store since then (`apps/libdeskui/widgets.c:21-81`).
- `docs/desktop.md:725` says there is no parallel-port driver. One exists:
  `src/kernel/drivers/lpt.c`.
- `apps/libdeskui/recent.h` says the Start menu reads `recent.lst`. The shell
  reads `cfg.recent` (`shell.c:397-405`).
- `apps/desktop/shell.c:777`: Properties on a Win32 `.lnk` desktop icon
  (`SH_ACT_WIN32_LINK`) shows "Type: System folder" and a note about the
  Trash. Computer, Documents, Trash and Pictures are shown as "Shortcut" to
  `/Programs/files` (`shell.c:774-776`).
- `docs/design/desktop-interaction-audit.md` (2026-10-15) marks the desktop
  icons and taskbar rows "ok". The rows DT-3 to DT-7 and TB-2 to TB-5 above
  are not.

## Prioritised gap list (directive-sized work items)

### How the list is ordered

Items are ranked by what a user on the T23 meets in the first ten minutes.
The session that sets the order is:

1. boot, then the desktop appears;
2. look at the clock;
3. open Files and a few programs, move and arrange the windows;
4. plug in a USB stick and copy files;
5. delete something;
6. copy text between programs;
7. look at a file's properties;
8. try the Windows-key shortcuts and the taskbar menu;
9. leave the machine idle;
10. play DOOM;
11. unplug the charger;
12. shut down.

Within the same moment, an item that can lose data or show wrong information
comes before an item that is only missing.

### Size scale

Sizes include the host tests:

- **S:** one directive, up to about 800 changed lines.
- **M:** one directive, about 800 to 2,500 lines.
- **L:** two or three directives.
- **XL:** a programme of several directives.

### Already written (do not duplicate)

These directives were written from the trial of image `e831e685`. The items
below assume they land first.

| Directive | Covers rows |
| --- | --- |
| fx-14 NetSurf parser admission limits | NW-8 (real pages) |
| fx-15 Whole window-drawing system, launch speed | WM-2, WM-22 |
| fx-16 DOOM stutter and audio crackle | DOS-1, DOS-6, WM-18 on the T23 |
| fx-17 USB read-write mount, cross-drive copy/move, write errors | ST-6, DD-2, DD-7 |
| fx-18 Battery, AC and power data through APM when ACPI is not owned | PW-1, TB-8, PW-4 (data) |
| fx-19 Drag and drop everywhere, rubber-band selection | DT-4, DT-11, FM-19, DD-1 to DD-6 |

### New work items, highest user impact first

| # | Title | Scope (rows) | Dependencies | Size | Why it ranks here |
| ---: | --- | --- | --- | --- | --- |
| G1 | Local time, time zone and DST; set date and time | TM-1, TM-2, TB-6 (double-click, calendar), AX-15, FM-2 (local dates). A kernel or desktop call to set the RTC outside Setup; a tz database subset with EU and US DST rules applied to the `TZ` of every launched program, including Win32 `GetTimeZoneInformation`; a Date/Time page with calendar, time and time-zone tabs; the tray double-click | none | M | The clock is wrong on first sight: it shows UTC, which is two hours off in Italy in summer, and the user cannot fix it |
| G2 | Recycle Bin service | FM-13, FM-23, DT-1, DD-6 (bin drop), MM-2 (event), DT-5 (Shift+Del), W32 `SHFileOperation` FOF_ALLOWUNDO. A per-volume bin with original-path records (`INFO2`-like); Delete in Files and on the desktop goes to the bin, Shift+Delete bypasses it; Restore, Empty, Properties (size limit, "do not move to bin", confirmation); full and empty icon; delete confirmation everywhere | fx-19 for the drop target | M | A Delete in Files destroys data at once, unlike every Windows since 95. **Done 2026-10-23** (`docs/design/recycle-bin.md`) |
| G3 | Shutdown and end of session without data loss | SD-1, SD-2, SD-5, DOS-5, WM-16. No SIGTERM while a program is showing a save prompt (the program answers "busy" and the dialog waits for the user); a "This program is not responding" End Task dialog; WM_QUERYENDSESSION/WM_ENDSESSION to Win32 programs from the desktop's Shut Down; the DOS "cannot shut down automatically" choice; a second Ctrl+Alt+Del in Task Manager offers a restart **Delivered by G3 on 2026-10-23** (`build/codex/g3-report.md`); remaining: Stand by, MS-DOS mode restart, Log Off UI (G11, DOS-9, G32) | none | M | Shutting down with an unsaved document can lose it after 3 s |
| G4 | Taskbar menu, window arrangement and Windows-key shortcuts | WM-11, WM-12, WM-13, KB-5, KB-8, KB-9, KB-12, KB-13, SM-9. A taskbar context menu with Cascade, Tile Horizontally, Tile Vertically, Minimize All, Undo Minimize All and Properties; Show Desktop; Win+D/M/Shift+M/E/R/F/Tab/F1/Break; Alt+M; mnemonics in the shell menus; a caption icon menu and double-click to close; taskbar keyboard focus; Win32 `CascadeWindows`/`TileWindows`, SIZE_MAXIMIZED, WM_GETMINMAXINFO limits | none | M | The first right-click on the taskbar and the first Windows-key shortcut do nothing |
| G5 | One system clipboard | CB-1, CB-3, CB-4, CB-5, CB-6, CB-7, CB-8, KB-10. One desktop-owned store with formats: text and Unicode with locale, DIB, a file list exposed as CF_HDROP, private formats. Bridges to the native toolkit, w32srv and DOS windows (Mark/Copy/Paste and INT 2Fh 1700h–1705h); Paint on the shared store; Print Screen and Alt+Print Screen; a Clipboard Viewer | DOS window toolbar (G17) for Mark | L | Copying text from the browser or a Win32 program into CiukNote fails, and so does any paste into a DOS window |
| G6 | Property sheets, file attributes and disk space | FM-2 (Attributes column), FM-7, FM-8, FM-22, FM-24, FM-28, FM-29, KB-6, DT-5 (Alt+Enter). ABI fields for attributes, created and accessed times, and volume capacity and free space; a toolkit property-sheet frame; File, Folder and Drive sheets (General with attribute check boxes and the MS-DOS name; a drive pie chart and label; Tools tab); the "show hidden files" and "hide extensions" options | ABI review (`abi_layout` rows) | L | Right-click ▸ Properties shows a plain message box; free space appears nowhere |
| G7 | Find Files or Folders | FM-30, SM-6, KB-12 (Win+F), FM-10. Search by name with wildcards, by contained text, look-in with subfolders, date, size and type; a results list with the usual file actions; F3 in Files and on the desktop; a Start ▸ Find entry | G6 for the date and size fields | M | There is no way to find a file the user saved somewhere |
| G8 | File operations completeness | FM-11, FM-15, FM-16, FM-17, FM-25, FM-26, FM-27, FM-31, DT-6, DT-7, SM-8 (shared launcher). Undo for rename, move, copy and delete; Copy To and Move To; Send To with a user SendTo folder; Create Shortcut and Paste Shortcut; New ▸ Shortcut and Bitmap; registry-backed file types shared with Win32 (`HKCR`) with an Open With dialog and "always use"; launching `.exe`, `.com`, `.bat` and `.pif` from Files and the desktop | G2 for undoing a delete; G6 | L | The usual Explorer menu items are missing, and a Windows program cannot be started by double-clicking it |
| G9 | Run dialog | SM-8, KB-12 (Win+R). Command history drop-down, Browse, absolute paths, documents (through G8 associations), folders, URLs (NetSurf), Win32 and DOS programs | G8's launcher | S | Run accepts only names in `/Programs` |
| G10 | Screen saver and monitor power | DS-3, DS-4, SS-3 (password later). An idle timer fed by input; the SC_SCREENSAVE path and SPI_GET/SETSCREENSAVE*; three savers (Blank, the Ciuki logo, a starfield) with preview and wait time; VBE/DPMS monitor off from the Power page | none | M | G10 implemented (host): saver, shared policy and monitor-power paths; physical DPMS qualification remains; SS-3 password stays G32 |
| G11 | Power policy after fx-18 | PW-3, PW-4, PW-5, PW-6, PW-8, PW-9, SD-1 (Stand by), SD-7, TB-8 (tooltip). Power schemes (monitor off, disks off, standby after, AC and battery); low and critical alarms with action; tray tooltip with percent and time; a power-button and lid action choice; WM_POWERBROADCAST; Stand by where APM 1.2 or ACPI S1/S3 is qualified, with device resume (network, audio, USB) | fx-18 | L | On battery, the user gets no warning before the machine dies |
| G12 | Keyboard, language and mouse settings | IN-2, IN-3, IN-4, IN-5, IN-6, TB-11, WM-21. Repeat delay and rate (PS/2 0xF3 and USB repeat), cursor blink; PS/2 LEDs (0xED); Italian dead keys; a Left Alt+Shift layout switch with a tray indicator; button swap, pointer trails, wheel lines and pointer schemes (large and inverted); Win32 SPI values persisted | none | M | Italian typists need the dead keys and repeat rate; left-handed users need the button swap |
| G13 | Native fault dialog, shell restart, log viewer | ER-1, ER-2, ER-3, ER-5, SY-8. A dialog for native program faults with Details and a crash file; the desktop restarted by the supervisor after a crash; a formatted fatal-error screen with a restart choice; a log viewer for BOOT, NETSURF and CRASH files | none | M | A crashing native program just vanishes, and a desktop crash leaves a text console |
| G14 | Startup menu, ScanDisk and registry backups | BT-2, BT-3, BT-7, BT-8, BT-10, BT-11, BT-12, ST-9, SY-10. Ctrl/F8 hold and an automatic menu after a failed boot; Step-by-step and a "command prompt only" boot (Terminal only); safe mode with storage; a visible ScanDisk on a dirty boot and on demand (report, `.CHK` salvage, cross-link repair); five dated registry backups with restore at boot; a file checker against `build-manifest.json`; a persistent skip list | none | L | A failed boot today gives no repair path |
| G15 | Help everywhere | HP-1 to HP-4, KB-11, SM-7. F1 opens the program's topic in Help; index and search; Shift+F1 and a "?" caption button with WM_HELP; toolkit tooltips; topics for every built-in program; Start ▸ Help opens Help | none | M | F1 shows a one-line box instead of help |
| G16 | Common dialogs and controls | CD-1, CD-2, CD-4, CD-5, CD-6, CD-7, CD-9, CD-10, CD-8. Toolkit Save As with type filter, default extension and overwrite confirmation (removing the per-app copies); New Folder; Font, Color, Browse for Folder, Find and Replace dialogs; combo box, trackbar, up-down and tooltip widgets; `MessageBeep`; comdlg32 file-dialog crash fixed and run in the guest | none | L | Saving a file behaves differently in each program |
| G17 | DOS window and program properties | DOS-2, DOS-3, DOS-4, DOS-10, CB-5, DOS-9 (CiukiOS equivalent). Window toolbar (Mark, Copy, Paste, Full screen, Properties, Background, font size); a properties sheet for any DOS program (Program, Font, Memory, Screen, Misc), stored per program; mouse wheel; an exclusive full-screen session with its own configuration as the MS-DOS mode equivalent | G5 for Copy/Paste | L | DOS games cannot be configured unless they are in the catalogue |
| G18 | Win32 shell integration | TB-5, TB-10, WM-17, W32-9, W32-10, DD-5, SM-2. A tray icon protocol and `Shell_NotifyIcon`; `FlashWindow` and taskbar flashing; WM_DROPFILES from desktop drags; Win32 Start Menu folders as submenus; shortcut properties, hotkeys, Run minimised/maximised, `.ico` icons, target repair; the SendTo and Recent shell folders | fx-19, G8 | L | Installed Windows programs appear flat in the menu and lose their tray icons |
| G19 | Desktop icon management beyond fx-19 | DT-3, DT-5, DT-6, DT-8, DT-9. Arrange by name, type, size and date; Auto Arrange; type-ahead, Home/End and Ctrl+A on the desktop; New ▸ Bitmap and Shortcut on the menu; renaming system icons; wallpaper Browse and "Set as wallpaper" from Paint, the Viewer and NetSurf | fx-19 | M | Icons cannot be sorted, and pictures cannot become the wallpaper |
| G20 | Taskbar and Start menu customisation | TB-2, TB-3, TB-4, SM-1, SM-3, SM-4, SM-5. Taskbar Properties (always on top, auto-hide, clock, small icons); Quick Launch with Show Desktop; Start menu editing (add, remove, drag, Clear Documents, 15 items read from the apps' recent lists); Settings ▸ Taskbar & Start Menu and Folder Options | G4 | M | The user cannot pin a favourite program |
| G21 | Startup programs | Implemented (G21; `docs/design/startup-programs.md`); BT-11 configuration-file/boot-menu controls remain G14. SS-5, BT-11. A StartUp folder and the Run and RunServices keys at every desktop start, with Shift held to skip; an msconfig-like startup page in System | G18 for Win32 entries | S | Programs that expect to start at logon (tray tools, players) do not |
| G22 | Multimedia completion | MM-1, MM-3, MM-4, MM-5, MM-6, MM-7, MM-9, AX-9. Sound schemes with Browse; per-source mixer channels (wave, MIDI, CD, line, mic) and recording controls; a Multimedia page; a capture path and Sound Recorder; a native Media Player front-end for MIDI and AVI over the existing MCI code; CD Player track list and time; tray volume double-click | audio driver capture support (per driver family) | L | Only the master volume can be changed |
| G23 | Display appearance and DPI | DS-2, DS-5, DS-6, DS-9, DS-10, FT-4, FT-5. A scheme editor (per-item colours, sizes, fonts), saved schemes; Effects (full-window drag, smoothing, large icons); refresh rate and EDID monitor; Large Fonts / DPI; a desktop colour picker and patterns | none | L | **Implemented (host):** Appearance schemes/items, Effects, DPI, native Desktop Theme packages and monitor/refresh capability ABI. Hardware timing selection and external .theme interoperability are not claimed; sanitizer completion limitations are recorded in build/codex/g23-report.md |
| G24 | Regional Settings | TM-4, CP-17. Locale with number, currency, date and time formats; `LC_*` for native programs and NLS for Win32 (`GetLocaleInfo`, `GetDateFormat`); Italian and English defaults | G1 | M | Dates and numbers follow neither the Italian nor the US convention |
| G25 | Accessibility | AC-1 to AC-8, KB-17, WM-21. Accessibility Options applet: StickyKeys, FilterKeys, ToggleKeys, MouseKeys, SoundSentry, ShowSounds, High Contrast schemes and hotkeys, idle reset, tray indicator; large and inverted pointers; a Magnifier | G12, G23 | L | Required for users with disabilities; nothing exists |
| G26 | Network user tools | NW-5, NW-6, NW-7, NW-8 (downloads, windows, PNG/JPEG), CP-8. Raw ICMP and `ping`/`tracert`/`ipconfig`; a HOSTS file; Winsock 1.1/2 over the native sockets for Win32 programs; NetSurf downloads, new windows and PNG/JPEG decoding; an Internet page (home page, proxy, cache) | fx-14 | L | Win32 programs cannot reach the network, and a downloaded file cannot be saved |
| G27 | Accessories completion | AX-3, AX-4, AX-5, AX-6, AX-8, AX-11, AX-17, FT-7. Paint Stretch/Skew, Edit Colors, GIF/JPEG save, Set As Wallpaper; Calculator Scientific; Character Map fonts and Latin-1/Unicode ranges; Viewer rotate and wallpaper; a serial terminal; Solitaire, Minesweeper, FreeCell and Hearts equivalents; Terminal pipes, redirection, batch files and the missing commands | G5 for the clipboard parts | L (split per accessory) | Each accessory is reachable from the menu and shows its gap immediately |
| G28 | WordPad-class editor | AX-1 (large files), AX-2. A rich-text editor (RTF and text, later `.wri`/`.doc` import) with fonts, paragraphs, bullets, print preview; CiukNote offers it above its 60 KiB limit | G16 (Font dialog), G31 for printing | L | Documents larger than 60 KiB cannot be opened |
| G29 | Disk tools | FM-33, FM-34, ST-8, ST-10, ST-11, ST-12, ST-14, ER-6. A Format dialog (floppy, USB, FAT12/16/32, quick or full, label); Disk Cleanup and the low-disk warning; later a defragmenter, Drive Converter, a partition tool and Backup | fx-17 (removable rw), G6 | L | A new USB stick or floppy cannot be formatted |
| G30 | System tools | SY-1, SY-2, SY-3, SY-7, CP-2, CP-19. A browsable System Information; selectable System Monitor counters; Resource Meter; Device Manager disable, enable, remove and update; an Add New Hardware wizard; Scheduled Tasks and a Maintenance wizard | G29 for maintenance tasks | L | For advanced users; lower first-hour impact |
| G31 | Printing | PR-1 to PR-6, CD-3, AX-1/AX-3 print. Printers folder and Add Printer; a spooler with a queue window; native generic text, PostScript and PCL drivers over LPT and print-to-file; Print and Page Setup in CiukNote, Paint, Viewer and NetSurf; Win32 printer DCs and winspool; DOS LPT capture to the spooler | G16 | XL | Few period users print in the first hour, but every application expects it |
| G32 | Users, logon and passwords | SS-1, SS-2, SS-3, SS-4, SD-6. An optional logon dialog, per-user profiles (Desktop, Start Menu, Documents, settings), Passwords and Users applets, Log Off, screen-saver password | G10, G20 | XL | Needed for shared machines; single-user use works without it |

Two areas are not new directives. They stay inside the existing programmes:

- **F5:** real Win32 applications and games (W32-3, W32-5, W32-11).
- **F7:** Direct3D (MM-10).

The out-of-scope rows are listed so that the decisions stay visible. They are
Active Desktop, multiple monitors, DriveSpace, dial-up, FTP and Telnet, ICS,
Win16, Windows Update, policies, Phone Dialer, the Microsoft Internet suite
and the scanner and infrared classes. Reopening any of them needs an owner
decision.
