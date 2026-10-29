# Integration 9 glue

Research and implementation decisions for `docs/directives/integ9-glue.md`.

- Wine 10.0's [WinINet DllMain](https://gitlab.winehq.org/wine/wine/-/blob/wine-10.0/dlls/wininet/internet.c)
  allocates TLS, then calls [init_urlcache](https://gitlab.winehq.org/wine/wine/-/blob/wine-10.0/dlls/wininet/urlcache.c).
  The pinned release source in `build/external/wine-10.0` confirms that the
  latter creates **one auto-reset event and one semaphore**, then initializes
  cache containers. The Wine GitLab pages and the URL-cache GitHub mirror could
  not be fetched here; the Internet entry point was also checked through the
  [upstream mirror](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/wininet/internet.c).
  TLS allocation is local to KERNEL32. H1 serves semaphores but refuses event
  creation with ENOSYS; production `objects.c` already serves events. Extend
  the existing eight-object H1 service with event create/set/reset and
  single-object waits, preserving auto/manual reset semantics, named reuse,
  generations and teardown. Unsupported operations and unsatisfied blocking
  waits still fail closed. Do not bypass Wine's constructor or change its PEs.
- Microsoft's [CreateEvent documentation](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw)
  defines initial state, manual/auto reset, case-sensitive named reuse and
  collisions across object types. Match those rules in the bounded H1 subset.
- Cache container initialization requests CSIDL_INTERNET_CACHE (32),
  CSIDL_COOKIES (33) and CSIDL_HISTORY (34), with directory creation enabled.
  CiukiOS's production shell table and registry seeds omit all three: this is
  a separate production gap even though Wine tolerates unavailable containers
  during attach. Add the locations to the existing folder resolver and seed
  both Shell Folders families in new default/profile hives. Keep existing
  persisted user edits/deletions intact; an absent profile value uses the
  existing USERPROFILE fallback. Directories are created through the existing
  permission-checked CreateDirectory path on request, not in the image.
- Microsoft's [CSIDL reference](https://learn.microsoft.com/en-us/windows/win32/shell/csidl)
  identifies the three per-user repositories. The owner's Windows 98 Resource
  Kit, `build/rskt/wrkc07_3.htm` (User Profiles), lists Cookies, History and
  Temporary Internet Files beneath a user's profile; `wrkappb_3.htm`
  (Windows 98 Directory Structure) gives the profile-less History and cache
  locations beneath Windows. Use these Windows 98 locations rather than
  importing the Windows XP Local Settings layout for these folders.
- COM_CLEAR_ERRORS uses the existing own-session port-owner policy. Map its
  census row to the fx-hlutils comm facade test that calls ClearCommError and
  checks the copied error and queue status.

Exact commands, outcomes and environment limits are in
`build/codex/integ9-report.md`. Host adapter execution is not evidence of a real
CiukiOS boot; session validation belongs to the lead.
