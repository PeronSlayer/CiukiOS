/* sec-user-05 H11: production Win32 startup environment (services/start.c),
 * console backing (kernel32/console.c) and shell folders (shell/folders.c)
 * in the host model. Only the file system, registry and loader boundaries
 * are fakes; a Standard user's view denies every write below the shared
 * C:\WINDOWS\TEMP (kernel path policy: /Windows is administrator-writable).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <w32/ldr.h>
#include <w32/shims.h>
#include <w32/shell.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static unsigned checks;
_Thread_local struct w32_teb *w32_model_teb;
static struct w32_teb teb;
static struct w32_peb peb;
static struct w32_process_parameters parameters;
static DWORD last_error;
DWORD W32API GetLastError(void) { return last_error; }
void W32API SetLastError(DWORD e) { last_error = e; }
void *k32_alloc(DWORD n) { return malloc(n ? n : 1); }
int w32_copy_string(char *out, uint32_t cap, const char *s) /* ntdll strings.c */
{ uint32_t n = (uint32_t)strlen(s); if (!cap || n >= cap) { if (cap) out[0] = 0; return 0; } memcpy(out, s, n + 1); return 1; }
void k32_free(void *p, DWORD n) { (void)n; free(p); }

/* ------------------------------------------------- start.c loader stubs */
static void *meta_blocks[64];
static void *meta_alloc(void *o, uint32_t n)
{
    (void)o;
    for (unsigned i = 0; i < 64; i++) if (!meta_blocks[i]) return meta_blocks[i] = calloc(1, n ? n : 1);
    return NULL;
}
static void meta_free(void *o, void *p, uint32_t n)
{
    (void)o; (void)n;
    for (unsigned i = 0; i < 64; i++) if (p && meta_blocks[i] == p) { free(p); meta_blocks[i] = 0; }
}
static void meta_release_all(void) { for (unsigned i = 0; i < 64; i++) { free(meta_blocks[i]); meta_blocks[i] = 0; } }
NTSTATUS w32_ciuki_start(struct w32_loader *, struct w32_peb *, const char *, const char *, const char *,
                         const char *, struct w32_module **);
HRESULT W32API SHGetFolderPathW(HANDLE, int, HANDLE, DWORD, WCHAR *);
static const struct w32_services services = {.alloc = meta_alloc, .free = meta_free};
const struct w32_services w32_ciuki_services = {.alloc = meta_alloc, .free = meta_free};
void w32_ldr_init(struct w32_loader *l, const struct w32_services *s, struct w32_peb *p)
{ memset(l, 0, sizeof(*l)); l->services = s; (void)p; }
void w32_ldr_bind(struct w32_loader *l) { (void)l; }
int w32_builtin_validate(const struct w32_builtin *b, ULONG n, struct w32_error *e) { (void)b; (void)n; (void)e; return 1; }
WCHAR w32_acp_character(uint8_t c) { return c; }
int w32_utf8_path(const char *s, WCHAR *out, ULONG cap)
{ ULONG i = 0; for (; s[i] && i + 1 < cap; i++) out[i] = (uint8_t)s[i]; out[i] = 0; return !s[i]; }
int w32_utf16_path(const WCHAR *s, ULONG n, char *out, ULONG cap)
{ ULONG i = 0; for (; i < n && i + 1 < cap; i++) out[i] = (char)s[i]; out[i] = 0; return i == n; }
int w32_shim_start(struct w32_loader *l, const char *exe) { (void)l; (void)exe; return 1; }
int w32_ldr_thread_attach(struct w32_loader *l, struct w32_teb *t) { (void)l; (void)t; return 1; }
int w32_ldr_load(struct w32_loader *l, const char *n, ULONG f, struct w32_module **m) { (void)l; (void)n; (void)f; *m = 0; return 1; }
NTSTATUS w32_ldr_start_process(struct w32_loader *l, const char *e, struct w32_module **m) { (void)l; (void)e; *m = 0; return 0; }
/* Runs the production startup on a native environment; returns the
 * resulting Win32 block as one ASCII string with '|' between entries. */
