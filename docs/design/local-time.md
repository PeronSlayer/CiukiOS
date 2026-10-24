# Local time, time zone and daylight saving (G1)

Status: implemented in branch `wt/g1` (directive G1, 2026-10-23), host tests
only; not yet qualified on QEMU or the T23. Inventory rows TM-1, TM-2, TM-3,
TB-6, AX-15, FM-2 (dates), CP-4.

## Reference behaviour (Windows 98)

| Topic | Windows behaviour | Source |
| --- | --- | --- |
| Date/Time applet | Control Panel "Date/Time: change date, time, and time zone settings"; two tabs, Date & Time (month calendar, clock, time edit, current zone name) and Time Zone (zone list, "Automatically adjust clock for daylight saving changes") | Windows 98 Resource Kit, ch. 23, Table 23.1 (`build/rskt/wrkc23_2.htm`) |
| System time | The system keeps UTC; SetLocalTime converts with the current zone and daylight-saving setting and needs SE_SYSTEMTIME_NAME | [SetLocalTime](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-setlocaltime) |
| Zone information | GetTimeZoneInformation: Bias, StandardName/Date/Bias, DaylightName/Date/Bias; dates in the "day-in-month" form (wDay 1–5, 5 = last); return UNKNOWN/STANDARD/DAYLIGHT | [GetTimeZoneInformation](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-gettimezoneinformation), [TIME_ZONE_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/ns-timezoneapi-time_zone_information) |
| FAT time stamps | FAT records LOCAL time; FindFirstFile/GetFileTime convert with the CURRENT zone and daylight-saving settings; FileTimeToLocalFileTime uses the current bias | [File Times](https://learn.microsoft.com/en-us/windows/win32/sysinfo/file-times) |
| RTC | PCs set up for Windows keep local time in the battery clock (Windows reads it as local unless `RealTimeIsUniversal`); a daylight-saving change moves the clock | owner's T23 (fx-11 open item, `docs/directives/t23-423b803a-trial-fixes.md`); [Arch wiki, System time](https://wiki.archlinux.org/title/System_time) |
| Auto adjust off | The "Automatically adjust" check box sets `DisableAutoDaylightTimeSet`; the zone then stays in standard time | [BCL team, Exploring Windows time zones](https://learn.microsoft.com/en-us/archive/blogs/bclteam/exploring-windows-time-zones-with-system-timezoneinfo-josh-free) |

POSIX rules: POSIX.1-2024 XBD 8.3 (`TZ`: `std offset dst [offset],start[/time],end[/time]`,
`Mm.w.d`, `Jn`, `n`, default 02:00) and RFC 9636 §3.3.1 (rule hours −167..167).
The bundled zones (`src/kernel/include/ciuki/tzdata.h`, 60 entries) carry the
POSIX footer line of the IANA tz database, release 2026e
(<https://www.iana.org/time-zones>), read from the compiled TZif files.

## CiukiOS model

* **One evaluator.** `src/kernel/lib/tzrule.c` (MIT, no library calls) parses
  rules, gives the offset at a UTC instant, converts wall time to UTC (a
  repeated hour resolves to its first, daylight, occurrence; a skipped hour is
  read in standard time, so 02:30 on the spring day is 03:30 daylight) and
  encodes `system/settings/clock.cfg`. The kernel, libciuki (`tz.c`,
  `tzrule.c`), kernel32 (`time.c`), Setup, libsetup and the tests use it.
* **Kernel time is UTC.** `proc/timezone.c` holds the zone. clock.cfg version
  2: `rtc_local`, `offset_minutes` (standard), `zone`, `tz`, `auto_dst`,
  `rtc_bias_minutes` — the offset of the local time the RTC holds now.
  Version 1 files (f9-04b) are read as a fixed offset. Boot reads RTC − bias,
  so a change that happened while the machine was off is read correctly.
* **The RTC follows local time.** Every change of zone, clock or daylight
  period rewrites the RTC (local time when `rtc_local`) and the bias; the
  configuration is written first and restored if the RTC write fails. The
  daylight-saving check runs on every `GET` (the desktop asks once a second);
  a move sets `ADJUSTED` and the desktop shows "CiukiOS has adjusted your
  clock for daylight saving changes." once, then acknowledges.
* **Authority.** Setting the clock or the zone needs `time_capability`
  (desktop and Control Panel, the SE_SYSTEMTIME_NAME equivalent). Win32
  SetLocalTime/SetSystemTime without it fail with ERROR_PRIVILEGE_NOT_HELD.
* **Native programs.** libciuki `localtime`, `mktime`, `tzset`, `ctime` and
  `strftime %z/%Z` follow `TZ`; when `TZ` is unset they follow the live system
  zone. The supervisor and the desktop's launchers pass `TZ=<system rule>`
  (the standard part when automatic adjustment is off); the desktop updates
  its own `TZ` when the zone generation changes. Win32 programs get no `TZ`
  so that their C runtime reads GetTimeZoneInformation, as on Windows.
* **FAT.** New stamps are the local time of the running clock (not a CMOS
  read); `stat` converts a stamp to UTC with the rule of its own instant, so
  libc shows the stored wall time in every season. kernel32 converts FAT
  stamps with the current bias, exactly as Microsoft documents for
  FindFirstFile/GetFileTime, so FileTimeToLocalFileTime returns the stored
  stamp. The DOS file API sees the stored stamp unchanged.
* **DOS VMs.** INT 1Ah AH=02h/04h and the CMOS clock registers (ports 70h/71h,
  read-only: 00h–09h, 0Ah–0Dh, 32h) give the system local time; the BIOS tick
  count starts at the local time of day. CiukiDOS's CLOCK$ reads them, so
  INT 21h 2Ah/2Ch agree. INT 1Ah AH=03h/05h (DOS TIME/DATE) move that VM's
  clock only (`time_offset`): a DOS program cannot change the system clock.
* **Win32.** GetLocalTime evaluates the rule at the instant; GetTimeZoneInformation
  returns the Windows key names of the bundled zone ("W. Europe Standard
  Time"), Bias/DaylightBias and the day-in-month dates (a change after 24:00,
  Egypt's `/24`, is reported as 23:59:59.999); UNKNOWN (0) with zero dates
  when the zone has no daylight time or automatic adjustment is off.
* **Date/Time Properties** (`apps/control/datetime.inc`): opened from Control
  Panel, the tray clock's right-click menu and a double-click on the tray
  clock. Apply sets the zone first, then the local date and time.
* **Setup.** The zone list is the bundled list; an Italian keyboard selects
  Europe/Rome until the user picks a zone. SETUP.INF `TimeZone=` accepts a
  bundled IANA id or the old fixed `+hh:mm`. Setup writes clock.cfg version 2
  and the registry values `System\TimeZone` `OffsetMinutes`, `RTCLocal`,
  `Zone`, `TZ`, `AutoDST`.

## Known gaps

* No `SetTimeZoneInformation`, no WM_TIMECHANGE broadcast to Win32 windows
  after a change, and Win32 programs read the record on each call (no cached
  zone).
* A DOS VM that is running across a daylight-saving change keeps its old
  wall time until it is restarted (its tick count is virtual); the Windows 98
  VM model is the same for DOS sessions that do not reread the RTC.
* No map on the Time Zone tab and no network time synchronisation (TM-3).
* Running native programs keep the `TZ` they were started with; programs
  that leave `TZ` unset follow the system zone at once.
* The bundled list is the IANA rule in force today: historical rule changes
  (before 1996 in the EU) are not represented.
