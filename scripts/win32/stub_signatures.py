"""Pinned i386 stack widths plus explicit W21 failure conventions.

Wine spec files supply calling convention/stack width, never return types.
The return policy below is CiukiOS metadata, checked against Microsoft API
contracts. APIs without a failure result (including void APIs) stay fatal.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[2]
CATALOG=ROOT/'tests/host/win32/wine_specs/dlls'
# Helper convention: 0 FALSE/NULL/zero, 1 E_NOTIMPL, 2 MMRESULT 8,
# 3 LSTATUS 120, 4 INVALID_HANDLE_VALUE/-1, 6 THREAD_PRIORITY_ERROR_RETURN,
# None no defined failure value.
FATAL=set('''DebugBreak DeleteCriticalSection EnterCriticalSection ExitProcess ExitThread
FatalAppExitA GetACP GetCommandLineA GetCurrentProcess GetCurrentProcessId GetCurrentThread
GetCurrentThreadId GetLastError GetLocalTime GetOEMCP GetStartupInfoA GetSystemInfo GetSystemTime
GetTickCount GetVersion GlobalMemoryStatus InitializeCriticalSection InterlockedDecrement
InterlockedIncrement LeaveCriticalSection OutputDebugStringA RaiseException RtlUnwind SetErrorMode
SetHandleCount SetLastError SetUnhandledExceptionFilter Sleep GetMessagePos GetMessageTime
GetDialogBaseUnits GetCaretBlinkTime GetSysColor PostQuitMessage ShowCursor DefDlgProcA
DefWindowProcA CallWindowProcA DispatchMessageA CallNextHookEx SendMessageA SendDlgItemMessageA
CheckMenuItem SetClassLongA SetWindowLongA SetWindowWord GetClassLongA GetWindowLongA
GetWindowWord GetSystemMetrics DragFinish SHChangeNotify CoUninitialize OleUninitialize
InitCommonControls LZClose GetSystemDefaultLCID GetSystemDefaultLangID GetUserDefaultLCID
GetUserDefaultLangID GetThreadLocale GetProcessHeap FreeSid GlobalFree LocalFree
GetActiveWindow GetCapture GetFocus GetForegroundWindow GetMenu GetParent GetSubMenu
GetCursor SetActiveWindow SetCapture SetCursor SetFocus EnableWindow ShowWindow ShowOwnedPopups
GetAsyncKeyState GetKeyState GetMenuCheckMarkDimensions GlobalFlags GlobalUnlock LocalUnlock
GetTextAlign GetBkMode GetMapMode GdiSetBatchLimit timeGetTime timeGetDevCaps
joyGetNumDevs midiOutGetNumDevs mixerGetNumDevs waveOutGetNumDevs CommDlgExtendedError
GetDriveTypeA GetFileType IsBadCodePtr IsBadReadPtr IsBadWritePtr IsDBCSLeadByte IsValidCodePage
IsValidLocale IsChild IsClipboardFormatAvailable IsIconic IsWindow IsWindowEnabled IsWindowUnicode
IsWindowVisible PtInRect EqualRect RectInRegion RectVisible PtVisible EqualSid GlobalDeleteAtom
GetPrivateProfileIntA GetProfileIntA GetTimeZoneInformation MulDiv lstrcmpA lstrcmpiA'''.split())
FATAL.update(('VerFindFileA','VerInstallFileA'))
FATAL.update(('lstrlenA','GetPrivateProfileStringA','GetProfileStringA'))
FATAL.update(('GetDeviceCaps','CharNextA','CharPrevA','CharLowerA','CharUpperA',
              'CharLowerBuffA','ImmAssociateContext','GetDesktopWindow','GetLastActivePopup','SelectObject'))
MINUS_ONE=set('''CreateFileA CreateFileW FindFirstFileA FindFirstFileW GetFileAttributesA
GetFileAttributesW GetFileSize SetFilePointer ResumeThread
SuspendThread WaitForSingleObject WaitForMultipleObjects MsgWaitForMultipleObjects WaitForInputIdle
_hread _lclose _lcreat _llseek _lopen _lread _lwrite ChangeDisplaySettingsA ChangeDisplaySettingsW
DialogBoxParamA DialogBoxIndirectParamA GetMessageA GetMessageW GetMenuItemCount GetMenuItemID
GetMenuState EnableMenuItem GetBkColor GetPixel SetBkColor SetPixel SetTextColor GetTextColor
GetFileTitleA mmioRead mmioSeek LZCopy LZOpenFileA DocumentPropertiesA'''.split())
MINUS_ONE.add('GetNearestPaletteIndex')
MINUS_ONE.update(('StretchDIBits','SetTextAlign'))
MINUS_ONE.update(('OpenFile','RealizePalette','wvsprintfA','LZOpenFileW'))
HRESULT=set('''CoCreateInstance CoInitialize OleInitialize SHGetDesktopFolder SHGetMalloc
SHGetSpecialFolderLocation DirectPlayCreate DirectPlayEnumerateA DirectPlayLobbyCreateA
DirectDrawCreate DirectDrawEnumerateA DirectSoundCreate'''.split())
MMRESULT=set('''joyGetDevCapsA joyGetPosEx mixerClose mixerGetControlDetailsA mixerGetLineControlsA
mixerGetLineInfoA mixerOpen mixerSetControlDetails mmioAdvance mmioAscend mmioClose mmioDescend
mmioGetInfo mmioSetInfo timeBeginPeriod timeEndPeriod timeKillEvent waveOutClose waveOutGetDevCapsA
waveOutGetID waveOutOpen waveOutPrepareHeader waveOutReset waveOutSetVolume waveOutUnprepareHeader
waveOutWrite'''.split())
# Audited zero-failure exports only. New names default to nonreturning.
ZERO_FAILURE=set('''AbortDoc AccessCheck AddFontResourceA AdjustTokenPrivileges AdjustWindowRect AdjustWindowRectEx
AllocateAndInitializeSid BeginDeferWindowPos BeginPaint BitBlt BringWindowToTop ChangeServiceConfigA
CharLowerA CharLowerBuffA CharNextA CharPrevA CharToOemA CharToOemBuffA CharUpperA CheckDlgButton
CheckRadioButton ClientToScreen ClipCursor CloseClipboard ClosePrinter CloseServiceHandle CombineRgn
CompareStringA CompareStringW ControlService CopyRect CreateBitmap CreateBrushIndirect
CreateCompatibleBitmap CreateCompatibleDC CreateCursor CreateDCA CreateDIBSection CreateDIBitmap
CreateDialogIndirectParamA CreateDialogParamA CreateFontA CreateFontIndirectA CreateICA CreateMenu
CreatePalette CreatePatternBrush CreatePen CreateRectRgn CreateScalableFontResourceA CreateServiceA
CreateSolidBrush CreateWindowExA DPtoLP DdeClientTransaction DdeConnect DdeCreateDataHandle
DdeCreateStringHandleA DdeDisconnect DdeFreeDataHandle DdeFreeStringHandle DdeGetData DdeInitializeA
DdeNameService DdeQueryStringA DdeUninitialize DeferWindowPos DeleteDC DeleteMenu DeleteMetaFile
DeleteObject DeleteService DestroyAcceleratorTable DestroyCaret DestroyCursor DestroyIcon
DestroyMenu DestroyWindow DragQueryFileA DrawEdge DrawFocusRect DrawIcon DrawMenuBar DrawTextA
DrawTextExA EndDeferWindowPos EndDialog EndDoc EndPage EndPaint EnumChildWindows
EnumDisplaySettingsA EnumWindows Escape ExcludeClipRect ExcludeUpdateRgn ExitWindowsEx ExtTextOutA
FillRect FindExecutableA FindTextA FindWindowA FindWindowExA FormatMessageA FrameRect GdiFlush
GetCharABCWidthsA GetCharWidthA GetClassInfoA GetClassNameA GetClientRect GetClipBox
GetClipboardData GetCommState GetConsoleScreenBufferInfo GetCurrentObject GetCurrentPositionEx
GetCursorPos GetDC GetDCEx GetDIBits GetDateFormatA GetDesktopWindow GetDeviceCaps GetDlgCtrlID
GetDlgItem GetDlgItemTextA GetFileSecurityA GetFileVersionInfoA GetFileVersionInfoSizeA
GetKeyboardState GetLargestConsoleWindowSize GetLastActivePopup GetLocaleInfoA GetLocaleInfoW
GetMenuStringA GetNextDlgTabItem GetNumberOfConsoleInputEvents GetObjectA GetOpenFileNameA
GetPaletteEntries GetProcessVersion GetPropA GetRegionData GetSaveFileNameA GetScrollRange
GetStockObject GetStringTypeA GetStringTypeExA GetStringTypeW GetSysColorBrush GetSystemMenu
GetSystemPaletteEntries GetSystemPaletteUse GetTabbedTextExtentA GetTextExtentExPointA
GetTextExtentPoint32A GetTextExtentPointA GetTextMetricsA GetTimeFormatA GetTokenInformation
GetTopWindow GetUpdateRect GetUpdateRgn GetUserNameA GetViewportExtEx GetWindow GetWindowDC
GetWindowRect GetWindowTextA GetWindowTextLengthA GetWindowThreadProcessId GlobalAddAtomA
GlobalGetAtomNameA GrayStringA HideCaret ICInfo ImageList_Destroy ImageList_LoadImageA
ImmAssociateContext ImmGetContext ImmNotifyIME ImmReleaseContext ImmSetOpenStatus ImpersonateSelf
InflateRect InsertMenuA IntersectClipRect IntersectRect InvalidateRect InvalidateRgn
IsDialogMessageA KillTimer LCMapStringA LCMapStringW LineTo LoadAcceleratorsA LoadBitmapA
LoadCursorA LoadIconA LoadImageA LoadMenuA LoadModule LoadStringA LockFile LockFileEx
LockServiceDatabase LockWindowUpdate LookupPrivilegeValueA MCIWndCreateA MCIWndRegisterClass
MapWindowPoints MessageBeep MessageBoxA ModifyMenuA MoveToEx MoveWindow OemToCharA OffsetRect
OffsetViewportOrgEx OpenClipboard OpenFile OpenPrinterA OpenProcessToken OpenSCManagerA OpenServiceA
OpenThreadToken PatBlt PeekMessageA PlayMetaFile PlaySoundA PostMessageA PrintDlgA ReadConsoleInputA
ReadConsoleOutputCharacterA RealizePalette Rectangle RedrawWindow RegisterClassA RegisterClassExA
RegisterHotKey RegisterWindowMessageA ReleaseCapture ReleaseDC RemoveFontResourceA RemovePropA
ReplaceTextA ResizePalette RestoreDC ReuseDDElParam RevertToSelf SHBrowseForFolderA SHGetFileInfoA
SHGetPathFromIDListA SaveDC ScaleViewportExtEx ScaleWindowExtEx ScreenToClient SelectClipRgn
SelectObject SelectPalette SendMessageTimeoutA SetAbortProc SetBkMode SetCommState SetCommTimeouts
SetConsoleCtrlHandler SetConsoleScreenBufferSize SetConsoleWindowInfo SetCursorPos SetDIBColorTable
SetDIBitsToDevice SetDlgItemTextA SetForegroundWindow SetMapMode SetMenu SetMenuItemBitmaps
SetMetaFileBitsEx SetPaletteEntries SetPropA SetRect SetRectEmpty SetScrollPos SetStretchBltMode
SetSysColors SetSystemPaletteUse SetTimer SetViewportExtEx SetViewportOrgEx SetVolumeLabelA
SetWindowExtEx SetWindowPos SetWindowTextA SetWindowsHookExA ShellExecuteA ShellExecuteExA
Shell_NotifyIconA ShowCaret StartDocA StartPage StretchBlt SystemParametersInfoA TabbedTextOutA
TextOutA TranslateAcceleratorA TranslateMessage UnhookWindowsHookEx UnionRect UnlockFile
UnlockFileEx UnlockServiceDatabase UnpackDDElParam UnregisterClassA UnregisterHotKey UpdateWindow
ValidateRect VerLanguageNameA VerQueryValueA VirtualLock VirtualUnlock WaitMessage WinHelpA
WindowFromPoint WriteConsoleInputA mciGetDeviceIDA mciGetErrorStringA mmioOpenA sndPlaySoundA
timeSetEvent wvsprintfA'''.split())
# f5-20C: census W21 stubs the reused Wine DLLs bind (config/wine-upper.json
# rule ciukios-stub-dll), with the failure their Microsoft reference gives:
# no printer (BOOL FALSE; DeviceCapabilities and DocumentProperties -1) and
# no IME (BOOL FALSE; ImmGetCompositionString negative IMM_ERROR_NODATA, -1).
ZERO_FAILURE.update(('EnumPrintersA','EnumPrintersW','GetDefaultPrinterA','GetDefaultPrinterW',
                     'GetPrinterA','GetPrinterW','GetPrinterDriverA','GetPrinterDriverW','OpenPrinterW',
                     'ImmSetCompositionFontW','ImmSetCompositionWindow'))
MINUS_ONE.update(('DeviceCapabilitiesA','DeviceCapabilitiesW','DocumentPropertiesW',
                  'ImmGetCompositionStringW'))
def signature(dll,name):
    if name.startswith('#'):return None
    stem=dll if dll.endswith('.drv') else dll[:-4]
    path=CATALOG/stem/(stem+'.spec')
    if not path.exists():return None
    result=None
    for line in path.read_text().splitlines():
        match=re.match(r'^\s*(?:@|\d+)\s+(stdcall|cdecl|varargs)\s+((?:-\S+\s+)*)([^\s(]+)\s*\(([^)]*)\)',line)
        if not match or match[3]!=name:continue
        arch=re.search(r'-arch=([^ ]+)',match[2])
        if arch and not {'i386','win32'}.intersection(arch[1].split(',')):continue
        args=match[4].split();widths=[]
        for arg in args:
            if arg in ('double','int64'):widths.append(8)
            elif arg in ('long','ptr','str','wstr','float'):widths.append(4)
            else:return None
        result={'calling':match[1],'widths':widths,'source':str(path.relative_to(ROOT))}
    if not result:return None
    if dll in ('crtdll.dll','msvcrt.dll') or name in FATAL:
        convention=None
    elif name in HRESULT:convention=1
    elif name in MMRESULT:convention=2
    elif dll=='winmm.dll' and name.startswith('mciSend'):convention=3 # MCIERR_UNSUPPORTED_FUNCTION=274 below
    elif dll=='advapi32.dll' and name.startswith('Reg'):convention=3
    elif dll=='mpr.dll':convention=3
    elif name=='GetThreadPriority':convention=6
    elif name in MINUS_ONE:convention=4
    elif name in ZERO_FAILURE:convention=0
    else:convention=None
    result['convention']=convention
    if result['calling']=='varargs':result['convention']=None
    return result

DECLARATIONS='''__declspec(dllimport) void w32_stub_hit(volatile unsigned *,const char *,const char *);
__declspec(dllimport) unsigned w32_shim_stub(volatile unsigned *,const char *,const char *,unsigned);
__declspec(dllimport) void w32_report_refusal(unsigned,const char *,unsigned);'''

def body(dll,name,symbol,index):
    sig=signature(dll,name)
    if not sig or sig['convention'] is None:
        return (f'static volatile unsigned once_{index}; void {symbol}(void) {{ w32_stub_hit(&once_{index},"{dll}","{name}"); __builtin_trap(); }}',symbol,
                dict(sig or {},failure='nonreturning STATUS_NOT_IMPLEMENTED'))
    words=sum(sig['widths'])//4
    args=','.join(f'unsigned a{i}' for i in range(words)) or 'void'
    unused=''.join(f'(void)a{i};' for i in range(words))
    call='__attribute__((stdcall)) ' if sig['calling']=='stdcall' else ''
    special=''
    if name in ('CreateServiceA','CreateServiceW'):
        # The shared helper sets the documented driver error after logging.
        special='if(a4&3){w32_report_refusal(2,"advapi32.dll",5);return w32_shim_stub(&once_'+str(index)+',"advapi32.dll","'+name+'",5);}'
    value=f'w32_shim_stub(&once_{index},"{dll}","{name}",{sig["convention"]})'
    if dll=='winmm.dll' and name.startswith('mciSend'):value='('+value+',274u)'
    if dll=='user32.dll' and name=='DdeInitializeA':value='('+value+',0x400fu)'
    target=f'_{symbol}@{words*4}' if sig['calling']=='stdcall' else symbol
    failure={0:'FALSE/NULL/zero + 120',1:'E_NOTIMPL + 120',2:'MMSYSERR_NOTSUPPORTED + 120',3:'LSTATUS 120',4:'-1 + 120',6:'THREAD_PRIORITY_ERROR_RETURN + 120'}[sig['convention']]
    if dll=='winmm.dll' and name.startswith('mciSend'):failure='MCIERR_UNSUPPORTED_FUNCTION 274 + 120'
    if dll=='user32.dll' and name=='DdeInitializeA':failure='DMLERR_SYS_ERROR 0x400f + 120'
    return (f'static volatile unsigned once_{index}; unsigned {call}{symbol}({args}){{{unused}{special}return {value};}}',target,dict(sig,failure=failure))

def wide_gate(dll,name,bytes_,index,convention=None):
    """Gate actual public Unicode implementations before reading any arguments."""
    params=','.join(f'unsigned a{j}' for j in range(bytes_//4)) or 'void'
    args=','.join(f'a{j}' for j in range(bytes_//4))
    if convention is None:convention=3 if name.startswith('Reg') else 4 if name in MINUS_ONE else 0
    if name=='VerFindFileW':
        fail=f'w32_stub_hit(&wide_once_{index},"{dll}","{name}");__builtin_trap();'
    else:fail=f'return w32_shim_stub(&wide_once_{index},"{dll}","{name}",{convention});'
    return (f'extern unsigned __attribute__((stdcall)) w32_real_{name}({params});\n'
        f'static volatile unsigned wide_once_{index}; unsigned __attribute__((stdcall)) {name}({params}){{'
        f'if(w32_shim_w_stub("{name}")){{{fail}}}'
        f'return w32_real_{name}({args});}}')