static void startup(const char *native, size_t bytes, char out[1024])
{
    struct w32_loader l;
    struct w32_module *main;
    memset(&peb, 0, sizeof(peb));
    w32_ldr_init(&l, &services, &peb);
    CHECK(!w32_ciuki_start(&l, &peb, "C:\\X.EXE", "x", "C:\\", native, &main));
    (void)bytes;
    const WCHAR *e = peb.ProcessParameters->Environment;
    size_t at = 0;
    for (size_t i = 0; e[i] || e[i + 1]; i++) out[at++] = e[i] ? (char)e[i] : '|';
    out[at] = 0;
    peb.ProcessParameters = NULL;
    meta_release_all();
}

/* ----------------------------------------------- fake file system (C:) */
static char dirs[64][200], files[64][200];
static unsigned ndirs, nfiles;
static int standard_user; /* deny writes below the shared machine TEMP */
static int denied(const char *p) { return standard_user && !strncasecmp(p, "C:\\WINDOWS\\TEMP", 15); }
static int find(char list[][200], unsigned n, const char *p)
{ for (unsigned i = 0; i < n; i++) if (!strcasecmp(list[i], p)) return (int)i; return -1; }
DWORD W32API GetFileAttributesA(const char *p)
{
    if (!strcmp(p, "C:") || find(dirs, ndirs, p) >= 0) return 16;
    if (find(files, nfiles, p) >= 0) return 32;
    last_error = 2; return ~0u;
}
BOOL W32API CreateDirectoryA(const char *p, SECURITY_ATTRIBUTES *sa)
{
    (void)sa;
    if (find(dirs, ndirs, p) >= 0 || find(files, nfiles, p) >= 0) { last_error = 183; return 0; }
    if (denied(p)) { last_error = 5; return 0; }
    char parent[200]; snprintf(parent, sizeof(parent), "%s", p);
    char *slash = strrchr(parent, '\\'); if (slash) *slash = 0;
    if (strcmp(parent, "C:") && find(dirs, ndirs, parent) < 0) { last_error = 3; return 0; }
    CHECK(ndirs < 64); snprintf(dirs[ndirs++], 200, "%s", p); return 1;
}
static unsigned handles;
HANDLE W32API CreateFileA(const char *p, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD create, DWORD flags, HANDLE t)
{
    (void)access; (void)share; (void)sa; (void)flags; (void)t;
    int exists = find(files, nfiles, p) >= 0;
    if (create == 1 && exists) { last_error = 80; return K32_INVALID; }
    if (create == 3 && !exists) { last_error = 2; return K32_INVALID; }
    if (!exists) {
        if (denied(p)) { last_error = 5; return K32_INVALID; }
        char parent[200]; snprintf(parent, sizeof(parent), "%s", p);
        char *slash = strrchr(parent, '\\'); if (slash) *slash = 0;
        if (find(dirs, ndirs, parent) < 0) { last_error = 3; return K32_INVALID; }
        CHECK(nfiles < 64); snprintf(files[nfiles++], 200, "%s", p);
    }
    handles++; return (HANDLE)(uintptr_t)(0x100 + nfiles);
}
BOOL W32API CloseHandle(HANDLE h) { (void)h; CHECK(handles); handles--; return 1; }
BOOL W32API WriteFile(HANDLE h, const void *d, DWORD n, DWORD *done, OVERLAPPED *o) { (void)h; (void)d; (void)o; if (done) *done = n; return 1; }
int W32API WideCharToMultiByte(DWORD cp, DWORD f, const WCHAR *s, int n, char *o, int cap, const char *d, BOOL *u)
{ (void)cp; (void)f; (void)d; (void)u; int i = 0; for (; (n < 0 || i < n) && i < cap; i++) { o[i] = (char)s[i]; if (n < 0 && !s[i]) return i + 1; } return i; }
int W32API MultiByteToWideChar(DWORD cp, DWORD f, const char *s, int n, WCHAR *o, int cap)
{ (void)cp; (void)f; int i = 0; for (; (n < 0 || i < n) && i < cap; i++) { o[i] = (uint8_t)s[i]; if (n < 0 && !s[i]) return i + 1; } return i; }
/* GetTempPath reads the process environment (TMP, TEMP, USERPROFILE) as
 * kernel32/files.c does; the value under test comes from start.c above. */
