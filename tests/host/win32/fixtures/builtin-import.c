/* Minimal builtin DEF imports, not a guest DLL implementation. SPDX-License-Identifier: MIT */
typedef void *HMODULE;
typedef int (__cdecl *Proc)(void);
__declspec(dllimport) HMODULE __stdcall LoadLibraryA(const char *);
__declspec(dllimport) HMODULE __stdcall LoadLibraryExA(const char *, void *, unsigned long);
__declspec(dllimport) HMODULE __stdcall GetModuleHandleA(const char *);
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(HMODULE, char *, unsigned long);
__declspec(dllimport) Proc __stdcall GetProcAddress(HMODULE, const char *);
__declspec(dllimport) int __stdcall FreeLibrary(HMODULE);
int entry(void) {
    HMODULE a = LoadLibraryA("provider.dll");
    HMODULE b = LoadLibraryA("provider.dll");
    char path[260];
    if (!a || a != b || GetModuleHandleA("provider.dll") != a) return 1;
    if (!GetModuleFileNameA(a,path,sizeof path)) return 2;
    Proc p = GetProcAddress(a,"FixtureValue");
    if (!p || p() != 7) return 3;
    if (!FreeLibrary(a) || GetModuleHandleA("provider.dll") != b) return 4;
    if (!FreeLibrary(b) || GetModuleHandleA("provider.dll")) return 5;
    HMODULE data = LoadLibraryExA("compiled-resources.exe",0,2);
    if (!data || !FreeLibrary(data)) return 6;
    return 0;
}
