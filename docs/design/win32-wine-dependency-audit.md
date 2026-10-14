# F5-14A — Wine 10.0 dependency and PE build audit

Date: 2026-10-14. Host-only evidence for the lead before the core ABI freeze.
The machine-readable census is `config/wine-audit.json`; the release and
tool/source pins are in `config/wine-pin.json`.

## Research and method decisions

- Select Wine **10.0**, the stable 10.x release, rather than a development
  10.x snapshot or the installed 11.18 tools. The [upstream announcement](https://list.winehq.org/hyperkitty/list/wine-announce%40list.winehq.org/thread/JN2HC7JA7DUGMRJBAD7RPYLE65K3MAFQ/)
  names the archive. The [tag refs](https://api.github.com/repos/wine-mirror/wine/git/matching-refs/tags/wine-10.0)
  contain 10.0 and its release candidates, with no later 10.0.y tag at audit time.
  Its [annotated tag](https://api.github.com/repos/wine-mirror/wine/git/tags/2e9cb99369e5ead9d7fe9836dcd43c05e42890bc)
  resolves to commit `b073859675060c9211fcbccfd90e4e87520dc2c2`.
- The archive SHA-256 is the independently signature-checked value recorded by
  the [Buildroot maintainer's update](https://lists.buildroot.org/pipermail/buildroot/2025-January/771333.html).
  Sandbox `curl` failed with exit 6, `Could not resolve host: dl.winehq.org`.
  The archive therefore remains **pending-download**, not locally verified.
  Sources were obtained through the GitHub connector at the tagged commit;
  do not treat those individual files as the verified release archive.
- Record [Wine 9.0](https://list.winehq.org/hyperkitty/list/wine-announce%40list.winehq.org/message/TB7WYKWZF4DWNQLWN3XZM2NLEAF6MKST/)
  as the alternative requested as "9.0 LTS" in the directive. Upstream calls
  it a stable release; that announcement does not promise LTS maintenance.
  Changing releases requires repeating the audit, not substituting this pin.
- Read the pinned [makedep defaults](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/tools/makedep.c):
  `get_default_imports` adds static `winecrt0`, a CRT (normally **ucrtbase**
  for DLLs), `kernel32`, and `ntdll`. Explicit Makefile `IMPORTS` alone is
  not a complete dependency census; static GUID libraries are not PE DLLs.
- The pinned [Wine configure](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/configure.ac)
  supports clang/lld targeting `i686-windows`, with Wine's own `include/msvcrt`
  headers. Lack of MinGW GCC does not by itself prevent a Wine PE build.
  For external PE libraries, separately supplied target headers/libraries
  still matter. [MinGW-w64 12.0.0](https://www.mingw-w64.org/changelog/)
  defaults to UCRT; headers and CRT settings must agree if that route is used.
- In pinned [winegcc](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/tools/winegcc/winegcc.c),
  the Windows target selects PE; `-mno-cygwin` selects Microsoft CRT handling,
  and `--wine-builtin` post-processes the PE with winebuild `--builtin`.
  Neither flag alone converts an ELF Winelib module into a usable PE DLL.
  Inspect the resulting headers/imports and retain a SHA-256 manifest.

The source census will distinguish source references, `.spec` forwards,
runtime lookup strings, and actual linked PE imports. Source token matching
cannot prove preprocessor reachability, provider selection, IDL-generated
imports, or a complete runtime closure. An unbuilt DLL must not be described
as a measured import set or as ABI-freeze evidence.

<!-- BEGIN GENERATED WINE AUDIT -->

## Reproducible source-reference tables

These counts and lists are i386 source-reference candidates, not exact PE
imports. They include conditional/unreachable source paths and exclude
unexpanded IDL/header-generated code. Private/header helpers are additional
requirements; zero direct Unix/server sites does not mean zero transitive use.

| DLL | ntdll | kernel32 | user32 | gdi32 | Module closure | Direct server / Unix sites | PE evidence |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| advapi32 | 43 | 53 | 1 | 0 | 58 | none observed / none observed | not-built |
| advpack | 7 | 50 | 0 | 0 | 58 | none observed / none observed | not-built |
| cabinet | 8 | 20 | 0 | 0 | 58 | none observed / none observed | not-built |
| comctl32 | 17 | 80 | 187 | 89 | 58 | none observed / none observed | not-built |
| comdlg32 | 18 | 61 | 118 | 32 | 58 | none observed / none observed | not-built |
| ddraw | 6 | 12 | 17 | 11 | 62 | none observed / none observed | not-built |
| dinput | 15 | 35 | 37 | 0 | 59 | none observed / none observed | not-built |
| dsound | 12 | 33 | 0 | 0 | 59 | none observed / none observed | not-built |
| imaadp32.acm | 0 | 5 | 1 | 0 | 59 | none observed / none observed | not-built |
| lz32 | 0 | 0 | 0 | 0 | 59 | none observed / none observed | not-built |
| msacm32 | 7 | 27 | 13 | 0 | 58 | none observed / none observed | not-built |
| msadp32.acm | 1 | 5 | 1 | 0 | 59 | none observed / none observed | not-built |
| msg711.acm | 0 | 5 | 1 | 0 | 59 | none observed / none observed | not-built |
| msrle32 | 1 | 8 | 2 | 0 | 59 | none observed / none observed | not-built |
| msvcrt | 189 | 168 | 2 | 0 | 58 | none observed / none observed | not-built |
| msvfw32 | 11 | 27 | 33 | 9 | 59 | none observed / none observed | not-built |
| msvidc32 | 2 | 4 | 1 | 0 | 59 | none observed / none observed | not-built |
| ole32 | 22 | 76 | 50 | 37 | 58 | none observed / none observed | not-built |
| oleaut32 | 33 | 66 | 9 | 33 | 58 | none observed / none observed | not-built |
| riched20 | 22 | 25 | 64 | 47 | 59 | none observed / none observed | not-built |
| riched32 | 0 | 1 | 3 | 0 | 60 | none observed / none observed | not-built |
| rpcrt4 | 23 | 51 | 1 | 0 | 58 | none observed / none observed | not-built |
| setupapi | 37 | 92 | 11 | 0 | 58 | none observed / none observed | not-built |
| shell32 | 39 | 148 | 134 | 17 | 58 | none observed / none observed | not-built |
| shlwapi | 21 | 83 | 55 | 7 | 58 | none observed / none observed | not-built |
| version | 4 | 16 | 0 | 0 | 58 | none observed / none observed | built-experiment |
| winmm | 19 | 61 | 24 | 0 | 58 | none observed / none observed | not-built |

| Core | Unique source-reference candidates |
| --- | ---: |
| ntdll | 242 |
| kernel32 | 374 |
| user32 | 301 |
| gdi32 | 135 |

### Modern provider candidates

Names may overlap the four-core table; do not add these counts to it.

| Provider | Unique source-reference candidates |
| --- | ---: |
| combase | 54 |
| coml2 | 19 |
| kernelbase | 439 |
| sechost | 55 |
| shcore | 17 |
| win32u | 0 |

### Actual Wine 10.0 version.dll PE imports

| Provider | Count | Exact linked names |
| --- | ---: | --- |
| KERNEL32.DLL | 7 | `IsBadStringPtrA`, `LZClose`, `LZCopy`, `LZOpenFileA`, `MoveFileA`, `OpenFile`, `_lclose` |
| KERNELBASE.DLL | 21 | `DeleteFileA`, `DisableThreadLibraryCalls`, `GetFileAttributesA`, `GetFileVersionInfoA`, `GetFileVersionInfoExA`, `GetFileVersionInfoExW`, `GetFileVersionInfoSizeA`, `GetFileVersionInfoSizeExA`, `GetFileVersionInfoSizeExW`, `GetFileVersionInfoSizeW`, `GetFileVersionInfoW`, `GetTempFileNameA`, `HeapAlloc`, `HeapFree`, `MultiByteToWideChar`, `VerFindFileA`, `VerFindFileW`, `VerQueryValueA`, `VerQueryValueW`, `WideCharToMultiByte`, `lstrcmpiA` |
| NTDLL.DLL | 3 | `__wine_dbg_header`, `__wine_dbg_output`, `__wine_dbg_strdup` |
| UCRTBASE.DLL | 4 | `__stdio_common_vsprintf`, `strcpy`, `strlen`, `strrchr` |

| Export | PE forwarder |
| --- | --- |
| VerLanguageNameA | `kernel32.VerLanguageNameA` |
| VerLanguageNameW | `kernel32.VerLanguageNameW` |

SHA-256 `f4ccd4292b9b9e40af081f9479c15d759256c70898cbd952666ce1e3bce12d32`; file 40960 bytes; SizeOfImage 32768 bytes; resource directory 948 bytes.

### NTDLL Unix backend request survey

These are exact request tokens in the three pinned files below, with line
locations in JSON. This survey is not the reachable protocol of any root DLL.

| Source file | Count | Named requests |
| --- | ---: | --- |
| dlls/ntdll/unix/file.c | 35 | `add_fd_completion`, `cancel_async`, `cancel_sync`, `create_file`, `create_mailslot`, `create_named_pipe`, `flush`, `get_async_result`, `get_directory_cache_entry`, `get_file_info`, `get_handle_unix_name`, `get_object_info`, `get_object_name`, `get_object_type`, `get_object_types`, `get_volume_info`, `ioctl`, `lock_file`, `open_file_object`, `read`, `read_change`, `read_directory_changes`, `register_async`, `set_async_direct_result`, `set_completion_info`, `set_fd_completion_mode`, `set_fd_disp_info`, `set_fd_eof_info`, `set_fd_name_info`, `set_handle_info`, `set_mailslot_info`, `set_named_pipe_info`, `set_serial_info`, `unlock_file`, `write` |
| dlls/ntdll/unix/server.c | 12 | `alloc_file_handle`, `allocate_reserve_object`, `close_handle`, `compare_objects`, `dup_handle`, `get_apc_result`, `get_handle_fd`, `init_first_thread`, `init_process_done`, `init_thread`, `queue_apc`, `select` |
| dlls/ntdll/unix/sync.c | 49 | `add_atom`, `add_completion`, `assign_job`, `cancel_timer`, `create_completion`, `create_debug_obj`, `create_directory`, `create_event`, `create_job`, `create_keyed_event`, `create_mapping`, `create_mutex`, `create_semaphore`, `create_symlink`, `create_timer`, `delete_atom`, `event_op`, `find_atom`, `get_atom_information`, `get_directory_entries`, `get_job_info`, `get_thread_completion`, `get_timer_info`, `open_completion`, `open_directory`, `open_event`, `open_job`, `open_keyed_event`, `open_mapping`, `open_mutex`, `open_semaphore`, `open_symlink`, `open_timer`, `process_in_job`, `query_completion`, `query_event`, `query_mutex`, `query_semaphore`, `query_symlink`, `release_mutex`, `release_semaphore`, `remove_completion`, `set_debug_obj_info`, `set_job_completion_port`, `set_job_limits`, `set_object_permanence`, `set_timer`, `terminate_job`, `wait_debug_event` |

### Per-DLL export names

#### advapi32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 43 | `MD4Final`, `MD4Init`, `MD4Update`, `NtCreateFile`, `NtCurrentTeb`, `NtOpenFile`, `NtQueryObject`, `NtQuerySecurityObject`, `NtQuerySystemInformation`, `NtRenameKey`, `NtSetSecurityObject`, `RtlAddAccessAllowedAceEx`, `RtlAddAccessDeniedAceEx`, `RtlAddAce`, `RtlAddAuditAccessAceEx`, `RtlCreateAcl`, `RtlCreateSecurityDescriptor`, `RtlCreateUnicodeStringFromAsciiz`, `RtlDosPathNameToNtPathName_U`, `RtlFreeUnicodeString`, `RtlGetAce`, `RtlGetControlSecurityDescriptor`, `RtlInitUnicodeString`, `RtlInitUnicodeStringEx`, `RtlIsTextUnicode`, `RtlLengthSecurityDescriptor`, `RtlMakeSelfRelativeSD`, `RtlNtStatusToDosError`, `RtlQueryInformationAcl`, `RtlSelfRelativeToAbsoluteSD`, `RtlUniform`, `memcmp`, `memcpy`, `memset`, `strcpy`, `strlen`, `swprintf`, `wcschr`, `wcscmp`, `wcscpy`, `wcslen`, `wcsncmp`, `wcsrchr` |
| kernel32 | 53 | `CloseHandle`, `CompareStringW`, `CreateFileW`, `CreateProcessAsUserW`, `DeviceIoControl`, `EnterCriticalSection`, `ExpandEnvironmentStringsW`, `FileTimeToSystemTime`, `FreeLibrary`, `GetComputerNameExW`, `GetComputerNameW`, `GetCurrentProcess`, `GetCurrentThread`, `GetEnvironmentVariableA`, `GetEnvironmentVariableW`, `GetFileAttributesW`, `GetLastError`, `GetProcAddress`, `GetSystemTimeAsFileTime`, `GetThreadLocale`, `GetVersion`, `InterlockedDecrement`, `InterlockedIncrement`, `LeaveCriticalSection`, `LoadLibraryW`, `LocalAlloc`, `LocalFree`, `MultiByteToWideChar`, `OpenProcessToken`, `OpenThreadToken`, `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `RegCloseKey`, `RegCreateKeyExA`, `RegCreateKeyExW`, `RegDeleteKeyExA`, `RegDeleteKeyExW`, `RegDeleteValueW`, `RegEnumKeyExA`, `RegEnumKeyExW`, `RegOpenKeyExA`, `RegOpenKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSaveKeyExA`, `RegSaveKeyExW`, `RegSetValueExW`, `SetLastError`, `WideCharToMultiByte`, `lstrcatW`, `lstrcpyW`, `lstrlenW` |
| user32 | 1 | `GetUserObjectSecurity` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 30; `-import` exports: 196. Exact targets, potential providers and source locations are in the JSON census.

#### advpack

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 7 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `memmove`, `strlen`, `wcschr`, `wcsrchr`, `wcstol` |
| kernel32 | 50 | `CloseHandle`, `CopyFileW`, `CreateFileW`, `CreateProcessW`, `DeleteFileW`, `FindClose`, `FindFirstFileW`, `FindNextFileW`, `FindResourceW`, `FreeLibrary`, `FreeResource`, `GetCurrentDirectoryW`, `GetCurrentProcess`, `GetEnvironmentVariableW`, `GetFileAttributesA`, `GetFileAttributesW`, `GetLastError`, `GetModuleFileNameW`, `GetProcAddress`, `GetTempFileNameW`, `GetTempPathW`, `GetWindowsDirectoryW`, `LoadLibraryA`, `LoadLibraryExW`, `LoadResource`, `LockResource`, `MultiByteToWideChar`, `OpenProcessToken`, `RegCloseKey`, `RegCreateKeyExW`, `RegOpenKeyExW`, `RegQueryValueExW`, `RegSetValueExW`, `RemoveDirectoryW`, `SetFileAttributesW`, `SizeofResource`, `WaitForSingleObject`, `WideCharToMultiByte`, `WriteFile`, `WritePrivateProfileStringW`, `lstrcatW`, `lstrcmpW`, `lstrcmpiA`, `lstrcmpiW`, `lstrcpyA`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### cabinet

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 8 | `RtlUlongByteSwap`, `RtlUshortByteSwap`, `memcpy`, `memset`, `strcat`, `strcpy`, `strlen`, `strrchr` |
| kernel32 | 20 | `CloseHandle`, `CreateDirectoryA`, `CreateFileA`, `DosDateTimeToFileTime`, `GetFileAttributesA`, `GetLastError`, `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `LocalFileTimeToFileTime`, `ReadFile`, `SetFilePointer`, `SetFileTime`, `SetLastError`, `WriteFile`, `lstrcatA`, `lstrcmpiA`, `lstrcpyA`, `lstrcpynA`, `lstrlenA` |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### comctl32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 17 | `RtlMoveMemory`, `abs`, `floor`, `isdigit`, `iswalnum`, `memcpy`, `memmove`, `memset`, `qsort`, `strcat`, `strcpy`, `strlen`, `wcschr`, `wcsncmp`, `wcsrchr`, `wcsstr`, `wcstol` |
| kernel32 | 80 | `CloseHandle`, `CompareFileTime`, `CompareStringA`, `CompareStringW`, `CreateEventW`, `CreateThread`, `DeleteCriticalSection`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `FileTimeToSystemTime`, `FindClose`, `FindFirstFileW`, `FindNextFileW`, `FindResourceA`, `FindResourceW`, `FreeResource`, `GetCurrentThreadId`, `GetDateFormatW`, `GetDriveTypeW`, `GetLastError`, `GetLocalTime`, `GetLocaleInfoW`, `GetModuleFileNameW`, `GetModuleHandleA`, `GetProcAddress`, `GetProcessHeap`, `GetThreadLocale`, `GetTickCount`, `GetUserDefaultLCID`, `GetVersion`, `GlobalAddAtomW`, `GlobalAlloc`, `GlobalDeleteAtom`, `GlobalFree`, `GlobalLock`, `GlobalUnlock`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedIncrement`, `IsBadCodePtr`, `IsBadStringPtrA`, `IsBadStringPtrW`, `IsBadWritePtr`, `IsValidLocale`, `LeaveCriticalSection`, `LoadLibraryW`, `LoadResource`, `LocalAlloc`, `LocalFree`, `LocalLock`, `LocalReAlloc`, `LocalSize`, `LocalUnlock`, `LockResource`, `MulDiv`, `MultiByteToWideChar`, `RegCloseKey`, `RegCreateKeyExW`, `RegOpenKeyExW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExW`, `RtlMoveMemory`, `SetEvent`, `SetLastError`, `SizeofResource`, `SystemTimeToFileTime`, `WaitForSingleObject`, `WideCharToMultiByte`, `lstrcatW`, `lstrcmpW`, `lstrcmpiW`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 187 | `AdjustWindowRect`, `AdjustWindowRectEx`, `AppendMenuW`, `BeginDeferWindowPos`, `BeginPaint`, `CallWindowProcA`, `CallWindowProcW`, `CharLowerBuffW`, `CharLowerW`, `CharUpperBuffW`, `CharUpperW`, `CheckMenuItem`, `ChildWindowFromPoint`, `ClientToScreen`, `CloseClipboard`, `CopyIcon`, `CopyImage`, `CopyRect`, `CreateCaret`, `CreateDialogIndirectParamA`, `CreateDialogIndirectParamW`, `CreateIconIndirect`, `CreatePopupMenu`, `CreateWindowExW`, `DefWindowProcW`, `DeferWindowPos`, `DeleteMenu`, `DestroyCaret`, `DestroyCursor`, `DestroyIcon`, `DestroyMenu`, `DestroyWindow`, `DialogBoxIndirectParamW`, `DialogBoxParamW`, `DispatchMessageW`, `DragDetect`, `DrawEdge`, `DrawFocusRect`, `DrawFrameControl`, `DrawIcon`, `DrawIconEx`, `DrawStateW`, `DrawTextA`, `DrawTextW`, `EditWndProc`, `EmptyClipboard`, `EnableMenuItem`, `EnableScrollBar`, `EnableWindow`, `EndDeferWindowPos`, `EndDialog`, `EndPaint`, `EnumChildWindows`, `EqualRect`, `FillRect`, `FrameRect`, `GetActiveWindow`, `GetAsyncKeyState`, `GetCapture`, `GetCaretPos`, `GetClassLongW`, `GetClassNameW`, `GetClientRect`, `GetClipboardData`, `GetCursorPos`, `GetDC`, `GetDCEx`, `GetDesktopWindow`, `GetDlgCtrlID`, `GetDlgItem`, `GetDoubleClickTime`, `GetDpiForWindow`, `GetFocus`, `GetIconInfo`, `GetKeyNameTextW`, `GetKeyState`, `GetMenuState`, `GetMessagePos`, `GetMessageTime`, `GetMessageW`, `GetMonitorInfoW`, `GetNextDlgGroupItem`, `GetNextDlgTabItem`, `GetParent`, `GetPropW`, `GetScrollInfo`, `GetScrollPos`, `GetScrollRange`, `GetSubMenu`, `GetSysColor`, `GetSysColorBrush`, `GetSystemMenu`, `GetSystemMetrics`, `GetTopWindow`, `GetWindow`, `GetWindowDC`, `GetWindowLongW`, `GetWindowRect`, `GetWindowRgn`, `GetWindowTextA`, `GetWindowTextLengthA`, `GetWindowTextLengthW`, `GetWindowTextW`, `InflateRect`, `InternalGetWindowText`, `IntersectRect`, `InvalidateRect`, `InvalidateRgn`, `InvertRect`, `IsChild`, `IsClipboardFormatAvailable`, `IsDialogMessageW`, `IsRectEmpty`, `IsWindow`, `IsWindowEnabled`, `IsWindowUnicode`, `IsWindowVisible`, `KillSystemTimer`, `KillTimer`, `LoadBitmapW`, `LoadCursorW`, `LoadIconW`, `LoadImageA`, `LoadImageW`, `LoadMenuA`, `LoadStringW`, `MapDialogRect`, `MapVirtualKeyW`, `MapWindowPoints`, `MonitorFromRect`, `MonitorFromWindow`, `MoveWindow`, `NotifyWinEvent`, `OffsetRect`, `OpenClipboard`, `PeekMessageW`, `PostMessageW`, `PostQuitMessage`, `PtInRect`, `RedrawWindow`, `RegisterClassW`, `RegisterWindowMessageW`, `ReleaseCapture`, `ReleaseDC`, `RemovePropW`, `ScreenToClient`, `ScrollWindow`, `ScrollWindowEx`, `SendDlgItemMessageW`, `SendMessageA`, `SendMessageW`, `SetCapture`, `SetCaretPos`, `SetClipboardData`, `SetCursor`, `SetFocus`, `SetParent`, `SetPropW`, `SetRect`, `SetRectEmpty`, `SetScrollInfo`, `SetScrollPos`, `SetScrollRange`, `SetSystemTimer`, `SetTimer`, `SetWindowLongA`, `SetWindowLongW`, `SetWindowPos`, `SetWindowRgn`, `SetWindowTextA`, `SetWindowTextW`, `ShowCaret`, `ShowScrollBar`, `ShowWindow`, `SubtractRect`, `SystemParametersInfoW`, `TabbedTextOutW`, `TrackMouseEvent`, `TrackPopupMenu`, `TranslateMessage`, `UnionRect`, `UnregisterClassA`, `UnregisterClassW`, `UpdateWindow`, `WindowFromPoint`, `mouse_event`, `wsprintfW` |
| gdi32 | 89 | `BitBlt`, `CombineRgn`, `CreateBitmap`, `CreateCompatibleBitmap`, `CreateCompatibleDC`, `CreateDIBSection`, `CreateFontIndirectW`, `CreatePatternBrush`, `CreatePen`, `CreatePenIndirect`, `CreatePolygonRgn`, `CreateRectRgn`, `CreateRectRgnIndirect`, `CreateRoundRectRgn`, `CreateSolidBrush`, `DPtoLP`, `DeleteDC`, `DeleteObject`, `ExcludeClipRect`, `ExtCreatePen`, `ExtCreateRegion`, `ExtSelectClipRgn`, `ExtTextOutW`, `FillRgn`, `FrameRgn`, `GdiAlphaBlend`, `GdiGetCharDimensions`, `GdiGetCodePage`, `GetBkColor`, `GetBkMode`, `GetCharABCWidthsW`, `GetCharWidthInfo`, `GetCharWidthW`, `GetClipBox`, `GetClipRgn`, `GetCurrentObject`, `GetDIBColorTable`, `GetDIBits`, `GetDeviceCaps`, `GetLayout`, `GetNearestColor`, `GetObjectType`, `GetObjectW`, `GetPixel`, `GetStockObject`, `GetTextCharsetInfo`, `GetTextColor`, `GetTextExtentExPointW`, `GetTextExtentPoint32A`, `GetTextExtentPoint32W`, `GetTextExtentPointA`, `GetTextExtentPointW`, `GetTextMetricsW`, `IntersectClipRect`, `LineTo`, `MoveToEx`, `OffsetWindowOrgEx`, `PatBlt`, `PlayEnhMetaFile`, `PolyPolyline`, `Polygon`, `RectVisible`, `Rectangle`, `ScriptBreak`, `ScriptStringAnalyse`, `ScriptStringCPtoX`, `ScriptStringFree`, `ScriptStringGetLogicalWidths`, `ScriptStringOut`, `ScriptStringXtoCP`, `ScriptString_pSize`, `ScriptString_pcOutChars`, `SelectClipRgn`, `SelectObject`, `SelectPalette`, `SetBkColor`, `SetBkMode`, `SetDIBColorTable`, `SetDIBits`, `SetPixel`, `SetPolyFillMode`, `SetRectRgn`, `SetTextAlign`, `SetTextColor`, `SetViewportOrgEx`, `SetWindowOrgEx`, `StretchBlt`, `StretchDIBits`, `TextOutW` |

Private source names: none observed directly. Explicit forwarders: 31; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### comdlg32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 18 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `atoi`, `iswdigit`, `memcpy`, `memset`, `sprintf`, `strcat`, `strcmp`, `strcpy`, `strlen`, `strrchr`, `swprintf`, `toupper`, `wcschr`, `wcspbrk`, `wcsrchr`, `wcstol` |
| kernel32 | 61 | `ActivateActCtx`, `CreateActCtxW`, `DeactivateActCtx`, `DisableThreadLibraryCalls`, `ExpandEnvironmentStringsW`, `FindClose`, `FindFirstFileW`, `FindResourceA`, `FindResourceW`, `FormatMessageW`, `GetCurrentActCtx`, `GetCurrentDirectoryA`, `GetCurrentDirectoryW`, `GetDriveTypeA`, `GetFileAttributesW`, `GetFullPathNameW`, `GetLastError`, `GetLocaleInfoW`, `GetModuleFileNameW`, `GetModuleHandleW`, `GetProcessHeap`, `GetVersionExW`, `GlobalAlloc`, `GlobalFlags`, `GlobalFree`, `GlobalLock`, `GlobalReAlloc`, `GlobalSize`, `GlobalUnlock`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `InterlockedDecrement`, `InterlockedIncrement`, `LoadResource`, `LocalFree`, `LockResource`, `MulDiv`, `MultiByteToWideChar`, `RegCloseKey`, `RegGetValueW`, `RegOpenKeyExW`, `RegQueryValueExW`, `RegSetValueExW`, `ReleaseActCtx`, `SetCurrentDirectoryW`, `SetLastError`, `TlsAlloc`, `TlsFree`, `TlsGetValue`, `TlsSetValue`, `WideCharToMultiByte`, `lstrcatW`, `lstrcmpA`, `lstrcmpW`, `lstrcmpiW`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 118 | `AdjustWindowRectEx`, `AppendMenuW`, `BeginDeferWindowPos`, `BeginPaint`, `CallWindowProcA`, `CallWindowProcW`, `CharLowerW`, `CheckDlgButton`, `CheckRadioButton`, `ClientToScreen`, `CreateDialogIndirectParamA`, `CreateDialogIndirectParamW`, `CreatePopupMenu`, `CreateWindowExA`, `CreateWindowExW`, `DefWindowProcW`, `DeferWindowPos`, `DeleteMenu`, `DestroyIcon`, `DestroyMenu`, `DestroyWindow`, `DialogBoxIndirectParamA`, `DialogBoxIndirectParamW`, `DialogBoxParamW`, `DlgDirListComboBoxW`, `DlgDirListW`, `DrawEdge`, `DrawFocusRect`, `DrawIconEx`, `DrawTextW`, `EnableMenuItem`, `EnableWindow`, `EndDeferWindowPos`, `EndDialog`, `EndPaint`, `EqualRect`, `FillRect`, `GetClassInfoW`, `GetClassLongW`, `GetClientRect`, `GetDC`, `GetDialogBaseUnits`, `GetDlgCtrlID`, `GetDlgItem`, `GetDlgItemInt`, `GetDlgItemTextA`, `GetDlgItemTextW`, `GetFocus`, `GetMenuItemInfoW`, `GetParent`, `GetPropA`, `GetPropW`, `GetSysColor`, `GetSysColorBrush`, `GetSystemMetrics`, `GetWindow`, `GetWindowInfo`, `GetWindowLongW`, `GetWindowRect`, `GetWindowTextA`, `GetWindowTextLengthW`, `GetWindowTextW`, `InflateRect`, `InsertMenuItemW`, `InvalidateRect`, `IsChild`, `IsDlgButtonChecked`, `IsWindow`, `IsWindowVisible`, `LoadCursorA`, `LoadCursorW`, `LoadIconA`, `LoadIconW`, `LoadImageA`, `LoadImageW`, `LoadStringA`, `LoadStringW`, `MapWindowPoints`, `MessageBoxW`, `MoveWindow`, `PeekMessageW`, `PostMessageA`, `PtInRect`, `RedrawWindow`, `RegisterClassW`, `RegisterClipboardFormatA`, `RegisterWindowMessageA`, `RegisterWindowMessageW`, `ReleaseCapture`, `ReleaseDC`, `RemovePropA`, `RemovePropW`, `ScreenToClient`, `SendDlgItemMessageA`, `SendDlgItemMessageW`, `SendMessageA`, `SendMessageW`, `SetCapture`, `SetCursor`, `SetDlgItemInt`, `SetDlgItemTextA`, `SetDlgItemTextW`, `SetFocus`, `SetMenuInfo`, `SetParent`, `SetPropA`, `SetPropW`, `SetRect`, `SetRectEmpty`, `SetWindowLongW`, `SetWindowPos`, `SetWindowTextA`, `SetWindowTextW`, `ShowWindow`, `SystemParametersInfoW`, `TrackPopupMenu`, `UpdateWindow`, `wsprintfW` |
| gdi32 | 32 | `BitBlt`, `CreateCompatibleBitmap`, `CreateCompatibleDC`, `CreateDCA`, `CreateDCW`, `CreateFontIndirectW`, `CreateICA`, `CreateICW`, `CreatePen`, `CreateRectRgnIndirect`, `CreateSolidBrush`, `DeleteDC`, `DeleteObject`, `EnumFontFamiliesExW`, `ExtTextOutW`, `GdiConvertToDevmodeW`, `GetDeviceCaps`, `GetNearestColor`, `GetObjectW`, `GetStockObject`, `GetTextExtentPoint32W`, `GetTextMetricsW`, `LineTo`, `MoveToEx`, `Polygon`, `Rectangle`, `SelectClipRgn`, `SelectObject`, `SetBkColor`, `SetBkMode`, `SetTextColor`, `TextOutW` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### ddraw

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 6 | `fabs`, `memcmp`, `memcpy`, `memmove`, `memset`, `strcpy` |
| kernel32 | 12 | `DisableThreadLibraryCalls`, `GetLastError`, `GetModuleHandleExW`, `GetTickCount`, `GetVersion`, `InterlockedCompareExchange`, `InterlockedDecrement`, `InterlockedIncrement`, `IsBadReadPtr`, `RegCloseKey`, `RegQueryValueExA`, `WideCharToMultiByte` |
| user32 | 17 | `ClipCursor`, `CreateWindowExA`, `DefWindowProcA`, `DestroyWindow`, `GetDC`, `GetDesktopWindow`, `GetForegroundWindow`, `GetSystemMetrics`, `IsRectEmpty`, `RedrawWindow`, `RegisterClassA`, `ReleaseDC`, `SetRect`, `SetRectEmpty`, `SetWindowPos`, `ShowWindow`, `UnregisterClassA` |
| gdi32 | 11 | `BitBlt`, `CombineRgn`, `CreateRectRgn`, `CreateRectRgnIndirect`, `DeleteObject`, `ExtCreateRegion`, `GetDCOrgEx`, `GetRandomRgn`, `GetRegionData`, `GetStockObject`, `OffsetRgn` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### dinput

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 15 | `atan2`, `cos`, `memcmp`, `memcpy`, `memset`, `sin`, `sprintf`, `sqrt`, `strcat`, `strrchr`, `swprintf`, `wcscmp`, `wcscpy`, `wcslen`, `wcsstr` |
| kernel32 | 35 | `CancelIoEx`, `CloseHandle`, `CreateEventW`, `CreateFileW`, `CreateProcessW`, `CreateThread`, `DeleteCriticalSection`, `DeviceIoControl`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `FreeLibraryAndExitThread`, `GetCurrentProcessId`, `GetCurrentThread`, `GetLastError`, `GetModuleFileNameA`, `GetModuleHandleExA`, `GetOverlappedResult`, `GetTickCount64`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedIncrement`, `LeaveCriticalSection`, `MulDiv`, `MultiByteToWideChar`, `ReadFile`, `RegCloseKey`, `RegDeleteTreeW`, `RegQueryValueExW`, `RegSetValueExW`, `SetEvent`, `SetThreadDescription`, `WaitForSingleObject`, `WideCharToMultiByte`, `WriteFile`, `lstrcpynW` |
| user32 | 37 | `CallNextHookEx`, `ClipCursor`, `DefWindowProcW`, `DestroyWindow`, `DialogBoxParamW`, `DispatchMessageW`, `EndDialog`, `GetClientRect`, `GetCursorPos`, `GetDesktopWindow`, `GetDlgItem`, `GetForegroundWindow`, `GetKeyNameTextW`, `GetKeyState`, `GetKeyboardType`, `GetRawInputData`, `GetSystemMetrics`, `GetWindowLongW`, `GetWindowThreadProcessId`, `IsWindow`, `LoadStringW`, `MapVirtualKeyW`, `MapWindowPoints`, `MsgWaitForMultipleObjectsEx`, `PeekMessageW`, `PostMessageW`, `RegisterClassExW`, `RegisterRawInputDevices`, `SendDlgItemMessageW`, `SendMessageW`, `SetCursorPos`, `SetWindowLongW`, `SetWindowsHookExW`, `ShowCursor`, `TranslateMessage`, `UnhookWindowsHookEx`, `UnregisterClassW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### dsound

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 12 | `RtlUlongByteSwap`, `RtlUshortByteSwap`, `atoi`, `cos`, `log`, `memcpy`, `memset`, `pow`, `qsort`, `sqrt`, `strcat`, `strrchr` |
| kernel32 | 33 | `AcquireSRWLockExclusive`, `AcquireSRWLockShared`, `CloseHandle`, `CreateEventW`, `CreateThread`, `DeleteCriticalSection`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `GetCurrentThread`, `GetLastError`, `GetModuleFileNameA`, `GetModuleHandleExW`, `GetTickCount`, `InitializeCriticalSectionEx`, `InitializeSRWLock`, `InterlockedCompareExchange`, `InterlockedDecrement`, `InterlockedIncrement`, `LeaveCriticalSection`, `MulDiv`, `MultiByteToWideChar`, `RegCloseKey`, `RegQueryValueExA`, `ReleaseSRWLockExclusive`, `ReleaseSRWLockShared`, `SetEvent`, `SetThreadDescription`, `SetThreadPriority`, `WaitForSingleObject`, `WideCharToMultiByte`, `lstrcmpW`, `lstrcpynW`, `lstrlenW` |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### imaadp32.acm

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 0 | — |
| kernel32 | 5 | `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `MultiByteToWideChar`, `lstrcpyW` |
| user32 | 1 | `MessageBoxA` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### lz32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 0 | — |
| kernel32 | 0 | — |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 12; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msacm32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 7 | `memcmp`, `memcpy`, `memset`, `strlen`, `swprintf`, `wcschr`, `wcsstr` |
| kernel32 | 27 | `DisableThreadLibraryCalls`, `GetPrivateProfileSectionW`, `GetProcessHeap`, `GetVersionExA`, `HeapAlloc`, `HeapCreate`, `HeapDestroy`, `HeapFree`, `InterlockedDecrement`, `InterlockedIncrement`, `IsBadReadPtr`, `MultiByteToWideChar`, `RegCloseKey`, `RegEnumKeyExW`, `RegEnumValueW`, `RegOpenKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExA`, `RegSetValueExW`, `SetLastError`, `WideCharToMultiByte`, `lstrcmpW`, `lstrcpyW`, `lstrcpynW`, `lstrlenW` |
| user32 | 13 | `CharLowerW`, `DialogBoxIndirectParamW`, `DialogBoxParamW`, `EndDialog`, `GetDlgItem`, `GetPropW`, `MessageBoxA`, `RegisterWindowMessageW`, `SendDlgItemMessageW`, `SendMessageW`, `SetPropW`, `ShowWindow`, `wsprintfW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msadp32.acm

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 1 | `memcpy` |
| kernel32 | 5 | `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `MultiByteToWideChar`, `lstrcpyW` |
| user32 | 1 | `MessageBoxA` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msg711.acm

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 0 | — |
| kernel32 | 5 | `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `MultiByteToWideChar`, `lstrcpyW` |
| user32 | 1 | `MessageBoxA` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msrle32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 1 | `memcpy` |
| kernel32 | 8 | `DisableThreadLibraryCalls`, `GlobalAlloc`, `GlobalFree`, `GlobalHandle`, `GlobalLock`, `GlobalUnlock`, `LocalAlloc`, `LocalFree` |
| user32 | 2 | `LoadStringW`, `MessageBoxW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msvcrt

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 189 | `LdrAddRefDll`, `NtCurrentTeb`, `NtQueryInformationFile`, `NtQuerySystemTime`, `RtlAreBitsClear`, `RtlAreBitsSet`, `RtlCaptureContext`, `RtlInitializeBitMap`, `RtlPcToFileHeader`, `RtlSetBits`, `RtlTimeToSecondsSince1970`, `RtlUnwind`, `RtlWaitOnAddress`, `RtlWakeAddressSingle`, `_CIcos`, `_CIlog`, `_CIpow`, `_CIsin`, `_CIsqrt`, `__isascii`, `__iscsym`, `__iscsymf`, `__toascii`, `_atoi64`, `_errno`, `_ftol`, `_i64toa`, `_i64toa_s`, `_i64tow`, `_i64tow_s`, `_itoa`, `_itoa_s`, `_itow`, `_itow_s`, `_lfind`, `_ltoa`, `_ltoa_s`, `_ltow`, `_ltow_s`, `_makepath_s`, `_memccpy`, `_memicmp`, `_snprintf`, `_snprintf_s`, `_snwprintf`, `_snwprintf_s`, `_splitpath`, `_splitpath_s`, `_stricmp`, `_strlwr`, `_strlwr_s`, `_strnicmp`, `_strupr`, `_strupr_s`, `_swprintf`, `_tolower`, `_toupper`, `_ui64toa`, `_ui64toa_s`, `_ui64tow`, `_ui64tow_s`, `_ultoa`, `_ultoa_s`, `_ultow`, `_ultow_s`, `_vscprintf`, `_vscwprintf`, `_vsnprintf`, `_vsnprintf_s`, `_vsnwprintf`, `_vsnwprintf_s`, `_vswprintf`, `_wcsicmp`, `_wcslwr`, `_wcslwr_s`, `_wcsnicmp`, `_wcstoi64`, `_wcstoui64`, `_wcsupr`, `_wcsupr_s`, `_wmakepath_s`, `_wsplitpath_s`, `_wtoi`, `_wtoi64`, `_wtol`, `abs`, `atan`, `atan2`, `atoi`, `atol`, `bsearch`, `bsearch_s`, `ceil`, `cos`, `fabs`, `floor`, `isalnum`, `isalpha`, `iscntrl`, `isdigit`, `isgraph`, `islower`, `isprint`, `ispunct`, `isspace`, `isupper`, `iswalnum`, `iswalpha`, `iswascii`, `iswctype`, `iswdigit`, `iswgraph`, `iswlower`, `iswprint`, `iswspace`, `iswxdigit`, `isxdigit`, `labs`, `log`, `mbstowcs`, `memchr`, `memcmp`, `memcpy`, `memcpy_s`, `memmove`, `memmove_s`, `memset`, `pow`, `qsort`, `qsort_s`, `sin`, `sprintf`, `sprintf_s`, `sqrt`, `sscanf`, `strcat`, `strcat_s`, `strchr`, `strcmp`, `strcpy`, `strcpy_s`, `strcspn`, `strlen`, `strncat`, `strncat_s`, `strncmp`, `strncpy`, `strncpy_s`, `strnlen`, `strpbrk`, `strrchr`, `strspn`, `strstr`, `strtok_s`, `strtol`, `strtoul`, `swprintf`, `swprintf_s`, `tan`, `tolower`, `toupper`, `towlower`, `towupper`, `vsprintf`, `vsprintf_s`, `vswprintf_s`, `wcscat`, `wcscat_s`, `wcschr`, `wcscmp`, `wcscpy`, `wcscpy_s`, `wcscspn`, `wcslen`, `wcsncat`, `wcsncat_s`, `wcsncmp`, `wcsncpy`, `wcsncpy_s`, `wcsnlen`, `wcspbrk`, `wcsrchr`, `wcsspn`, `wcsstr`, `wcstok`, `wcstok_s`, `wcstol`, `wcstombs`, `wcstoul` |
| kernel32 | 168 | `Beep`, `CloseHandle`, `CloseThreadpoolTimer`, `CloseThreadpoolWork`, `CompareStringA`, `CompareStringW`, `CreateDirectoryW`, `CreateFileA`, `CreateFileW`, `CreatePipe`, `CreateProcessW`, `CreateThread`, `CreateThreadpoolTimer`, `CreateThreadpoolWork`, `DebugBreak`, `DecodePointer`, `DeleteCriticalSection`, `DeleteFileW`, `DuplicateHandle`, `EncodePointer`, `EnterCriticalSection`, `EnumSystemLocalesEx`, `ExitProcess`, `ExitThread`, `FileTimeToSystemTime`, `FindClose`, `FindFirstFileA`, `FindFirstFileW`, `FindNextFileA`, `FindNextFileW`, `FlsAlloc`, `FlsFree`, `FlsGetValue`, `FlsSetValue`, `FlushFileBuffers`, `FreeEnvironmentStringsA`, `FreeEnvironmentStringsW`, `FreeLibrary`, `FreeLibraryAndExitThread`, `GetACP`, `GetCPInfo`, `GetCommandLineA`, `GetCommandLineW`, `GetConsoleMode`, `GetCurrentDirectoryW`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetCurrentThreadId`, `GetDateFormatW`, `GetDiskFreeSpaceW`, `GetDriveTypeW`, `GetEnvironmentStringsA`, `GetEnvironmentStringsW`, `GetEnvironmentVariableW`, `GetExitCodeProcess`, `GetFileAttributesA`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetFileSizeEx`, `GetFileType`, `GetFullPathNameW`, `GetLastError`, `GetLocalTime`, `GetLocaleInfoA`, `GetLocaleInfoEx`, `GetLocaleInfoW`, `GetModuleFileNameA`, `GetModuleFileNameW`, `GetModuleHandleA`, `GetModuleHandleExW`, `GetModuleHandleW`, `GetNumberOfConsoleInputEvents`, `GetOEMCP`, `GetProcAddress`, `GetProcessHeap`, `GetStartupInfoA`, `GetStartupInfoW`, `GetStdHandle`, `GetStringTypeExA`, `GetStringTypeW`, `GetSystemInfo`, `GetSystemTimeAsFileTime`, `GetSystemTimePreciseAsFileTime`, `GetTempFileNameW`, `GetTimeFormatW`, `GetTimeZoneInformation`, `GetUserDefaultLocaleName`, `GetVersion`, `GetVersionExW`, `HeapAlloc`, `HeapCompact`, `HeapCreate`, `HeapDestroy`, `HeapFree`, `HeapReAlloc`, `HeapSize`, `HeapValidate`, `HeapWalk`, `InitOnceExecuteOnce`, `InitializeCriticalSection`, `InitializeCriticalSectionEx`, `InterlockedCompareExchange`, `InterlockedDecrement`, `InterlockedExchange`, `InterlockedFlushSList`, `InterlockedIncrement`, `InterlockedPushEntrySList`, `IsBadReadPtr`, `IsProcessorFeaturePresent`, `IsValidCodePage`, `IsValidLocaleName`, `LCIDToLocaleName`, `LCMapStringA`, `LCMapStringW`, `LeaveCriticalSection`, `LoadLibraryW`, `LocaleNameToLCID`, `LockFile`, `MoveFileExW`, `MultiByteToWideChar`, `PeekConsoleInputA`, `RaiseException`, `ReadConsoleA`, `ReadConsoleInputA`, `ReadConsoleInputW`, `ReadFile`, `RemoveDirectoryW`, `ResumeThread`, `RtlCaptureContext`, `RtlPcToFileHeader`, `RtlUnwind`, `SetConsoleCtrlHandler`, `SetConsoleMode`, `SetCurrentDirectoryW`, `SetEndOfFile`, `SetEnvironmentVariableW`, `SetErrorMode`, `SetEvent`, `SetFileAttributesW`, `SetFilePointer`, `SetFileTime`, `SetLastError`, `SetStdHandle`, `SetThreadpoolTimer`, `SetUnhandledExceptionFilter`, `Sleep`, `SubmitThreadpoolWork`, `SystemTimeToFileTime`, `SystemTimeToTzSpecificLocalTime`, `TerminateProcess`, `TlsAlloc`, `TlsFree`, `TlsGetValue`, `TlsSetValue`, `TryEnterCriticalSection`, `UnhandledExceptionFilter`, `UnlockFile`, `VerifyConsoleIoHandle`, `VirtualProtect`, `WaitForSingleObject`, `WaitForThreadpoolTimerCallbacks`, `WideCharToMultiByte`, `WriteConsoleA`, `WriteConsoleW`, `WriteFile`, `lstrcatW`, `lstrcpyW`, `lstrcpynA` |
| user32 | 2 | `GetActiveWindow`, `MessageBoxIndirectW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 3; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msvfw32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 11 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `_strnicmp`, `abs`, `memcmp`, `memcpy`, `memset`, `strlen`, `tolower`, `wcschr`, `wcslen` |
| kernel32 | 27 | `DisableThreadLibraryCalls`, `FreeLibrary`, `GetLastError`, `GetModuleHandleW`, `GetPrivateProfileSectionA`, `GetPrivateProfileStringW`, `GetProcAddress`, `GlobalAlloc`, `GlobalFree`, `GlobalLock`, `GlobalUnlock`, `LoadLibraryA`, `MulDiv`, `MultiByteToWideChar`, `RegCloseKey`, `RegDeleteValueA`, `RegEnumValueA`, `RegOpenKeyExA`, `RegSetValueExA`, `WideCharToMultiByte`, `lstrcatA`, `lstrcatW`, `lstrcmpW`, `lstrcpyA`, `lstrcpyW`, `lstrcpynW`, `lstrlenW` |
| user32 | 33 | `AdjustWindowRect`, `BeginPaint`, `CreateWindowExW`, `DefMDIChildProcW`, `DefWindowProcW`, `DestroyWindow`, `DialogBoxParamW`, `EnableWindow`, `EndDialog`, `EndPaint`, `GetClientRect`, `GetDlgItem`, `GetWindow`, `GetWindowLongW`, `InvalidateRect`, `IsWindowUnicode`, `KillTimer`, `LoadCursorW`, `LoadStringW`, `MessageBoxW`, `RegisterClassW`, `SendDlgItemMessageW`, `SendMessageW`, `SetCursor`, `SetFocus`, `SetTimer`, `SetWindowLongW`, `SetWindowPos`, `SetWindowTextA`, `SetWindowTextW`, `ShowWindow`, `wsprintfA`, `wsprintfW` |
| gdi32 | 9 | `CreateCompatibleDC`, `CreateDIBSection`, `CreateHalftonePalette`, `DeleteDC`, `DeleteObject`, `RealizePalette`, `SelectObject`, `SelectPalette`, `StretchBlt` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msvidc32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 2 | `memcpy`, `memset` |
| kernel32 | 4 | `DisableThreadLibraryCalls`, `GetProcessHeap`, `HeapAlloc`, `HeapFree` |
| user32 | 1 | `LoadStringW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### ole32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 22 | `NtClose`, `NtCreateKey`, `NtCurrentTeb`, `NtOpenKey`, `RtlInitUnicodeString`, `RtlNtStatusToDosError`, `RtlUlongByteSwap`, `RtlUshortByteSwap`, `abs`, `memcmp`, `memcpy`, `memmove`, `memset`, `sprintf`, `strcpy`, `strlen`, `swprintf`, `towupper`, `wcschr`, `wcscmp`, `wcstol`, `wcstoul` |
| kernel32 | 76 | `ActivateActCtx`, `CloseHandle`, `CreateEventW`, `CreateFileMappingW`, `CreateFileW`, `DeactivateActCtx`, `DeleteCriticalSection`, `DeleteFileW`, `DuplicateHandle`, `EnterCriticalSection`, `ExpandEnvironmentStringsW`, `FindActCtxSectionGuid`, `FreeLibrary`, `GetACP`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetCurrentThreadId`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetFileSize`, `GetFullPathNameW`, `GetLastError`, `GetModuleHandleW`, `GetProcessHeap`, `GetTempFileNameW`, `GetTempPathW`, `GetThreadLocale`, `GetTickCount`, `GetVersion`, `GlobalAlloc`, `GlobalFree`, `GlobalLock`, `GlobalSize`, `GlobalUnlock`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `InitOnceExecuteOnce`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedIncrement`, `IsBadCodePtr`, `IsBadReadPtr`, `LeaveCriticalSection`, `LoadLibraryExW`, `LockFileEx`, `MapViewOfFile`, `MulDiv`, `MultiByteToWideChar`, `OpenProcess`, `RaiseException`, `ReadFile`, `RegCloseKey`, `RegEnumKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExW`, `RegSetValueExW`, `ResetEvent`, `SearchPathW`, `SetEndOfFile`, `SetEvent`, `SetFilePointerEx`, `Sleep`, `UnlockFileEx`, `UnmapViewOfFile`, `VirtualQuery`, `WideCharToMultiByte`, `WriteFile`, `lstrcatW`, `lstrcmpA`, `lstrcmpiA`, `lstrcmpiW`, `lstrcpyW`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 50 | `CallNextHookEx`, `CloseClipboard`, `CopyAcceleratorTableW`, `DefWindowProcW`, `DestroyWindow`, `DispatchMessageW`, `DrawIcon`, `EmptyClipboard`, `EnumClipboardFormats`, `GetClipboardData`, `GetClipboardFormatNameA`, `GetClipboardFormatNameW`, `GetClipboardSequenceNumber`, `GetDC`, `GetKeyState`, `GetKeyboardState`, `GetMenuItemCount`, `GetMessageW`, `GetParent`, `GetPropW`, `GetSubMenu`, `GetSystemMetrics`, `GetWindowLongA`, `GetWindowThreadProcessId`, `IsClipboardFormatAvailable`, `IsWindow`, `KillTimer`, `LoadCursorW`, `OpenClipboard`, `PostQuitMessage`, `RegisterClassExW`, `RegisterClassW`, `RegisterClipboardFormatA`, `RegisterClipboardFormatW`, `ReleaseCapture`, `ReleaseDC`, `RemovePropW`, `SendMessageW`, `SetCapture`, `SetClipboardData`, `SetCursor`, `SetPropW`, `SetTimer`, `SetWindowLongW`, `SetWindowsHookExW`, `SystemParametersInfoW`, `UnhookWindowsHookEx`, `UnregisterClassW`, `WindowFromPoint`, `wsprintfW` |
| gdi32 | 37 | `BitBlt`, `CloseMetaFile`, `CopyEnhMetaFileW`, `CopyMetaFileW`, `CreateBitmapIndirect`, `CreateCompatibleBitmap`, `CreateCompatibleDC`, `CreateDIBitmap`, `CreateFontIndirectW`, `CreateMetaFileW`, `CreatePalette`, `DeleteDC`, `DeleteEnhMetaFile`, `DeleteMetaFile`, `DeleteObject`, `ExtEscape`, `GetBitmapBits`, `GetDIBits`, `GetDeviceCaps`, `GetEnhMetaFileBits`, `GetMetaFileBitsEx`, `GetObjectW`, `GetPaletteEntries`, `GetTextExtentPoint32W`, `GetWinMetaFileBits`, `PlayMetaFile`, `SelectObject`, `SetEnhMetaFileBits`, `SetMapMode`, `SetMetaFileBitsEx`, `SetViewportExtEx`, `SetViewportOrgEx`, `SetWinMetaFileBits`, `SetWindowExtEx`, `SetWindowOrgEx`, `StretchDIBits`, `TextOutW` |

Private source names: none observed directly. Explicit forwarders: 134; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### oleaut32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 33 | `_strnicmp`, `bsearch`, `ceil`, `fabs`, `floor`, `isalnum`, `iswalpha`, `iswspace`, `labs`, `memcmp`, `memcpy`, `memmove`, `memset`, `pow`, `sprintf`, `sscanf`, `strcmp`, `strcpy`, `strlen`, `strtol`, `swprintf`, `towlower`, `towupper`, `wcscat`, `wcschr`, `wcscmp`, `wcscpy`, `wcslen`, `wcsncmp`, `wcsrchr`, `wcsspn`, `wcstol`, `wcstoul` |
| kernel32 | 66 | `CloseHandle`, `CompareStringW`, `ConvertDefaultLocale`, `CreateFileMappingW`, `CreateFileW`, `DebugBreak`, `EnterCriticalSection`, `FindActCtxSectionGuid`, `FindResourceExW`, `FindResourceW`, `FreeLibrary`, `FreeResource`, `GetCurrencyFormatW`, `GetCurrentProcess`, `GetEnvironmentVariableW`, `GetFileSize`, `GetFinalPathNameByHandleW`, `GetLastError`, `GetLocaleInfoW`, `GetNumberFormatW`, `GetProcAddress`, `GetProcessHeap`, `GetSystemDefaultLCID`, `GetSystemDirectoryW`, `GetSystemTime`, `GetTimeFormatW`, `GetVersion`, `GlobalAlloc`, `GlobalFree`, `HeapAlloc`, `HeapFree`, `InterlockedDecrement`, `InterlockedIncrement`, `IsWow64Process`, `LZClose`, `LZOpenFileW`, `LZRead`, `LZSeek`, `LeaveCriticalSection`, `LoadLibraryExW`, `LoadLibraryW`, `LoadResource`, `LockResource`, `MapViewOfFile`, `MulDiv`, `MultiByteToWideChar`, `ReadFile`, `RegCloseKey`, `RegCreateKeyExW`, `RegDeleteKeyExW`, `RegEnumKeyExA`, `RegEnumKeyExW`, `RegOpenKeyExA`, `RegOpenKeyExW`, `RegQueryValueExA`, `RegSetValueExW`, `SearchPathW`, `SizeofResource`, `UnmapViewOfFile`, `WideCharToMultiByte`, `WriteFile`, `lstrcatW`, `lstrcmpiW`, `lstrcpyW`, `lstrcpynW`, `lstrlenW` |
| user32 | 9 | `CreateIconFromResourceEx`, `DestroyIcon`, `GetClientRect`, `GetDC`, `GetIconInfo`, `GetSysColor`, `GetWindowLongW`, `ReleaseDC`, `SetWindowLongW` |
| gdi32 | 33 | `BitBlt`, `CreateBitmap`, `CreateCompatibleDC`, `CreateDIBSection`, `CreateDIBitmap`, `CreateFontIndirectW`, `DeleteDC`, `DeleteEnhMetaFile`, `DeleteMetaFile`, `DeleteObject`, `EnumFontFamiliesExW`, `GdiGetCharDimensions`, `GetCurrentObject`, `GetDIBits`, `GetDeviceCaps`, `GetEnhMetaFileHeader`, `GetObjectW`, `GetPaletteEntries`, `GetTextFaceW`, `GetTextMetricsW`, `PlayEnhMetaFile`, `PlayMetaFile`, `SelectObject`, `SetBkColor`, `SetEnhMetaFileBits`, `SetMapMode`, `SetMetaFileBitsEx`, `SetTextColor`, `SetViewportExtEx`, `SetViewportOrgEx`, `SetWindowExtEx`, `SetWindowOrgEx`, `StretchBlt` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 3. Exact targets, potential providers and source locations are in the JSON census.

#### riched20

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 22 | `abs`, `bsearch`, `isalnum`, `isalpha`, `isdigit`, `isupper`, `iswalnum`, `iswspace`, `isxdigit`, `memcmp`, `memcpy`, `memmove`, `memset`, `sprintf`, `strcat`, `strcmp`, `strlen`, `strncmp`, `swprintf`, `tolower`, `towupper`, `wcscmp` |
| kernel32 | 25 | `DisableThreadLibraryCalls`, `GetACP`, `GetCPInfo`, `GetCPInfoExW`, `GetLastError`, `GetOEMCP`, `GetSystemDefaultLCID`, `GetUserDefaultLangID`, `GetVersion`, `GlobalAlloc`, `GlobalFree`, `GlobalLock`, `GlobalReAlloc`, `GlobalSize`, `GlobalUnlock`, `InterlockedDecrement`, `InterlockedIncrement`, `MulDiv`, `MultiByteToWideChar`, `WideCharToMultiByte`, `lstrcpyA`, `lstrcpyW`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 64 | `BeginPaint`, `ClientToScreen`, `CreateCaret`, `DefWindowProcW`, `DestroyCaret`, `DestroyMenu`, `EnableScrollBar`, `EndPaint`, `EqualRect`, `FillRect`, `FrameRect`, `GetClientRect`, `GetCursorPos`, `GetDC`, `GetDlgItem`, `GetDoubleClickTime`, `GetKeyState`, `GetMessageTime`, `GetParent`, `GetSysColor`, `GetSysColorBrush`, `GetSystemMetrics`, `GetUpdateRect`, `GetWindowLongW`, `HideCaret`, `InflateRect`, `IntersectRect`, `InvalidateRect`, `IsWindowVisible`, `KillTimer`, `LoadCursorW`, `MessageBeep`, `MessageBoxA`, `OffsetRect`, `PostMessageW`, `PtInRect`, `RegisterClassA`, `RegisterClassW`, `RegisterClipboardFormatA`, `RegisterClipboardFormatW`, `ReleaseCapture`, `ReleaseDC`, `ScreenToClient`, `ScrollWindowEx`, `SendMessageW`, `SetActiveWindow`, `SetCapture`, `SetCaretPos`, `SetCursor`, `SetFocus`, `SetRect`, `SetRectEmpty`, `SetScrollInfo`, `SetScrollPos`, `SetTimer`, `SetWindowLongW`, `ShowCaret`, `ShowScrollBar`, `SystemParametersInfoW`, `TrackPopupMenu`, `UnregisterClassA`, `UnregisterClassW`, `UpdateWindow`, `wsprintfW` |
| gdi32 | 47 | `CreateCompatibleDC`, `CreateDIBitmap`, `CreateFontIndirectW`, `CreatePen`, `CreateRectRgn`, `CreateRectRgnIndirect`, `CreateSolidBrush`, `DeleteDC`, `DeleteObject`, `ExtCreatePen`, `ExtSelectClipRgn`, `ExtTextOutW`, `GetClipRgn`, `GetDeviceCaps`, `GetEnhMetaFileBits`, `GetEnhMetaFileHeader`, `GetObjectW`, `GetStockObject`, `GetTextExtentExPointW`, `GetTextExtentPoint32W`, `GetTextExtentPointW`, `GetTextMetricsW`, `IntersectClipRect`, `LineTo`, `MoveToEx`, `PatBlt`, `PlayEnhMetaFile`, `RectVisible`, `ScriptCPtoX`, `ScriptFreeCache`, `ScriptItemize`, `ScriptLayout`, `ScriptPlace`, `ScriptShape`, `ScriptTextOut`, `ScriptXtoCP`, `SelectClipRgn`, `SelectObject`, `SetBkColor`, `SetBkMode`, `SetEnhMetaFileBits`, `SetTextAlign`, `SetTextColor`, `SetWinMetaFileBits`, `StretchBlt`, `TextOutW`, `TranslateCharsetInfo` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### riched32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 0 | — |
| kernel32 | 1 | `DisableThreadLibraryCalls` |
| user32 | 3 | `LoadCursorA`, `RegisterClassA`, `UnregisterClassA` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### rpcrt4

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 23 | `NtCancelIoFileEx`, `NtCurrentTeb`, `NtFsControlFile`, `NtReadFile`, `NtWriteFile`, `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `memcmp`, `memcpy`, `memmove`, `memset`, `sin`, `sprintf`, `strcat`, `strcmp`, `strcpy`, `strlen`, `strncmp`, `swprintf`, `wcschr`, `wcscmp`, `wcsncmp`, `wcstol` |
| kernel32 | 51 | `CancelIoEx`, `CloseHandle`, `CreateEventW`, `CreateFileA`, `CreateMutexW`, `CreateNamedPipeA`, `CreateThread`, `DeleteCriticalSection`, `EnterCriticalSection`, `ExitProcess`, `FlushFileBuffers`, `FormatMessageW`, `GetComputerNameA`, `GetCurrentProcessId`, `GetCurrentThread`, `GetCurrentThreadId`, `GetLastError`, `GetModuleFileNameW`, `GetNamedPipeClientProcessId`, `GetSystemTimeAsFileTime`, `GetThreadId`, `GetTickCount`, `GetTickCount64`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedExchangeAdd`, `InterlockedIncrement`, `LeaveCriticalSection`, `MultiByteToWideChar`, `PostQueuedCompletionStatus`, `QueueUserAPC`, `QueueUserWorkItem`, `RaiseException`, `RegCloseKey`, `RegDeleteTreeW`, `RegSetValueExA`, `RegSetValueExW`, `ReleaseMutex`, `ResetEvent`, `SetEvent`, `SetNamedPipeHandleState`, `SetThreadDescription`, `Sleep`, `WaitForMultipleObjects`, `WaitForMultipleObjectsEx`, `WaitForSingleObject`, `WaitNamedPipeA`, `WideCharToMultiByte`, `lstrcatW`, `lstrcpyW`, `lstrlenW` |
| user32 | 1 | `PostMessageW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### setupapi

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 37 | `LdrFindResourceDirectory_U`, `NtAllocateVirtualMemory`, `NtClose`, `NtDeleteKey`, `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `RtlImageDirectoryEntryToData`, `RtlImageNtHeader`, `RtlImageRvaToVa`, `RtlIsTextUnicode`, `RtlUnicodeToMultiByteN`, `RtlUnicodeToMultiByteSize`, `_vsnwprintf`, `iswspace`, `memchr`, `memcmp`, `memcpy`, `memset`, `sprintf`, `strcmp`, `strcpy`, `strlen`, `strncmp`, `strrchr`, `strtol`, `swprintf`, `towlower`, `wcscat`, `wcschr`, `wcscmp`, `wcscpy`, `wcslen`, `wcsncmp`, `wcspbrk`, `wcsrchr`, `wcstol`, `wcstoul` |
| kernel32 | 92 | `CloseHandle`, `CopyFileW`, `CreateDirectoryW`, `CreateFileA`, `CreateFileMappingW`, `CreateFileW`, `CreateProcessW`, `DeleteFileW`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `EnumResourceNamesW`, `FindClose`, `FindFirstFileW`, `FindNextFileW`, `FindResourceW`, `FormatMessageW`, `FreeLibrary`, `GetCurrentProcess`, `GetDiskFreeSpaceW`, `GetDriveTypeW`, `GetEnvironmentVariableW`, `GetFileAttributesW`, `GetFileSize`, `GetFileSizeEx`, `GetFileTime`, `GetFullPathNameA`, `GetFullPathNameW`, `GetLastError`, `GetLogicalDriveStringsW`, `GetPrivateProfileStringW`, `GetProcAddress`, `GetProcessHeap`, `GetSystemDirectoryW`, `GetSystemInfo`, `GetTempFileNameW`, `GetTempPathW`, `GetThreadLocale`, `GetVersion`, `GetVersionExW`, `GetWindowsDirectoryW`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `HeapSize`, `IsWow64Process`, `LZClose`, `LZCopy`, `LZOpenFileW`, `LZSeek`, `LeaveCriticalSection`, `LoadLibraryExW`, `LoadLibraryW`, `LoadResource`, `LockResource`, `MapViewOfFile`, `MoveFileExW`, `MoveFileW`, `MultiByteToWideChar`, `OpenProcessToken`, `ReadFile`, `RegCloseKey`, `RegCreateKeyExW`, `RegDeleteTreeW`, `RegDeleteValueW`, `RegEnumKeyExW`, `RegGetValueW`, `RegOpenKeyExW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExA`, `RegSetValueExW`, `RemoveDirectoryW`, `SetEndOfFile`, `SetErrorMode`, `SetFilePointer`, `SetLastError`, `SizeofResource`, `TerminateProcess`, `UnmapViewOfFile`, `VirtualFree`, `WaitForSingleObject`, `WideCharToMultiByte`, `WriteFile`, `WritePrivateProfileStringW`, `lstrcatW`, `lstrcmpiW`, `lstrcpyA`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 11 | `CharLowerW`, `DialogBoxParamW`, `EndDialog`, `GetDlgItem`, `GetWindowLongW`, `GetWindowTextW`, `LoadStringW`, `SetDlgItemTextW`, `SetWindowLongW`, `SetWindowTextW`, `ShowWindow` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### shell32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 39 | `RtlDosPathNameToNtPathName_U`, `RtlFreeUnicodeString`, `RtlGUIDFromString`, `RtlInitUnicodeString`, `RtlRandom`, `atoi`, `isprint`, `iswspace`, `memcmp`, `memcpy`, `memmove`, `memset`, `pow`, `qsort`, `sprintf`, `sscanf`, `strcat`, `strchr`, `strcmp`, `strcpy`, `strlen`, `strncmp`, `strrchr`, `swprintf`, `towlower`, `towupper`, `wcscat`, `wcschr`, `wcscmp`, `wcscpy`, `wcscpy_s`, `wcslen`, `wcsncmp`, `wcspbrk`, `wcsrchr`, `wcsstr`, `wcstol`, `wine_get_build_id`, `wine_get_host_version` |
| kernel32 | 148 | `ActivateActCtx`, `CloseHandle`, `CompareFileTime`, `CopyFileW`, `CreateActCtxW`, `CreateDirectoryW`, `CreateFileMappingW`, `CreateFileW`, `CreateProcessAsUserW`, `CreateProcessW`, `DeactivateActCtx`, `DeleteCriticalSection`, `DeleteFileA`, `DeleteFileW`, `DeviceIoControl`, `DisableThreadLibraryCalls`, `DosDateTimeToFileTime`, `EnterCriticalSection`, `ExpandEnvironmentStringsA`, `ExpandEnvironmentStringsW`, `FileTimeToDosDateTime`, `FileTimeToLocalFileTime`, `FileTimeToSystemTime`, `FindClose`, `FindFirstFileA`, `FindFirstFileW`, `FindNextFileA`, `FindNextFileW`, `FindResourceW`, `FormatMessageA`, `FormatMessageW`, `FreeEnvironmentStringsW`, `FreeLibrary`, `GetBinaryTypeW`, `GetCurrentDirectoryA`, `GetCurrentDirectoryW`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetDateFormatA`, `GetDateFormatW`, `GetDiskFreeSpaceExW`, `GetDriveTypeA`, `GetDriveTypeW`, `GetEnvironmentStringsW`, `GetEnvironmentVariableW`, `GetFileAttributesA`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetFileSize`, `GetFullPathNameA`, `GetFullPathNameW`, `GetLastError`, `GetLocalTime`, `GetLogicalDrives`, `GetModuleFileNameW`, `GetModuleHandleA`, `GetModuleHandleW`, `GetPrivateProfileStringW`, `GetProcAddress`, `GetProcessHeap`, `GetProfileStringW`, `GetShortPathNameA`, `GetShortPathNameW`, `GetSystemDirectoryA`, `GetSystemDirectoryW`, `GetSystemWow64DirectoryW`, `GetTickCount`, `GetTimeFormatA`, `GetTimeFormatW`, `GetVersion`, `GetVolumeInformationA`, `GetVolumeInformationW`, `GetWindowsDirectoryW`, `GlobalAlloc`, `GlobalFree`, `GlobalLock`, `GlobalSize`, `GlobalUnlock`, `HeapAlloc`, `HeapFree`, `InitOnceExecuteOnce`, `InterlockedDecrement`, `InterlockedIncrement`, `IsWow64Process`, `LeaveCriticalSection`, `LoadLibraryA`, `LoadLibraryExW`, `LoadLibraryW`, `LoadResource`, `LocalAlloc`, `LocalFileTimeToFileTime`, `LocalFree`, `LockResource`, `MapViewOfFile`, `MoveFileExW`, `MoveFileW`, `MulDiv`, `MultiByteToWideChar`, `OpenProcessToken`, `ProcessIdToSessionId`, `ReadFile`, `RegCloseKey`, `RegCreateKeyExA`, `RegCreateKeyExW`, `RegEnumKeyExW`, `RegEnumValueA`, `RegEnumValueW`, `RegGetValueW`, `RegLoadMUIStringA`, `RegLoadMUIStringW`, `RegOpenKeyExA`, `RegOpenKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExA`, `RegSetValueExW`, `ReleaseActCtx`, `RemoveDirectoryW`, `SearchPathW`, `SetCurrentDirectoryA`, `SetCurrentDirectoryW`, `SetErrorMode`, `SetFileAttributesW`, `SetFilePointer`, `SetLastError`, `SizeofResource`, `Sleep`, `SystemTimeToFileTime`, `UnmapViewOfFile`, `WideCharToMultiByte`, `Wow64DisableWow64FsRedirection`, `Wow64RevertWow64FsRedirection`, `WriteFile`, `WritePrivateProfileStringW`, `lstrcatA`, `lstrcatW`, `lstrcmpA`, `lstrcmpiA`, `lstrcmpiW`, `lstrcpyA`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW`, `wine_get_dos_file_name`, `wine_get_unix_file_name` |
| user32 | 134 | `BeginDeferWindowPos`, `BeginPaint`, `CallWindowProcW`, `CharLowerW`, `CheckMenuRadioItem`, `CreateIconIndirect`, `CreateMenu`, `CreatePopupMenu`, `CreateWindowExW`, `DdeClientTransaction`, `DdeConnect`, `DdeCreateDataHandle`, `DdeCreateStringHandleW`, `DdeDisconnect`, `DdeFreeDataHandle`, `DdeFreeStringHandle`, `DdeGetData`, `DdeGetLastError`, `DdeInitializeA`, `DdeInitializeW`, `DdeNameService`, `DdeQueryStringW`, `DdeUninitialize`, `DefWindowProcW`, `DeferWindowPos`, `DeleteMenu`, `DestroyIcon`, `DestroyMenu`, `DestroyWindow`, `DialogBoxIndirectParamW`, `DialogBoxParamW`, `DispatchMessageW`, `DrawEdge`, `DrawMenuBar`, `DrawTextW`, `EnableMenuItem`, `EnableWindow`, `EndDeferWindowPos`, `EndDialog`, `EndPaint`, `ExitWindowsEx`, `FindWindowExW`, `FindWindowW`, `GetActiveWindow`, `GetAncestor`, `GetCapture`, `GetClassInfoW`, `GetClientRect`, `GetDC`, `GetDCEx`, `GetDlgItem`, `GetDlgItemTextW`, `GetIconInfo`, `GetKeyState`, `GetMenu`, `GetMenuDefaultItem`, `GetMenuInfo`, `GetMenuItemCount`, `GetMenuItemID`, `GetMenuItemInfoW`, `GetMenuItemRect`, `GetMessageW`, `GetParent`, `GetPropW`, `GetSubMenu`, `GetSysColor`, `GetSystemMetrics`, `GetWindowLongA`, `GetWindowLongW`, `GetWindowRect`, `GetWindowTextA`, `GetWindowTextLengthW`, `GetWindowTextW`, `InsertMenuA`, `InsertMenuItemW`, `InsertMenuW`, `InvertRect`, `IsProcessDPIAware`, `IsWindow`, `IsWindowVisible`, `LoadAcceleratorsW`, `LoadCursorA`, `LoadCursorW`, `LoadIconW`, `LoadImageA`, `LoadImageW`, `LoadMenuA`, `LoadMenuW`, `LoadStringA`, `LoadStringW`, `MapWindowPoints`, `MessageBoxA`, `MessageBoxIndirectW`, `MessageBoxW`, `MoveWindow`, `PostQuitMessage`, `PrivateExtractIconExW`, `PrivateExtractIconsW`, `RegisterClassExW`, `RegisterClassW`, `RegisterClipboardFormatA`, `RegisterClipboardFormatW`, `ReleaseCapture`, `ReleaseDC`, `RemoveMenu`, `RemovePropW`, `ScreenToClient`, `SendDlgItemMessageW`, `SendMessageA`, `SendMessageTimeoutW`, `SendMessageW`, `SetCapture`, `SetCursor`, `SetDlgItemTextW`, `SetFocus`, `SetForegroundWindow`, `SetMenu`, `SetMenuDefaultItem`, `SetMenuInfo`, `SetPropW`, `SetRect`, `SetWindowLongA`, `SetWindowLongW`, `SetWindowPos`, `SetWindowTextA`, `SetWindowTextW`, `ShowWindow`, `SystemParametersInfoW`, `TrackPopupMenu`, `TrackPopupMenuEx`, `TranslateMessage`, `UpdateWindow`, `WaitForInputIdle`, `wsprintfA` |
| gdi32 | 17 | `BitBlt`, `CreateCompatibleBitmap`, `CreateCompatibleDC`, `CreateDIBSection`, `CreateFontIndirectW`, `DeleteObject`, `ExtTextOutW`, `GetBitmapBits`, `GetDeviceCaps`, `GetObjectW`, `GetStockObject`, `GetTextExtentPoint32W`, `GetTextMetricsW`, `SelectObject`, `SetBkColor`, `SetBkMode`, `SetTextColor` |

Private source names: kernel32!wine_get_dos_file_name, kernel32!wine_get_unix_file_name, ntdll!wine_get_build_id, ntdll!wine_get_host_version. Explicit forwarders: 34; `-import` exports: 4. Exact targets, potential providers and source locations are in the JSON census.

#### shlwapi

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 21 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `floor`, `islower`, `iswlower`, `memcmp`, `memcpy`, `memmove`, `memset`, `sprintf`, `strcat`, `strcmp`, `strcpy`, `strlen`, `strrchr`, `swprintf`, `tolower`, `towlower`, `wcscmp`, `wcslen`, `wcsrchr` |
| kernel32 | 83 | `CloseHandle`, `CreateFileMappingA`, `CreateSemaphoreW`, `CreateTimerQueueTimer`, `DisableThreadLibraryCalls`, `DuplicateHandle`, `FileTimeToSystemTime`, `FindClose`, `FindFirstFileW`, `FindNextFileW`, `FormatMessageW`, `FreeLibrary`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetDateFormatW`, `GetEnvironmentVariableW`, `GetFileAttributesA`, `GetFileAttributesW`, `GetLastError`, `GetLocaleInfoW`, `GetModuleFileNameA`, `GetModuleFileNameW`, `GetModuleHandleW`, `GetNumberFormatW`, `GetPrivateProfileStringW`, `GetProcAddress`, `GetStringTypeW`, `GetSystemDefaultUILanguage`, `GetSystemDirectoryW`, `GetSystemWindowsDirectoryW`, `GetTickCount`, `GetTimeFormatW`, `GetUserDefaultUILanguage`, `GetVersion`, `GetWindowsDirectoryW`, `InterlockedDecrement`, `InterlockedIncrement`, `IsDBCSLeadByte`, `LoadLibraryA`, `LoadLibraryExA`, `LoadLibraryW`, `LocalAlloc`, `LocalFree`, `LocalReAlloc`, `LocalSize`, `MapViewOfFile`, `MultiByteToWideChar`, `OpenProcess`, `OpenProcessToken`, `OpenSemaphoreW`, `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `QueueUserWorkItem`, `RegCloseKey`, `RegCreateKeyExW`, `RegDeleteValueA`, `RegDeleteValueW`, `RegEnumValueA`, `RegEnumValueW`, `RegOpenKeyExA`, `RegOpenKeyExW`, `RegQueryInfoKeyA`, `RegQueryInfoKeyW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExA`, `RegSetValueExW`, `ReleaseSemaphore`, `SetErrorMode`, `SetFileAttributesA`, `SetFileAttributesW`, `SystemTimeToTzSpecificLocalTime`, `UnmapViewOfFile`, `WaitForSingleObject`, `WideCharToMultiByte`, `WritePrivateProfileStringW`, `lstrcatW`, `lstrcmpiA`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 55 | `CharNextA`, `CharPrevA`, `CheckMenuItem`, `CreatePopupMenu`, `CreateWindowExA`, `CreateWindowExW`, `DefWindowProcA`, `DefWindowProcW`, `DestroyMenu`, `DialogBoxParamW`, `EnableMenuItem`, `EndDialog`, `EnumChildWindows`, `GetClassInfoA`, `GetClassInfoW`, `GetClientRect`, `GetDC`, `GetDlgItem`, `GetMenuDefaultItem`, `GetMenuItemCount`, `GetMenuItemInfoW`, `GetParent`, `GetPropA`, `GetSubMenu`, `GetSystemMetrics`, `GetWindowLongW`, `IsChild`, `IsWindowUnicode`, `LoadCursorA`, `LoadCursorW`, `LoadMenuW`, `LoadStringW`, `MessageBoxW`, `MsgWaitForMultipleObjectsEx`, `PeekMessageW`, `PostMessageA`, `PostMessageW`, `RegisterClassA`, `RegisterClassW`, `RegisterClipboardFormatA`, `ReleaseDC`, `RemoveMenu`, `RemovePropA`, `SendMessageA`, `SendMessageTimeoutA`, `SendMessageTimeoutW`, `SendMessageW`, `SetParent`, `SetWindowLongA`, `SetWindowLongW`, `SetWindowTextW`, `ShowWindow`, `UnregisterClassA`, `UnregisterClassW`, `wsprintfW` |
| gdi32 | 7 | `CreateHalftonePalette`, `DeleteObject`, `ExtTextOutA`, `GetDeviceCaps`, `GetTextExtentPointW`, `SelectObject`, `SetBkColor` |

Private source names: none observed directly. Explicit forwarders: 215; `-import` exports: 198. Exact targets, potential providers and source locations are in the JSON census.

#### version

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 4 | `sprintf`, `strcpy`, `strlen`, `strrchr` |
| kernel32 | 16 | `DeleteFileA`, `GetFileAttributesA`, `GetLastError`, `GetProcessHeap`, `GetTempFileNameA`, `HeapAlloc`, `HeapFree`, `LZClose`, `LZCopy`, `LZOpenFileA`, `MoveFileA`, `MultiByteToWideChar`, `OpenFile`, `WideCharToMultiByte`, `_lclose`, `lstrcmpiA` |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 2; `-import` exports: 12. Exact targets, potential providers and source locations are in the JSON census.

#### winmm

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 19 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `RtlUpperChar`, `memcmp`, `memcpy`, `memmove`, `memset`, `sprintf`, `strcat`, `strcmp`, `strcpy`, `strrchr`, `swprintf`, `wcschr`, `wcscmp`, `wcscpy`, `wcsrchr`, `wcsstr`, `wcstoul` |
| kernel32 | 61 | `CloseHandle`, `CreateEventW`, `CreateFileA`, `CreateThread`, `DeleteCriticalSection`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `FindResourceW`, `FreeLibrary`, `FreeLibraryAndExitThread`, `FreeResource`, `GetCurrentThreadId`, `GetDriveTypeW`, `GetFileAttributesA`, `GetFullPathNameA`, `GetLastError`, `GetModuleFileNameW`, `GetModuleHandleExW`, `GetPrivateProfileStringW`, `GetProcAddress`, `GetProfileStringW`, `GetTickCount`, `InitOnceExecuteOnce`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedIncrement`, `LeaveCriticalSection`, `LoadLibraryW`, `LoadResource`, `LockResource`, `MoveFileA`, `MulDiv`, `MultiByteToWideChar`, `OpenFile`, `PulseEvent`, `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `RegCloseKey`, `RegEnumKeyExW`, `RegOpenKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExW`, `ResetEvent`, `SearchPathA`, `SearchPathW`, `SetEvent`, `SetThreadPriority`, `Sleep`, `SleepConditionVariableCS`, `WaitForMultipleObjects`, `WaitForSingleObject`, `WakeConditionVariable`, `WideCharToMultiByte`, `_hwrite`, `_lclose`, `_llseek`, `_lread`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenW` |
| user32 | 24 | `CharLowerW`, `CharUpperW`, `DefWindowProcW`, `DestroyWindow`, `DispatchMessageA`, `GetActiveWindow`, `GetAsyncKeyState`, `GetMessageA`, `IsWindow`, `KillTimer`, `LoadStringA`, `LoadStringW`, `MessageBoxA`, `MsgWaitForMultipleObjects`, `PeekMessageA`, `PeekMessageW`, `PostMessageA`, `PostMessageW`, `PostThreadMessageA`, `PostThreadMessageW`, `SendMessageA`, `SendMessageW`, `SetTimer`, `SetWindowLongW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

### Recursive Makefile closure

Direct, delay and implicit default libraries are kept separately. Static
GUID/helper libraries and unresolved configure variables are not PE DLLs.
This is the build graph, including cycles; it is not a runtime working set.

| Module | Direct | Delay | Defaults | Unix companion | Configure variables |
| --- | --- | --- | --- | --- | --- |
| advapi32 | kernelbase sechost msvcrt | rpcrt4 user32 | winecrt0 msvcrt kernel32 ntdll | — | — |
| advpack | ole32 setupapi version advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| bcrypt | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | bcrypt.so | — |
| cabinet | — | — | winecrt0 ucrtbase kernel32 ntdll | — | ZLIB_PE_LIBS |
| combase | advapi32 ole32 user32 gdi32 uuid rpcrt4 | oleaut32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| comctl32 | uuid user32 gdi32 advapi32 imm32 kernelbase | winmm uxtheme | winecrt0 ucrtbase kernel32 ntdll | — | — |
| comdlg32 | uuid shell32 shlwapi comctl32 winspool user32 gdi32 advapi32 | ole32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| coml2 | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| compstui | comctl32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| crypt32 | user32 advapi32 bcrypt | cryptnet | winecrt0 ucrtbase kernel32 ntdll | crypt32.so | — |
| cryptnet | crypt32 shell32 ole32 advapi32 | wininet | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cryptui | uuid crypt32 ole32 comctl32 comdlg32 user32 gdi32 advapi32 | wintrust urlmon | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dbghelp | kernelbase | — | winecrt0 ucrtbase kernel32 ntdll | — | ZLIB_PE_LIBS |
| ddraw | dxguid uuid wined3d user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dhcpcsvc | — | iphlpapi | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dinput | dinput dxguid uuid comctl32 ole32 user32 advapi32 hid setupapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dinput8 | dinput8 dxguid uuid comctl32 ole32 user32 advapi32 hid setupapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dnsapi | — | netapi32 | winecrt0 ucrtbase kernel32 ntdll | dnsapi.so | — |
| dsound | dxguid uuid winmm ole32 advapi32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| gdi32 | user32 advapi32 win32u | winspool | winecrt0 ucrtbase kernel32 ntdll | — | — |
| gdiplus | uuid shlwapi ole32 oleaut32 user32 gdi32 mlang | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| glu32 | opengl32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hid | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ieframe | uuid urlmon shell32 comctl32 shlwapi oleaut32 ole32 user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| imaadp32.acm | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| imagehlp | dbghelp | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| imm32 | user32 gdi32 advapi32 kernelbase win32u | ole32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| iphlpapi | advapi32 dnsapi nsi uuid ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| kernel32 | kernelbase ntdll winecrt0 | — | — | — | — |
| kernelbase | uuid ntdll winecrt0 | — | — | — | — |
| lz32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mlang | uuid ole32 gdi32 advapi32 | oleaut32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mpr | user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msacm32 | winmm user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msadp32.acm | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msg711.acm | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msimg32 | gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msrle32 | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcrt | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvfw32 | winmm version comctl32 user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvidc32 | user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| netapi32 | rpcrt4 iphlpapi ws2_32 advapi32 dnsapi netutils | — | winecrt0 ucrtbase kernel32 ntdll | netapi32.so | — |
| netutils | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| nsi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ntdll | winecrt0 | — | — | ntdll.so | MUSL_PE_LIBS |
| ole32 | uuid advapi32 user32 gdi32 combase rpcrt4 kernelbase coml2 | oleaut32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| oleaut32 | uuid ole32 rpcrt4 user32 gdi32 advapi32 | comctl32 urlmon windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| opengl32 | user32 gdi32 advapi32 win32u | glu32 | winecrt0 ucrtbase kernel32 ntdll | opengl32.so | — |
| propsys | ole32 oleaut32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| riched20 | uuid ole32 oleaut32 imm32 user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| riched32 | riched20 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rpcrt4 | uuid advapi32 | iphlpapi wininet secur32 user32 ws2_32 ole32 oleaut32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sechost | kernelbase | rpcrt4 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| secur32 | advapi32 | crypt32 | winecrt0 ucrtbase kernel32 ntdll | secur32.so | — |
| setupapi | uuid advapi32 rpcrt4 kernelbase | cabinet shell32 wintrust ole32 comdlg32 user32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shcore | advapi32 | user32 ole32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shdocvw | uuid shlwapi advapi32 | version ole32 oleaut32 ieframe | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shell32 | uuid shlwapi user32 gdi32 advapi32 | ole32 oleaut32 shdocvw version comctl32 comdlg32 gdiplus | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shlwapi | uuid shcore user32 gdi32 advapi32 kernelbase | userenv oleaut32 ole32 comctl32 comdlg32 mpr mlang urlmon shell32 winmm | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ucrtbase | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| urlmon | uuid ole32 oleaut32 shell32 rpcrt4 shlwapi wininet user32 advapi32 | advpack | winecrt0 ucrtbase kernel32 ntdll | — | — |
| user32 | gdi32 sechost advapi32 kernelbase win32u uuid | imm32 combase | winecrt0 ucrtbase kernel32 ntdll | — | PNG_PE_LIBS |
| userenv | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| uxtheme | user32 gdi32 advapi32 | msimg32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| version | kernelbase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| win32u | ntdll winecrt0 | — | — | win32u.so | — |
| windowscodecs | windowscodecs uuid ole32 oleaut32 propsys rpcrt4 shlwapi user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | JPEG_PE_LIBS, PNG_PE_LIBS, TIFF_PE_LIBS |
| winecrt0 | — | — | — | — | — |
| wined3d | dxguid opengl32 user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | VKD3D_PE_LIBS |
| wininet | mpr shlwapi shell32 user32 ws2_32 advapi32 | secur32 crypt32 cryptui dhcpcsvc iphlpapi | winecrt0 ucrtbase kernel32 ntdll | — | ZLIB_PE_LIBS |
| winmm | uuid user32 advapi32 ole32 msacm32 | dinput8 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| winspool | user32 gdi32 advapi32 compstui | — | winecrt0 ucrtbase kernel32 ntdll | winspool.so | — |
| wintrust | crypt32 user32 advapi32 | cryptui imagehlp | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ws2_32 | — | dnsapi advapi32 iphlpapi user32 | winecrt0 ucrtbase kernel32 ntdll | ws2_32.so | — |

<!-- END GENERATED WINE AUDIT -->

## Evidence boundary and dependency decisions

This deliverable exercises the directive's pending-download fallback. The
archive is not verified locally. **Do not freeze the core ABI from this audit.**
The tagged source inventory has 665 files; the offline snapshots include 73
module `.spec` files and 74 build Makefiles. All 27 requested/candidate/P4/P5
modules are covered. The root census reads 387 declared C/C++/IDL and local
header inputs, records 60 runtime loader/lookup source sites and 52 matching
literal observations, and preserves 461 explicit `.spec` forwards and 413
`-import` export entries. Literal observations may repeat by source location.
All named Makefile dependencies in this graph resolve; configure-variable
libraries, COM activation, runtime-selected drivers, child executables and
unexpanded IDL/header code remain additional closure work.

The source references intersect the pinned **i386 export catalogs**; source
files are not preprocessed for i386. Architecture guards, dormant functions,
local declarations, aliases and alternative providers can change the actual
imports. `-arch=win64` exports are excluded, explicit ordinals are retained,
automatic ordinals are left unassigned, and stubs/private flags stay visible.
The JSON includes first source locations and full forward/`-import` targets.
The host test checks names against these original `.spec` snapshots and hashes;
it does not prove that an exported stub implements useful behavior.

The four-core source union is **1,052 names**: NTDLL 242, KERNEL32 374,
USER32 301, GDI32 135. Excluding the six full core/candidate modules
`advapi32`, `winmm`, `dinput`, `ddraw`, `dsound`, `msvcrt` still leaves
**784** P4/P5 source names (86 + 280 + 287 + 131). The modern provider
columns overlap that set: e.g. KERNEL32 and KERNELBASE export many identical
names. They cannot be added as independent function counts. Neither number
is an exact linked requirement or a verified reachable subset.

The 40-function warning in the original contract does not qualify an ABI.
The verified small DLL alone has **35 imports and 2 forwarded requirements**:
9 KERNEL32 exports including the two forward targets, 3 NTDLL debug exports,
21 KERNELBASE exports and 4 UCRTBASE exports. USER32 and GDI32 are absent in
this particular PE. These are exact names for the recorded experiment hash,
not the dependency list of a future canonical Wine build or a game workload.
The three NTDLL imports are Wine debug ABI; the experiment does not import
`__wine_unix_call_dispatcher` or `__wine_unixlib_handle`.

### Server, Unix and private boundaries

No direct `SERVER_START_REQ`, `wine_server_call`, `__wine_unix_call` or
`WINE_UNIX_CALL` site was observed in the audited root source inputs.
This is consistent with implementing the core services in CiukiOS rather
than porting Wine's Unix backends. It does not make upstream core DLLs
freestanding: [NTDLL's Makefile](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/ntdll/Makefile.in)
and [WIN32U's Makefile](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/win32u/Makefile.in)
define Unix companions; the graph also reaches Unix-backed crypto/network/
graphics modules. Reusing those core implementations would introduce their
host and wineserver boundary.

The three-file NTDLL Unix survey names the actual requests used there, including
`create_file`, `create_named_pipe`, `get_handle_fd`, `close_handle`, `select`,
`queue_apc`, event/mutex/semaphore operations, mappings, completion and directory
operations. Its table deliberately names the surveyed files rather than assigning
all their requests to every root DLL. A complete per-root reachable wineserver
protocol cannot be established from this source scan. `NtCreateFile` reaches
the `create_file` helper, `NtCreateNamedPipeFile` sends `create_named_pipe`,
and `NtClose` sends `close_handle` in the pinned sources. CiukiOS option C
needs corresponding semantics, not these exact Wine wire requests.

The common-helper survey records NTDLL debug functions (five header candidates;
three present in the built PE), Wine exception helpers, DLL registration helpers,
delay-load helpers and Unix-dispatch state. `__wine_register_resources` and
`__wine_unregister_resources` are static winecrt0 helpers, not proof of missing
NTDLL exports. Their generated data, registry calls and IDL output must be audited
when the larger DLLs are linked. `__wine_unix_call_dispatcher` and
`__wine_unixlib_handle` are private NTDLL data exports in the pinned `.spec`;
their presence in a generic helper header does not mean every root imports them.

Two concrete runtime boundaries escape an IMPORTS-only graph:

- [WinMM waveform](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/winmm/waveform.c)
  activates `CLSID_MMDeviceEnumerator`; MMDevAPI/audio endpoint services need
  a deliberate adapter or a retained Ciuki WinMM core. This class is not a
  literal `mmdevapi.dll` Makefile import.
- [COMBASE RPC](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/combase/rpc.c)
  has RPCSS startup and local-server/surrogate creation paths. An in-process
  COM subset requires an explicit reachable-path cut and tests; dropping
  out-of-process COM cannot be inferred just from the installer-facing API list.

### PE-only feasibility, notices and expected patches

"PE-only" below means the module can be compiled into PE code; it does not
mean its backend/registry/classes/driver dependencies exist in CiukiOS. No
source patch was applied in this experiment. Subset patches below are proposed
decision input, not implemented changes or success-returning substitutes.
Every unbuilt row still needs direct/delay/forward/ordinal/private/runtime PE
closure inspection before it can become an approved reuse manifest.

| DLL | PE-only build evidence or constraint | Required notices | Expected adapter/patch decision |
| --- | --- | --- | --- |
| version | Built i386 PE32, 16 exports, VERSIONINFO resource, 35 imports | Wine LICENSE, COPYING.LIB, AUTHORS and source headers; winecrt0 included | Supply KERNELBASE and UCRTBASE facade/imports, plus debug ABI; canonical toolchain comparison pending |
| shell32 | Not built; PE sources with large direct/delay/forward closure | Wine notices and original resource notices | Replace or cut Unix recycle-bin/path conversions, process helpers and unsupported shell namespace paths; preserve installer operations |
| comctl32 | Not built; kernelbase/imm32 plus delayed uxtheme/winmm | Wine notices and resource notices | Decide v5/v6/theme/IME coverage; complete USER/GDI semantics; no blanket removal of controls used by installers |
| comdlg32 | Not built; shell32/comctl32/winspool, delayed ole32 | Wine notices and resource notices | Decide print/modern item-dialog paths and COM classes; retain measured installer file dialogs |
| ole32 | Not built; 134 explicit forwards, COMBASE/RPCRT4/COML2 boundary | Wine notices | Resolve COM apartment/marshal/storage boundary and RPCSS/out-of-process paths before freezing exports |
| oleaut32 | Not built; RPCRT4/COM, delayed URLMON/WINDOWSCODECS | Wine notices | Decide automation/type-library/picture paths, generated proxies and supported image decoders |
| rpcrt4 | Full module audited; subset not specified; networking/security delay libraries | Wine notices | Define an in-process marshaling subset with actual generated proxy use; networking/authentication removal needs evidence |
| msvcrt | Full module source audited; custom exception ABI and MUSL_PE_LIBS | Wine plus musl MIT and file-specific notices | Specify which CRT versions games need; do not confuse game MSVCRT with Wine upper DLLs' UCRTBASE |
| setupapi | Not built; RPCRT4/KERNELBASE, cabinet/shell/trust/UI delays | Wine notices | Limit to installer INF/file operations with recorded hardware/trust paths; no unsupported driver-install success |
| advapi32 | Full module audited, including 196 imported exports; KERNELBASE/SECHOST/MSVCRT | Wine notices | Retain Ciuki registry/security core or specify precisely which Wine parts survive service/LSA/crypto/eventlog cuts |
| winmm | Full module audited; MMDevice COM activation and delayed dinput8 | Wine notices | Keep Ciuki audio/input/MCI boundary; select portable parts/codecs without silently importing a modern audio host |
| dinput | Candidate; PE code with HID/setupapi/USER/raw-input dependency | Wine notices | Retain approved Ciuki input core unless a concrete HID/raw-input adapter beats it |
| ddraw | Candidate; PE code imports WineD3D, which reaches OpenGL/Vulkan paths | Wine notices plus selected graphics dependencies' notices | Retain F5 Ciuki software DirectDraw; full WineD3D reuse is outside the small offline F5 boundary |
| dsound | Candidate; PE code and COM/audio backend requirements | Wine notices | Retain Ciuki sound adapter or prove a supported endpoint implementation |
| shlwapi | Not built; SHCORE/KERNELBASE and many forwards/imported exports | Wine notices | Define string/path/registry/shell subset and native/builtin precedence; adapt path semantics |
| riched20 | Not built; OLE/OLEAUT/IME/USER/GDI | Wine notices | Define rich text/OLE/font behavior; generated TOM IDL and font residency matter |
| riched32 | Not built; wrapper around riched20/USER | Wine notices | No independent text engine; include riched20 closure |
| cabinet | Not built; ZLIB_PE_LIBS configured/bundled source | Wine plus bundled zlib 1.3.1 notice | Pin compression backend; handle CAB callbacks/errors rather than replacing the installer |
| advpack | Not built; setupapi/version/advapi/ole32 | Wine notices | INF/registration/restart flow subset; cannot be qualified before setupapi/shell/core closure |
| lz32 | Data-only PE forwarding DLL, not linked in this experiment | Wine notices | Resolve its 12 forwards; do not budget an independent decompressor from this module name |
| msacm32 | Not built; portable PCM converter plus driver/WinMM boundary | Wine notices | Map ACM driver loading and buffers to Ciuki audio; no Unix driver DLLs in the reuse manifest |
| msadp32.acm | Not built; portable codec candidate, WinMM/USER callbacks | Wine notices | Codec host fixtures and callback/heap checks; no backend chosen by this audit |
| imaadp32.acm | Not built; portable codec candidate, WinMM/USER callbacks | Wine notices | Same boundary validation for IMA ADPCM |
| msg711.acm | Not built; portable codec candidate, WinMM/USER callbacks | Wine notices | Same boundary validation for G.711; needed format remains census-dependent |
| msvfw32 | Not built; WinMM/version/comctl32/USER/GDI | Wine notices | MCI/codec and window-message integration; driver discovery subset |
| msrle32 | Not built; portable codec candidate, WinMM/USER | Wine notices | RLE codec production tests plus driver callbacks |
| msvidc32 | Not built; portable codec candidate, USER | Wine notices | Video 1 codec production tests plus driver callbacks |

Wine's [LICENSE](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/LICENSE)
and [COPYING.LIB](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/COPYING.LIB)
govern the reused Wine code. Retain per-file copyright/alternate-license
notices, AUTHORS, complete corresponding source and the exact build/patch
manifest; retain the LGPL replacement/relinking arrangement required by the
selected distribution method. A PE boundary alone does not establish that
arrangement. The fixture snapshots are audit inputs, not corresponding source
for shipping a DLL. The [musl COPYRIGHT](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/libs/musl/COPYRIGHT)
and [bundled zlib header](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/libs/zlib/zlib.h)
are retained separately; any other selected PE libraries need their own pinned
notices rather than being relabeled LGPL.

## Build experiment and lead recipe

The host has clang/lld **23.1.1**, Wine tools **11.18**, and no MinGW GCC.
The latter was an assumption in earlier directives, not a verified absence
of all Wine tools. Neither system winegcc nor system winebuild was used to
compile/link the DLL. The Wine **10.0** tools were built from the pinned
source with clang using a small Linux configuration header. The PE code uses
Wine 10.0 Windows and CRT headers, not system Wine 11.18 headers. The target
is `i686-windows`, `-march=pentiumpro -mno-sse -mno-sse2`, with `__WINESRC__`,
`__WINE_PE_BUILD`, `_UCRT`, `-fno-builtin` and `-fshort-wchar`.

`winebuild --implib` creates pinned import libraries for KERNELBASE, UCRTBASE,
KERNEL32 and NTDLL. The recipe builds the winecrt0 debug/default-DllMain/CRT
entry objects and uses winegcc `-b i686-windows -mno-cygwin -shared`,
`-Wl,--wine-builtin` (winebuild `--builtin`) and `-Wl,-timestamp:0`.
The COFF symbol/section headers and imports were independently inspected with
llvm-readobj; the production Ciuki census parser agrees on every import.
Two consecutive script runs produced identical hashes for all three artifacts.

| Output under build/host/win32/wine-10.0 | SHA-256 |
| --- | --- |
| tools/winebuild | `e6cf212c382f95af8d8b1cbe2c33bde311c948343e765192b9ae0baacf11ebc9` |
| tools/winegcc | `fffa94b2fde1fb6198f19bb6146d5cfdb07905bb5e0bf51cddc212426768930c` |
| version.dll | `f4ccd4292b9b9e40af081f9479c15d759256c70898cbd952666ce1e3bce12d32` |

The DLL is 40,960 file bytes, 32,768 SizeOfImage bytes, with a 948-byte resource
directory and relocations. Its OS/subsystem header version is 6.0 from the modern
toolchain; no Windows 98 or Ciuki runtime result is implied. The export resource
and entry point are present. The bootstrap configuration is a build experiment,
not an upstream configure qualification or the canonical F5 distribution recipe.

Exact encountered gaps/failures and fixes:

1. WineHQ archive: sandbox DNS failure, still pending. Individual tagged files
   supply this limited experiment but not the complete generated release tree.
2. winegcc bootstrap: `CXX`/`CPP` configuration macros missing on the first
   attempt; adding the normal tool configuration macros fixed compilation.
3. PE source: missing Wine header dependencies (`sal.h`, `concurrencysal.h`)
   were fetched from the same commit; `TRACE`/`WARN` required `__WINESRC__`.
   Neither mingw-w64 headers nor an external MinGW CRT was needed for this DLL.
4. LLVM resource compiler: rejected Wine's adjacent string literals in the
   VERSIONINFO BLOCK (`expected '{', got "04B0"`). The separately hash-pinned
   system **wrc 11.18** parses the original resource using `--nostdinc` and only
   pinned Wine headers. Rebuild with Wine 10.0 wrc during canonical qualification;
   do not claim the entire experiment's toolchain is Wine 10.0.
5. Larger DLLs: generated IDL/proxy/registration headers, complete host tools,
   and configured target libraries are not built here. `ZLIB_PE_LIBS`,
   `MUSL_PE_LIBS`, graphics/crypto/font/media library variables still require
   explicit configured values and hashes.

Repeat the implemented recipe, with all jobs sequential and at nice 19:

```sh
nice -n 19 bash scripts/win32/build-wine-dlls.sh
nice -n 19 python3 scripts/win32/wine_audit.py check
nice -n 19 python3 -m unittest discover -s tests/host/win32 -p test_wine_audit.py -v
```

It verifies all source/tool hashes before building, writes `hashes.json`,
`version-pe.json` and `version-readobj.txt`, and never runs Wine or a guest.
The source tree may be replaced by the verified release archive after the
lead checks its pinned SHA-256; the source-file verifier must still pass.
For the upstream build route, Wine 10.0 configure accepts
`--enable-archs=i386 --with-mingw=clang`, with `CC=clang`, `i386_CC=clang` and
the recorded i386 CPU flags. Build tools and selected `dlls/<module>/all`
targets with `make -j1`, then inspect actual PE outputs. The lead runs any
larger build in the repository's capped scope. If an external MinGW route is
needed, `config/wine-pin.json` supplies the optional **mingw-w64 12.0.0** archive
and its [MXE-pinned SHA-256](https://github.com/mxe/mxe/blob/29894c0c51b378030478e51530f7f558a6e9430f/src/mingw-w64.mk);
headers and CRT must both be configured for the same runtime. No missing
external CRT is asserted for the already successful version experiment.

## Core size versus the ten-title census

No f5-02 title/installer/installed-binary import census is present in this
worktree. The available `scripts/pe_import_census.py` is a driver census/parser,
not measurements of the ten game titles. Thus the title-required four-core
export count and the incremental Wine-reuse cost are **unknown**, not zero.

| Approved title | Installer + installed/child/runtime DLL → function census |
| --- | --- |
| Diablo shareware | Pending f5-02A/B |
| StarCraft shareware | Pending f5-02A/B |
| Age of Empires trial | Pending f5-02A/B |
| Half-Life: Uplink | Pending f5-02A/B |
| Unreal Tournament demo 348 | Pending f5-02A/B |
| Worms Armageddon demo | Pending f5-02A/B |
| Heroes of Might and Magic III demo | Pending f5-02A/B |
| RollerCoaster Tycoon demo | Pending f5-02A/B |
| Jazz Jackrabbit 2 shareware | Pending f5-02A/B |
| Total Annihilation demo | Pending f5-02A/B |

The f5-02 format is DLL → named/ordinal function list per hashed PE, keeping
direct, delay and later runtime observations distinct. The build's verified
KERNEL32 list, for example, is:

```text
Wine 10.0 version.dll / SHA-256 f4ccd4292b9b9e40af081f9479c15d759256c70898cbd952666ce1e3bce12d32
KERNEL32.DLL -> IsBadStringPtrA, LZClose, LZCopy, LZOpenFileA, MoveFileA, OpenFile, _lclose
forward targets -> kernel32.VerLanguageNameA, kernel32.VerLanguageNameW
```

Once the real census and full linked reuse manifest exist, compute a set union
per provider and the difference `reused_exports - title_exports`. Do not
subtract the unpreprocessed 1,052-name source candidate count from a static
game census and call the result an implemented ABI requirement.

## 128 MiB footprint and recommendation

Only Wine 10.0 **version.dll** has a measured PE size here. Its 32 KiB image
is small, but its 1 MiB heap-reserve / 4 KiB heap-commit header fields do not
create a heap for this DLL: it calls the process heap. Summing those fields
across DLLs would invent allocations. Heap use is input/path dependent;
the source initially allocates 1,000 bytes per version-information buffer and
can grow to a WORD-sized length, with two buffers during comparison and
additional filename conversions. No runtime heap high-water mark was measured.

To supply a numerical **planning proxy**, the installed Wine **11.18 i386**
copies of the same 27 modules were read without execution. Their names/hashes,
file sizes, SizeOfImage and heap header values are in `memory_proxy`. They total
**19,588,512 file bytes (18.681 MiB)** and **47,218,688 SizeOfImage bytes
(45.031 MiB)**. These are not Wine 10.0 measurements or runtime residency;
file bytes include nonresident data/symbols, and SizeOfImage includes alignment,
zero-fill and reservation. Shared read-only pages are charged once, while
private writable pages and heaps depend on each process and touched paths.

| Planning component | MiB | Basis |
| --- | ---: | --- |
| All 27 upper/candidate images fully resident | 45.031 | Wine 11.18 SizeOfImage stress proxy, not a measured Wine 10.0 working set |
| Native kernel/desktop/reservations | 32 | Assumption; actual supported-image baseline pending |
| Ciuki core/facades | 8 | Assumption; code not implemented/sized |
| Win32 private heaps | 8 | Assumption per active workload, not PE heap header sum |
| Surfaces/audio/sample cache | 8 | Assumption; title and GM-bank measurements pending |
| One active title/installer payload | 24 | Assumption; ten-title peak census pending |
| Total stress scenario | **125.031** | Only **2.969 MiB** of 128 MiB remains before extra processes/closure/queues/stacks/COW |

This illustrates why an all-loaded unmodified manifest is unsafe as a fit
claim; it is not an estimate of actual simultaneous DLL loading. The 18.681
MiB file proxy also cannot be substituted as committed RAM. Demand paging,
sharing, optional path cuts and sparse heaps can lower residency; helper
chains, extra core/COM/crypto/driver DLLs, queues, synth data and surfaces can
raise it. The 27-module proxy excludes the full 74-node graph, its Unix
companions and child programs. A full Wine port's memory is likewise unmeasured.
No hardware reservations or memory limits were changed by this audit.

**Recommendation to the lead:** retain approved option C provisionally and
hold the ABI freeze. The successful 37-requirement version slice shows leaf
reuse is technically feasible without a complete POSIX host. The untrimmed
P4/P5 source boundary (784 names even without the six core/candidate modules),
modern provider/CRT dependencies, shell Unix helpers and COM activation paths
do not establish a small or cheap boundary. A numerical claim that C still
beats a full Wine/POSIX port is not supported yet.

The lead must choose the exact installer-visible subset and KERNELBASE/UCRTBASE/
COMBASE/COML2/SECHOST/SHCORE facade policy, merge the f5-02 census, and compare
actual linked sizes/peak committed memory of one installer vertical slice.
Keeping the approved Ciuki DirectDraw/input/sound/registry core avoids the
candidate backends identified above, but does not settle upper UI/COM semantics.
If the required subset still entails effectively full modern core/COM behavior,
return option B as a separate reviewed POSIX/host-adaptation proposal to the
owner; do not silently change Q2 or declare C defeated by source name counts.

## Host validation

`tests/host/win32/test_wine_audit.py` runs offline and does not rebuild. It
checks every named core/modern candidate and actual PE import against pinned
`.spec` files, counts/unions, architecture filtering, forwards/import exports,
Makefile graph/default/delay consistency, protocol tokens, notices and generated
Markdown. Negative mutations reject invented exports, changed pins/counts/graphs
and corrupted snapshots. When the optional pinned source cache or built PE
exists, it independently reproduces the source census and compares PE imports
with llvm-readobj. Those live checks skip on a fresh checkout without build
artifacts; all offline snapshot checks still run.

The existing `tests/host/test_pe_import_census.py` suite separately exercises
the production import parser with synthetic named/ordinal/delay/truncated
PE/INF inputs. Neither suite proves a Ciuki DLL load, API fidelity, actual
title imports, hardware behavior, complete reachable protocol, licence release
arrangements or a 128 MiB runtime fit. Those remain lead integration evidence.