static char temp_value[200];
DWORD W32API GetTempPathA(DWORD n, char *out)
{
    size_t len = strlen(temp_value);
    if (n <= len) return (DWORD)len + 1;
    memcpy(out, temp_value, len + 1); return (DWORD)len;
}
DWORD W32API GetTempFileNameA(const char *path, const char *prefix, DWORD unique, char *out)
{
    (void)unique;
    for (unsigned i = 1; i < 100; i++) {
        snprintf(out, 260, "%s%.3s%04X.TMP", path, prefix, i);
        HANDLE h = CreateFileA(out, 0x40000000u, 0, 0, 1, 128, 0);
        if (h != K32_INVALID) { CloseHandle(h); return i; }
        if (last_error != 80) return 0;
    }
    return 0;
}
BOOL W32API AllocConsole(void);
BOOL W32API FreeConsole(void);

/* --------------------------------------------- shell folder boundaries */
static char environment_profile[200];
DWORD W32API GetEnvironmentVariableA(const char *name, char *out, DWORD cap)
{
    if (strcmp(name, "USERPROFILE") || !environment_profile[0]) { last_error = 203; return 0; }
    size_t n = strlen(environment_profile);
    if (cap <= n) return (DWORD)n + 1;
    memcpy(out, environment_profile, n + 1); return (DWORD)n;
}
static const char *hkcu_local_appdata; /* HKCU Shell Folders "Local AppData", or NULL */
LONG W32API RegOpenKeyExA(DWORD key, const char *path, DWORD o, DWORD a, DWORD *out)
{ (void)o; (void)a; CHECK(key == 0x80000001u && strstr(path, "Shell Folders")); *out = 7; return 0; }
LONG W32API RegQueryValueExA(DWORD key, const char *name, void *r, DWORD *type, void *data, DWORD *n)
{
    (void)r; CHECK(key == 7);
    if (strcmp(name, "Local AppData") || !hkcu_local_appdata) return 2;
    size_t len = strlen(hkcu_local_appdata) + 1; if (*n < len) return 234;
    memcpy(data, hkcu_local_appdata, len); *n = (DWORD)len; *type = 1; return 0;
}
LONG W32API RegCloseKey(DWORD key) { CHECK(key == 7); return 0; }
HANDLE W32API GetProcessHeap(void) { return (HANDLE)1; }
void *W32API HeapAlloc(HANDLE h, DWORD f, DWORD n) { (void)h; (void)f; return calloc(1, n); }
BOOL W32API HeapFree(HANDLE h, DWORD f, void *p) { (void)h; (void)f; free(p); return 1; }
static int admin_member, membership_calls;
BOOL W32API CheckTokenMembership(HANDLE token, const void *sid, BOOL *member)
{
    static const uint8_t administrators[16] = {1,2,0,0,0,0,0,5, 32,0,0,0, 0x20,2,0,0};
    membership_calls++;
    CHECK(!token && !memcmp(sid, administrators, 16));
    *member = admin_member; return 1;
}
BOOL W32API IsUserAnAdmin(void);

