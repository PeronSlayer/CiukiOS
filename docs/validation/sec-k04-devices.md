# sec-k04 device and administrative enforcement

Scope: sec-wave3 row 3B; host evidence only. The binding policy is
[user-security-kernel.md](../design/user-security-kernel.md), especially K-Q4,
K-Q8 and the device/action inventory. No new driver or maintenance protocol is
introduced by this implementation.

Primary sources checked before implementation:

- Microsoft [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens):
  process identity, groups and logon identity accompany authorization. Decision:
  use immutable kernel credentials, never executable names, capability flags or
  environment strings as administrative authority.
- Microsoft [SetTimeZoneInformation](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-settimezoneinformation):
  XP/2000 require the system-time privilege for time-zone changes. Decision:
  clock and zone mutation require administrator membership.
- Microsoft [XP Security Guide chapter 3](https://learn.microsoft.com/ko-kr/security-updates/security/20213971):
  client security recommendations distinguish shutdown from clock, driver and
  volume maintenance rights. These recommendations are not universal XP defaults.
- Microsoft [removable media policy](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/jj852171(v=ws.11))
  and [XP burning permissions](https://learn.microsoft.com/en-us/windows/win32/imapi/providing-user-permissions-for-media-burning-devices):
  device administration is distinct from ordinary local media use. Decision:
  administrator for format/conversion/defrag/raw snapshots; allow the interactive
  session safe removal/audio with existing busy/generation checks. No burning API.
- Microsoft [logon-screen shutdown policy](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/jj852274(v=ws.11)):
  logon-screen power is a separate workstation policy. K-Q8 explicitly permits
  Guest and trusted logon-screen shutdown/restart. Device grants confer no
  machine-policy authority.

Repository audit found unchecked raw reads in `fs/disklease.c`, executable-path
grants in `proc/supervisor.c`, capability-flag checks in network/time/recovery,
and missing `INPUT_KEYBOARD_SET` forwarding. The integrated baseline already
has `CIUKI_SETUP_NET_QUERY` in `net_devctl`, reached only through PX_NET;
its existing Setup flag check now also requires the SYSTEM Setup role. Checks belong
inside the real operation, before storage or hardware effects. Exclusive leases,
mounted/pagefile/log blockers, generations and forced-shutdown fencing remain
mandatory. Managed-system raw I/O remains denied to administrators; unsupported
maintenance uses the existing typed kernel storage service, admitted by the
protected SYSTEM controller only after checking the retained administrator
subject. This creates no raw-I/O grant or credential change.

The implementation report in `build/codex/sec-k04-report.md` records exact host
commands, results, integration seams and remaining evidence.


Policy census for row 3B (the full selector spelling remains in the binding
kernel contract; unknown selectors keep their typed rejection):

| Action family | Enforcing scope / outcome |
| --- | --- |
| DISK_GET_INFO / LEASE_STATE | Public copied geometry and bounded lease state. |
| DISK raw pread | Actual issuer must be administrator; whole system disk and physical aliases deny every user. |
| DISK raw pwrite / FLUSH / RELEASE | Same rights plus current holder PID, exclusive lease and durability/published-range checks. Dup/inheritance carries no authority. |
| LEASE_ACQUIRE / PUBLISH / RECHECK / SET_PHASE | SYSTEM Setup role, noninherited Setup flag and Setup boot, plus existing transaction/phase checks. |
| PARTITION_ACQUIRE | Administrator on a non-system disk, with mounted/pagefile/log/active-I/O blockers unchanged. |
| MEDIA state/volume/space/blockers/label/format/convert and CD info queries | Public nonsecret copied inventory; no private file paths. |
| MEDIA safe-remove/floppy-load/CD audio, transport and tray | Live local interactive credential or SYSTEM; underlying generation, busy and flush checks remain. Guest is admitted. |
| MEDIA label set | Guest denied; Users on unmanaged removable media, administrator on fixed data, SYSTEM execution plus administrator retained subject on system media. |
| MEDIA format/convert/defrag/analyze and floppy raw snapshots | Administrator, with protected-system maintenance requiring a SYSTEM worker and live retained administrator client. Original medium/lease fencing remains. |
| RECOVERY status/report | Public bounded summary; authority bit derives from the credential. |
| RECOVERY set/enable/scan (including read-only scans) | Administrator; whole-system-volume scan only through the retained-client SYSTEM entry. |
| NETCTL GET / SET / RENEW / RELEASE | Public GET; administrator mutation from any program. Path flags are irrelevant. |
| NET_INFO config/stats/routes/sockets | Public configuration/statistics/routes; socket details filtered by uid/session; administrators exclude SYSTEM sockets. Socket operations also reject foreign credentials on retained descriptors. |
| TIME GET / SET_ZONE / SET_LOCAL / SET_UTC / ACKNOWLEDGE | Public GET and internal DST maintenance; administrator for every explicit mutation/acknowledge. |
| POWER queries/status/events / local actions / machine policy | Public observations; live session desktop (including Guest) coordinates local actions; machine policy requires administrator. SYSTEM logon UI has coordinated shutdown/restart only. Force fencing/deadlines are unchanged. |
| INPUT keyboard/HID/joystick | Exact input-owner grant and live subject. Keyboard dispatch reaches its existing transient handler. Persistent joystick mutation is unsupported for interactive callers until profile-scoped storage exists; it cannot write machine defaults. |
| COM / LPT | Live local credential plus existing port/generation/stream checks. Serial-mouse machine binding additionally requires administrator. No new permanent routing API. |
| AUDIO streams/capture and MIXER | Existing stream/exclusive capture ownership. Mutating master/line controls require the designated desktop or SYSTEM plus the original control fd. Session master changes and EC keys cannot save SYSTEM/AUDIO.CFG or leak through shutdown flushing. The profile preference adapter remains a consumer integration. |
| DISPLAY / acceleration / DPMS | Existing compositor/grant/token/generation checks remain; screen-control requests also require the live session credential. Kernel screen PERSIST is a session preference notification, not permission to write machine defaults. Secure grant revocation is sec-k07. |
| Task authority / signal routes | Optional authority fd validates its kind only; own uid/session or administrator controls ordinary tasks. PID 1, kernel controllers, SYSTEM services and desktop/controller groups retain their protections. kill retains PGID plus uid/session checks. |
| DOS/program flags / Win32 service roles / generic file access | k05/k06/k03 respectively; no edits to their owned implementations. The remaining DOS/program path checks are resource checks for those rows, not administrative network/time/recovery authority. |

Integration boundaries:

- `media_devctl_subject` and `recovery_syscall_subject` accept a forwarded retained
  subject only from SYSTEM workers; ordinary syscalls use their own immutable
  subject. `security_storage_broker` admits their typed kernel maintenance
  operation through the protected SYSTEM controller before the storage engine.
  It never changes the requesting process credential or grants raw access.
- Personal joystick persistence has no profile adapter yet. It fails with
  EOPNOTSUPP for interactive callers rather than altering machine defaults.
- The mixer reports applied-but-unsaved/EOPNOTSUPP for session master changes;
  its profile consumer must persist the value privately. SYSTEM boot/default
  persistence and its alternating durable records remain available.
- No public SDK or abi.h changes, new driver loader, new panic paths, QEMU,
  systemd scope, boot image or development-diary changes accompany this work.
