/* SPDX-License-Identifier: GPL-2.0-only
 * Project-owned, dependency-free Win32 console and installer probes.
 * Build with /DPROBE_INSTALL=1 for SETUP.EXE.  Both target the Windows 95
 * API surface; neither contains or links Microsoft code. */
typedef unsigned long DWORD;
typedef long LONG;
typedef void *HANDLE;
typedef HANDLE HKEY;
#define WINAPI __stdcall
#define IMPORT __declspec(dllimport)
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define HKEY_CURRENT_USER ((HKEY)0x80000001UL)
#define REG_SZ 1UL
#define KEY_SET_VALUE 0x0002UL
#define CREATE_ALWAYS 2UL
#define FILE_ATTRIBUTE_NORMAL 0x80UL
#define GENERIC_WRITE 0x40000000UL

IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT int WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT void WINAPI ExitProcess(DWORD);
#ifdef PROBE_INSTALL
IMPORT int WINAPI CreateDirectoryA(const char *, void *);
IMPORT HANDLE WINAPI CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
IMPORT LONG WINAPI RegCreateKeyExA(HKEY, const char *, DWORD, char *, DWORD,
                                   DWORD, void *, HKEY *, DWORD *);
IMPORT LONG WINAPI RegSetValueExA(HKEY, const char *, DWORD, DWORD,
                                  const unsigned char *, DWORD);
IMPORT LONG WINAPI RegCloseKey(HKEY);
#endif

static DWORD length(const char *p) { DWORD n = 0; while (p[n]) n++; return n; }
static void say(const char *p)
{
    DWORD done;
    WriteFile(GetStdHandle((DWORD)-11), p, length(p), &done, 0);
}

void WINAPI mainCRTStartup(void)
{
#ifdef PROBE_INSTALL
    static const char path[] = "C:\\PROGRAMS\\CiukiProbe";
    static const char file[] = "C:\\PROGRAMS\\CiukiProbe\\README.TXT";
    static const char note[] = "CiukiOS free Win32 installer probe\r\n";
    HKEY key;
    HANDLE h;
    DWORD done, disposition;
    CreateDirectoryA("C:\\PROGRAMS", 0);
    CreateDirectoryA(path, 0);
    h = CreateFileA(file, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { say("SETUP: file failed\r\n"); ExitProcess(2); }
    if (!WriteFile(h, note, length(note), &done, 0)) {
        say("SETUP: write failed\r\n"); ExitProcess(3);
    }
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\CiukiProbe", 0, 0, 0,
                        KEY_SET_VALUE, 0, &key, &disposition)) {
        say("SETUP: registry key failed\r\n"); ExitProcess(4);
    }
    if (RegSetValueExA(key, "InstallPath", 0, REG_SZ,
                       (const unsigned char *)path, sizeof path)) {
        say("SETUP: registry value failed\r\n"); ExitProcess(5);
    }
    RegCloseKey(key);
    say("SETUP: PASS\r\n");
#else
    say("WIN32: PASS\r\n");
#endif
    ExitProcess(0);
}