int main(void)
{
    teb.ProcessEnvironmentBlock = &peb;
    w32_model_teb = &teb;
    char block[1024];
    /* 1. Profile-less process (SYSTEM tools, interim bootstrap): IO.SYS defaults. */
    startup("PATH=C:\\TOOLS\0\0", 0, block);
    CHECK(!strcmp(block, "PATH=C:\\TOOLS|TMP=C:\\WINDOWS\\TEMP|TEMP=C:\\WINDOWS\\TEMP"));
    /* 2. A LOGON-launched session: the kernel's native profile values are
     *    DOS-spelled; no shared machine TEMP appears. */
    static const char session[] =
        "USERPROFILE=/Windows/Profiles/u00000003\0TEMP=/Windows/Profiles/u00000003/Local Settings/Temp\0"
        "TMP=/Windows/Profiles/u00000003/Local Settings/Temp\0HOME=/Windows/Profiles/u00000003/Documents\0\0";
    startup(session, sizeof(session), block);
    CHECK(!strcmp(block, "USERPROFILE=C:\\Windows\\Profiles\\u00000003|"
                         "TEMP=C:\\Windows\\Profiles\\u00000003\\Local Settings\\Temp|"
                         "TMP=C:\\Windows\\Profiles\\u00000003\\Local Settings\\Temp|"
                         "HOME=/Windows/Profiles/u00000003/Documents"));
    CHECK(!strstr(block, "WINDOWS\\TEMP"));
    /* 3. A profile with absent TMP/TEMP gets the profile's own temporary folder,
     *    migrated uid 1007 keeping its retained u00000003 spelling. */
    startup("USERPROFILE=/Windows/Profiles/u00000003\0\0", 0, block);
    CHECK(!strcmp(block, "USERPROFILE=C:\\Windows\\Profiles\\u00000003|"
                         "TMP=C:\\Windows\\Profiles\\u00000003\\Local Settings\\Temp|"
                         "TEMP=C:\\Windows\\Profiles\\u00000003\\Local Settings\\Temp"));
    /* 4. A Win32 parent's DOS-spelled values pass unchanged; an inherited TEMP wins. */
    startup("USERPROFILE=C:\\Windows\\Profiles\\u00000003\0TEMP=C:\\Windows\\Profiles\\u00000003\\X\0\0", 0, block);
    CHECK(!strcmp(block, "USERPROFILE=C:\\Windows\\Profiles\\u00000003|TEMP=C:\\Windows\\Profiles\\u00000003\\X|"
                         "TMP=C:\\Windows\\Profiles\\u00000003\\Local Settings\\Temp"));
    puts("H11 Win32 startup environment: profile-less defaults, DOS-spelled profile USERPROFILE/TEMP/TMP, derived private Temp, inherited values PASS");

    /* Console backing for a Standard user with the shared TEMP denied. */
    peb.ProcessParameters = &parameters;
    standard_user = 1;
    snprintf(dirs[ndirs++], 200, "C:\\Windows");
    snprintf(dirs[ndirs++], 200, "C:\\Windows\\Profiles");
    snprintf(dirs[ndirs++], 200, "C:\\Windows\\Profiles\\u00000003");
    snprintf(dirs[ndirs++], 200, "C:\\WINDOWS\\TEMP");
    snprintf(temp_value, sizeof(temp_value), "C:\\Windows\\Profiles\\u00000003\\Local Settings\\Temp\\");
    CHECK(AllocConsole());
    CHECK(find(dirs, ndirs, "C:\\Windows\\Profiles\\u00000003\\Local Settings\\Temp") >= 0);
    CHECK(nfiles == 1 && !strncmp(files[0], "C:\\Windows\\Profiles\\u00000003\\Local Settings\\Temp\\W32", 53));
    CHECK(parameters.StandardOutput && parameters.StandardOutput == parameters.StandardError && parameters.ConsoleHandle);
    CHECK(!AllocConsole() && last_error == 5); /* one console */
    CHECK(FreeConsole() && !parameters.StandardOutput && !handles);
    /* Two consoles never share a backing file (exclusive creation). */
    CHECK(AllocConsole() && nfiles == 2 && strcmp(files[0], files[1]) && FreeConsole());
    /* No private Temp obtainable: an error, never the shared machine TEMP. */
    snprintf(temp_value, sizeof(temp_value), "C:\\WINDOWS\\TEMP\\");
    unsigned before = nfiles;
    CHECK(!AllocConsole() && last_error == 5 && nfiles == before && !parameters.StandardOutput && !handles);
    snprintf(temp_value, sizeof(temp_value), "C:\\Windows\\Profiles\\u00000003\\Local Settings\\Temp\\");
    snprintf(files[nfiles++], 200, "C:\\Windows\\Profiles\\u00000004");
    snprintf(temp_value, sizeof(temp_value), "C:\\Windows\\Profiles\\u00000004\\Temp\\");
    CHECK(!AllocConsole() && last_error == 267 && !handles);
    puts("H11 console backing: Standard user's private Temp created, exclusive names, cleanup, shared TEMP denied without fallback PASS");

    /* Shell folders of a profile session. */
    char path[260]; WCHAR wide[260];
    snprintf(environment_profile, sizeof(environment_profile), "C:\\Windows\\Profiles\\u00000003");
    CHECK(SHGetSpecialFolderPathA(0, path, 40, 0) && !strcmp(path, "C:\\Windows\\Profiles\\u00000003"));
    CHECK(SHGetSpecialFolderPathA(0, path, 28, 0) &&
          !strcmp(path, "C:\\Windows\\Profiles\\u00000003\\Local Settings\\Application Data"));
    CHECK(!SHGetFolderPathA(0, 28 | 0x8000, 0, 0, path) && find(dirs, ndirs, path) >= 0);
    CHECK(!SHGetFolderPathW(0, 40, 0, 0, wide) && wide[0] == 'C' && wide[2] == '\\');
    hkcu_local_appdata = "C:\\Windows\\Profiles\\u00000003\\Local Settings\\Application Data";
    CHECK(SHGetSpecialFolderPathA(0, path, 28, 0) && !strcmp(path, hkcu_local_appdata));
    hkcu_local_appdata = NULL;
    CHECK(SHGetSpecialFolderPathA(0, path, 35, 0) && !strcmp(path, "C:\\WINDOWS\\Application Data")); /* common: machine */
    /* XP shell folders: CSIDL_PROGRAMS/DESKTOPDIRECTORY/STARTUP/STARTMENU are the
     * user's, CSIDL_COMMON_* the All Users ones an administrator's installer writes. */
    static const struct { int id; const char *path; } xp[] = {
        {2, "C:\\Windows\\Profiles\\u00000003\\Start Menu\\Programs"}, {23, "C:\\WINDOWS\\Start Menu\\Programs"},
        {16, "C:\\Windows\\Profiles\\u00000003\\Documents\\Desktop"}, {25, "C:\\WINDOWS\\Desktop"},
        {7, "C:\\Windows\\Profiles\\u00000003\\Start Menu\\Programs\\StartUp"}, {24, "C:\\WINDOWS\\Start Menu\\Programs\\StartUp"},
        {11, "C:\\Windows\\Profiles\\u00000003\\Start Menu"}, {22, "C:\\WINDOWS\\Start Menu"}};
    for (unsigned i = 0; i < sizeof(xp) / sizeof(*xp); i++)
        CHECK(SHGetSpecialFolderPathA(0, path, xp[i].id, 0) && !strcmp(path, xp[i].path));
    /* Without a profile: the Windows 98 single-user layout; no CSIDL_PROFILE. */
    environment_profile[0] = 0;
    CHECK(SHGetSpecialFolderPathA(0, path, 28, 0) && !strcmp(path, "C:\\WINDOWS\\Application Data"));
    CHECK(!SHGetSpecialFolderPathA(0, path, 40, 0) && last_error == 87);
    /* A traversal in USERPROFILE is not a location. */
    snprintf(environment_profile, sizeof(environment_profile), "C:\\Windows\\Profiles\\..\\..\\Program Files");
    CHECK(!SHGetSpecialFolderPathA(0, path, 40, 0));
    admin_member = 1; CHECK(IsUserAnAdmin());
    admin_member = 0; CHECK(!IsUserAnAdmin() && membership_calls == 2);
    puts("H11 shell folders: CSIDL_PROFILE/LOCAL_APPDATA and the XP per-user Programs/Desktop/StartUp/Start Menu from the profile, HKCU value wins, CSIDL_COMMON_* machine, no-profile layout, IsUserAnAdmin membership PASS");
    printf("H11 Win32 facades: checks=%u PASS\n", checks);
    return 0;
}
