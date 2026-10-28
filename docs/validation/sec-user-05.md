# sec-user-05: application-facing identity (contract row 6B)

Research, repository comparison and decisions for the sec-user-05 step of
`docs/design/user-security-contract.md` (sections 4.1 "Authorized profile
information", 8.1-8.4, decisions K-Q8, A1-10, A2-8) and the kernel half's
"Read-only token/SID facade and privilege view". Host evidence is recorded in
`build/codex/sec-user05-report.md` of the implementation worktree; no QEMU or
hardware evidence is claimed here.

## Sources consulted (2026-10-27)

- Microsoft, [GetTokenInformation](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-gettokeninformation):
  sized queries report the required length and fail with ERROR_INSUFFICIENT_BUFFER;
  TOKEN_* records are self-contained (SID pointers into the caller's buffer).
- Microsoft, [CheckTokenMembership](https://msdn.microsoft.com/en-us/library/aa376389):
  a NULL handle checks the caller's effective identity; an explicit handle must be
  an impersonation token (the kernel half fixes ERROR_BAD_TOKEN_TYPE, 1349, for a
  primary token). [CheckTokenMembershipEx](https://learn.microsoft.com/en-us/windows/desktop/api/Securitybaseapi/nf-securitybaseapi-checktokenmembershipex)
  and [SHTestTokenMembership](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shtesttokenmembership)
  were compared for IsUserAnAdmin.
- ReactOS advapi32 [token.c](https://doxygen.reactos.org/d5/d2e/dll_2win32_2advapi32_2token_2token_8c_source.html)
  and [RtlCheckTokenMembership](https://ntdoc.m417z.com/rtlchecktokenmembership) as
  implementation comparisons only (no code taken).
- Microsoft access tokens, privilege constants and well-known SIDs pages already
  cited by sec-k06 (`docs/validation/sec-k06.md`).
- Microsoft, [CSIDL](https://learn.microsoft.com/en-us/windows/win32/shell/csidl):
  CSIDL_PROGRAMS/STARTMENU/STARTUP/DESKTOPDIRECTORY are the user's folders,
  CSIDL_COMMON_PROGRAMS/STARTMENU/STARTUP/DESKTOPDIRECTORY the All Users ones
  ("files and folders that appear on the desktop for all users").
- The Old New Thing, [2009-07-30](https://devblogs.microsoft.com/oldnewthing/20090730-00/?p=17293),
  and the Explorer++ [desktop](https://explorerplusplus.readthedocs.io/en/latest/menus/go/desktop.html)
  notes: the desktop is a virtual folder built from the per-user and the All Users
  Desktop folders.
- Wine [progman_dde.c tests](https://doxygen.reactos.org/db/d72/progman__dde_8c_source.html)
  (`use_common`): before Vista, Program Manager DDE groups went to
  CSIDL_COMMON_PROGRAMS for an administrator and to CSIDL_PROGRAMS otherwise;
  Microsoft's Program Groups [error text](https://technet.microsoft.com/en-us/library/cc958979.aspx):
  only administrators create or change common groups, others get personal groups.
- Microsoft [KB179365, Run and RunOnce](https://support.microsoft.com/kb/179365):
  the order of Run entries is not guaranteed; no Microsoft source orders the All
  Users and the personal StartUp folders (community reports differ by version).
- Microsoft, [RegOpenKeyA](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regopenkeya)
  ("uses the default security access mask"; an empty subkey returns the same
  handle), [RegOpenKeyExA](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regopenkeyexa)
  (the call fails only when the key's security does not permit the requested
  access), [Requesting Access Rights to an Object](https://learn.microsoft.com/en-us/windows/win32/secauthz/requesting-access-rights-to-an-object)
  (MAXIMUM_ALLOWED: "all the access rights that are valid for the caller") and
  [Registry Key Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-key-security-and-access-rights)
  (KEY_READ 0x20019). Wine [advapi32/registry.c](https://github.com/wine-mirror/wine/blob/master/dlls/advapi32/registry.c):
  RegOpenKey and RegCreateKey pass MAXIMUM_ALLOWED; RegConnectRegistry on the
  local computer is RegOpenKey(hKey, NULL).
- POSIX [getpwnam/getpwnam_r](https://man7.org/linux/man-pages/man3/getpwnam_r.3p.html):
  not found returns 0 with a null result; ERANGE for short caller storage; the
  non-reentrant forms leave errno unchanged when no entry exists.

## Decisions

1. **Win32 token/SID facade** (`sdk/win32/advapi32/identity.c`). OpenProcessToken
   creates the kernel's W32_OBJ_TOKEN query handle; only query rights (TOKEN_QUERY,
   TOKEN_QUERY_SOURCE, READ_CONTROL, MAXIMUM_ALLOWED, GENERIC_READ) are accepted,
   anything else is ERROR_ACCESS_DENIED before a handle exists. OpenThreadToken is
   ERROR_NO_TOKEN (no impersonation in this subset). GetTokenInformation serves
   TokenUser, TokenGroups, TokenPrivileges, TokenOwner, TokenPrimaryGroup, TokenType
   and TokenSessionId from the kernel credential, with i386 SDK layouts; unknown
   classes (TokenElevation included) are ERROR_INVALID_PARAMETER. The local account
   SID uses the protected per-install machine SID (REGISTRY RB_MACHINE_ID over
   ntdll's existing link, cached per process); a failure is an error, never a
   fabricated SID. AdjustTokenPrivileges, SetTokenInformation, CreateProcessAsUser,
   LogonUser and ConvertSidToStringSid keep their typed W21 refusals.
2. **GetUserNameA/W** return the account name of the caller's kernel credential
   (ACCOUNTS LOOKUP; "SYSTEM" for uid 0), never the profile label the desktop
   selects in w32srv; without a published account the result is
   ERROR_NOT_LOGGED_ON.
3. **LookupAccountSid** resolves SYSTEM (NT AUTHORITY), the three built-in groups
   (BUILTIN), the local domain and current stored accounts (computer name as the
   domain); another domain or an unknown uid is ERROR_NONE_MAPPED; a remote system
   name is ERROR_NOT_SUPPORTED, as for LookupPrivilegeValue (kernel half).
4. **Shell folders.** Personal CSIDLs come from HKCU Shell Folders (the broker
   provisions them from the committed root) or, when absent, from USERPROFILE;
   CSIDL_PROFILE (40) and CSIDL_LOCAL_APPDATA (28, `Local Settings\Application Data`,
   no longer the roaming folder) were added; common folders stay machine
   locations. IsUserAnAdmin is a CheckTokenMembership(NULL, Administrators) wrapper
   in the platform shell.
5. **TEMP, console and logs.** Win32 startup DOS-spells the kernel-generated
   USERPROFILE/TEMP/TMP and defaults an absent TMP/TEMP to the profile's
   `Local Settings\Temp`; AllocConsole creates its backing in GetTempPath's
   directory, creating the private folder, and fails rather than falling back to
   C:\WINDOWS\TEMP. Only a profile-less process (SYSTEM tools, the pre-logon
   bootstrap desktop until sec-user-03) keeps the Windows 98 defaults, which the
   kernel path policy leaves writable to administrators only. Native tmpfile/tmpnam
   use TMPDIR; desktop/Files/Win32 launchers forward the session's profile paths;
   NetSurf keeps its profile in Application Data and its log in Local Settings;
   desktop crash reports go to the owner's profile.
6. **Native passwd** (`sdk/libciuki/pwd.c`, `account_link.c`): LOOKUP then
   authorized PROFILE over a private ACCOUNTS link closed after each call;
   foreign private mappings are EACCES; pw_passwd is `*`; uid 0 is a synthetic
   SYSTEM entry; primary gid Guests (546) or Users (545).
7. **Per-user joystick preferences** (sec-k04 open item): one active settings
   scope. Interactive CALIBRATE/ROUTE/CONFIGURE/SAVE_GAME change the session's own
   store `<profile>\Settings\Joystick`, read and written as the session desktop's
   retained subject, reading the machine record where the profile has none; the
   machine store stays SYSTEM-only. Session end or subject revocation returns to
   the machine store. A controller with no record in either store keeps its
   runtime calibration until re-attached (no driver default-reset exists).
8. **Per-user mixer preferences** (sec-k04 open item): a profile session saves its
   master and source levels in its personal Desktop root and re-applies them at
   session start through the designated control fd; System\Audio and AUDIO.CFG
   (machine defaults) are no longer written by sessions.
9. **Printing for Standard users** (sec-k05 open item): each profile session
   queues jobs in `<profile>\Settings\Spool`, serviced by that session's worker
   (native libprint, Win32 gdi32, DOS LPT capture); the printer configuration
   remains the machine's `/WINDOWS/SPOOL/printers` (administrator-writable,
   readable by all). The kernel doorbell recognizes both queue roots and signals a
   profile queue's worker only when it has the publisher's uid and session.
10. **Windows XP shell folders in a profile session** (lead addition). The
    desktop of a kernel profile session takes its own `Start Menu/Programs` and
    `Documents/Desktop` from USERPROFILE (G32 local profiles keep theirs). The
    Start menu lists the personal Programs and `C:\WINDOWS\Start Menu\Programs`
    (All Users), same-named groups as one submenu; the personal share of the
    64-link snapshot is capped at a quarter so the All Users entries (Quake III,
    installed games) stay. The desktop view (`apps/desktop/desktop_union.h`)
    lists the personal Desktop and then the shared `/Documents/Desktop` entries
    the user has not shadowed; `C:\WINDOWS\Desktop` shortcuts keep arriving
    through the shortcut scan. New items go to the personal folder (made at
    start); renames and deletions of shared items act in place, under the
    kernel's checks; the Files model hands out physical paths. StartUp runs the
    All Users folder and then the user's. CSIDL_COMMON_* stay the machine
    folders: an administrator's installer writes there and a Standard user's
    write is refused by the kernel path policy. Program Manager DDE follows the
    pre-Vista rule: an administrator's groups are common unless
    CommonGroupFlag is 0, a Standard user's are personal whatever the flag,
    lookups without a flag try the administrator's common folder first and
    everybody else's personal folder first, and Groups lists both once each.
    Not changed: Add/Remove Programs' leftover-shortcut cleanup still walks the
    machine folders only, and HKLM keeps only the Common Desktop/Common
    Programs values.
11. **Registry access for Standard users** (lead addition, from sec-wave3fix).
    advapi32 and ntdll no longer turn MAXIMUM_ALLOWED into full access in the
    Windows 9x way: they ask w32srv for the full mask and, when the sec-k06
    policy refuses it (5), for KEY_READ, keeping any explicitly named right in
    both masks. RegOpenKey, RegCreateKey and RegConnectRegistry use
    MAXIMUM_ALLOWED as Wine does. A Standard user therefore reads
    HKLM\Software through RegOpenKey, RegOpenKeyEx(MAXIMUM_ALLOWED) and
    RegConnectRegistry(NULL, HKLM); a refused create of an existing key opens
    it for reading (disposition REG_OPENED_EXISTING_KEY), a new key and every
    explicit write right stay ERROR_ACCESS_DENIED; administrators keep full
    access. The service keeps admitting whole masks: handles do not record a
    granted mask, every later operation is checked again by the policy. The
    w32srv empty-subkey rule of wt/sec-wave3fix (bfd0d618d) is carried
    unchanged so RegConnectRegistry returns the predefined handle.
