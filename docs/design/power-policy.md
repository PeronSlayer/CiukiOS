# Power policy (G11)

- **Written:** 2026-10-23 23:16 CEST for directive G11
  (`docs/directives/g7-g12-os-gaps-batch-2.md`), inventory rows PW-3, PW-4,
  PW-5, PW-6, PW-8, PW-9, SD-1 (Stand by), SD-7 and TB-8. It builds on fx-18
  and fx-25 (APM 1.2 on the T23, `acpi-events-power.md`) and on G3's end of
  session (`desktop-shell.md`). G10 owns the screen saver and the DPMS
  mechanism (`screen-saver.md` in its branch); this document owns the timers
  that decide *when* the monitor, the disks and the system power down.

## Sources

- Windows 98 Resource Kit, chapter 30, "Overview of Power Management"
  (owner's media, `build/rskt/wrkc30_4.htm`, read-only, paraphrased):
  - three built-in schemes, Home/Office Desk ("for desktop machines"),
    Portable/Laptop ("aggressive settings for running on batteries (the AC
    settings are the same as desktop)") and Always On ("similar to
    Home/Office, but has the standby timer disabled and hard disk timer
    increased"; the default for APM desktop computers); a scheme can be
    changed or saved under a new name (Save As);
  - a scheme sets System standby, Turn off monitor and Turn off hard disks,
    with separate values plugged in and running on batteries;
  - the Alarms tab: a low and a critical battery alarm, each with a level
    slider, Alarm Action with Sound alarm and Display message, "When the
    alarm goes off, the computer will" stand by or shut down, and "Force
    standby or shutdown even if a program stops responding"; "The critical
    battery alarm level must be less than the low battery alarm";
  - the Power Meter tab: the power source and the percentage for all
    batteries, with details for each battery;
  - the Advanced tab: "Show power meter on taskbar"; "click the icon to
    change Power Schemes on-the-fly", right-click for the status of each
    battery; "Prompt for password when computer goes off standby" (the
    screen-saver password); the power-button and lid choices, offered "if
    your computer supports these options";
  - APM: "the APM BIOS is also responsible for monitoring the battery
    status and requesting a suspend if the battery is getting low"; under
    APM 1.0/1.1 "the only way to determine if the computer-supported
    standby" was to try it and some BIOSes crashed; "APM 1.2 provides a way
    to determine if standby is supported, and the operating system uses
    standby only if the computer supports it".
- Microsoft Win32 documentation:
  [WM_POWERBROADCAST](https://learn.microsoft.com/en-us/windows/win32/power/wm-powerbroadcast)
  (0x0218; return TRUE when processed),
  [PBT_APMQUERYSUSPEND](https://learn.microsoft.com/en-us/windows/win32/power/pbt-apmquerysuspend)
  (wParam 0; lParam bit 0: the application may prompt the user; deny with
  `BROADCAST_QUERY_DENY`; about 20 s for an application to take the message
  before it counts as agreeing),
  [PBT_APMPOWERSTATUSCHANGE](https://learn.microsoft.com/en-us/windows/win32/power/pbt-apmpowerstatuschange)
  (0x0A, lParam zero; sent on a change of AC or battery state, "when battery
  life drops to less than 5 minutes, or when the percentage of battery life
  drops below 10 percent, or if the battery life changes by 3 percent"; the
  application calls GetSystemPowerStatus),
  [SYSTEM_POWER_STATUS](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-system_power_status)
  (ACLineStatus 0/1/255; BatteryFlag 1 high above 66 %, 2 low below 33 %,
  4 critical below 5 %, 8 charging, 128 no system battery, 255 unknown;
  BatteryLifePercent 0..100 or 255; BatteryLifeTime and BatteryFullLifeTime
  seconds or -1, -1 also on AC). The legacy event values come from the same
  family of pages and Wine's `winuser.h`: QUERYSUSPEND 0, QUERYSTANDBY 1,
  QUERYSUSPENDFAILED 2, QUERYSTANDBYFAILED 3, SUSPEND 4, STANDBY 5,
  RESUMECRITICAL 6, RESUMESUSPEND 7, RESUMESTANDBY 8, BATTERYLOW 9,
  POWERSTATUSCHANGE 10, OEMEVENT 11, RESUMEAUTOMATIC 18;
  `BROADCAST_QUERY_DENY` 0x424D5144; `PBTF_APMRESUMEFROMFAILURE` 1.
- [APM 1.2](https://intel-vintage-developer.eu5.org/IAL/POWERMGM/APMV12.PDF)
  sections 4.3 and 4.6 (already cited by fx-18): events 0001h/0002h system
  standby/suspend request, 0009h/000Ah user standby/suspend request, 0005h
  battery low, 0006h power status change; 4.6.17 Get Capabilities (5310h)
  reports global standby and suspend support.
- ATA/ATAPI-6 (T13 1410D r3b) 8.15 IDENTIFY DEVICE word 82 bit 3 (Power
  Management feature set supported) and 8.14 IDLE (E3h): the Count register
  sets the drive's standby timer: 0 disabled, 1..240 in units of 5 s, 241..251
  in units of 30 min ((n-240) x 30 min), 252 = 21 min, 253 vendor-specific,
  255 = 21 min 15 s; the drive enters Standby (spun down) after that much
  idle time and returns to Active on the next media access by itself. An
  unsupported value or command ends with ERR and ABRT in the Error register.
  [hdparm(8)](https://man7.org/linux/man-pages/man8/hdparm.8.html) `-S` sets
  the same timer with the same encoding.

## Expected behaviour

### Power schemes (PW-3, CP-15)

1. Three built-in schemes with the Windows 98 names, plus up to eight saved
   by the user. Each scheme has three timers for AC and three for battery:
   System standby, Turn off monitor, Turn off hard disks. A timer is Never
   (0) or one of 1, 2, 3, 5, 10, 15, 20, 25, 30 and 45 minutes, 1, 2, 3, 4
   and 5 hours (the Windows 98 list).
2. The numeric defaults are CiukiOS choices that satisfy the Resource Kit's
   relations, because no primary source available here lists Windows 98's
   numbers (decision recorded for the lead):

   | Scheme | AC standby / monitor / disks | Battery standby / monitor / disks |
   | --- | --- | --- |
   | Home/Office Desk | 20 min / 15 min / 30 min | 20 min / 15 min / 30 min |
   | Portable/Laptop | 20 min / 15 min / 30 min (same as desktop) | 5 min / 2 min / 3 min |
   | Always On | Never / 15 min / 1 hour | Never / 15 min / 1 hour |

3. The active scheme defaults to Portable/Laptop when the power record
   reports a battery bay, to Always On on an APM machine without a battery
   (the Resource Kit's default for APM desktops) and to Home/Office Desk
   otherwise, until the user chooses one. Built-in schemes can be changed;
   only saved schemes can be deleted. "Save As" asks for a name (1..31
   characters, unique, no control characters, no `,` or `=`, which the
   settings line uses).
4. The settings live in `/system/settings/desktop.cfg` as `power_*` lines,
   kept by `settings.c` among its unknown keys and parsed only by
   `apps/desktop/power_policy.c`. Control Panel writes them and asks the
   desktop to reload (`DESK_SETTINGS_RELOAD`), like the other pages.
5. The timer that applies is the one of the current power source: battery
   when AC is off line or on APM backup power, AC otherwise (unknown AC
   counts as AC, so a desktop never takes the battery values).
6. **Turn off monitor:** the desktop publishes the active source's value as
   its monitor timeout. G10's idle engine and DPMS call consume it at merge
   (G10 has one `monitor_seconds` setting; G11 replaces its source, not its
   mechanism). Until then the value is computed, shown and host-tested only.
7. **Turn off hard disks:** whenever the active value changes (scheme, AC
   change, start of the desktop) the desktop sends it to the kernel with call
   73 POLICY. The kernel programs every identified ATA fixed disk whose
   IDENTIFY word 82 bit 3 is set with IDLE (E3h) and the standby-timer count
   for that time, rounded up to the next representable period (25 min
   becomes 30 min, the drive never spins down earlier than asked); Never is
   count 0 (timer disabled). Packet devices are skipped. An ABRT from the
   drive is a refusal, recorded for that disk; it never quarantines the
   channel (any other failure keeps the existing whole-channel rule). The
   drive spins up by itself on the next access; the existing 30 s ATA
   command deadline covers a spin-up. The call queues the time for the
   kernel's `power-policy` task and returns at once (an IDLE to a drive in
   standby spins it up, which must not stall the desktop); its out fields
   report the last completed application (disks programmed, refused, the
   programmed time, the first error) and EVENTS reports the programmed time.
8. **System standby after:** when standby is available (see Standby below)
   and no input has arrived for the active value, the desktop starts Stand
   by. A Win32 or DOS full-screen display lease, a running session (Shut
   Down, Log Off) or a running power sequence keep the computer awake while
   they last. When standby is not available the timer is shown disabled and
   never acts.

### Power meter and tray (PW-4, TB-8)

9. The tray battery icon is shown when the power record is valid and "Show
   power meter on taskbar" is set (default on). Hiding it re-lays out the
   tray.
10. Its tooltip: on battery, `<h> hr <mm> min (<p>%) remaining`, or
    `<p>% remaining` when the time is unknown, `On battery power` when the
    charge is unknown; on AC, `Charging (<p>%)`
    while a battery charges, `Fully charged (100%)` at 100 %, `AC power
    (<p>%)` otherwise, `AC power` without battery facts; `Battery status
    unknown` when nothing is known. A second line names the active scheme.
    The combined percentage is the capacity-weighted total of the present
    batteries when they share a unit (fx-18 rule), and the time is the
    largest firmware time of a discharging battery.
11. A left click opens a menu of the schemes with the active one checked;
    choosing one makes it active and saves it. A right click opens
    "Open Power Meter" (Control Panel, Power, Power Meter tab) and "Adjust
    Power Properties" (Power Schemes tab).
12. The Power Meter tab shows the power source, the total, and with "Show
    details for each battery" one line per battery: present or not
    installed, charge, state, remaining time.

### Alarms (PW-5)

13. Defaults: low alarm on at 10 % with Display message; critical alarm on
    at 3 % with Display message and Sound alarm and the action Stand by when
    standby is available, Shut down otherwise. Levels are 0..100 % in 1 %
    steps (Windows 98 slider); saving forces critical below low (critical =
    low - 1 when the user sets it at or above low; with low 0 the critical
    alarm is turned off).
14. An alarm goes off only while the computer runs on battery, once per
    discharge: when the combined percentage falls to its level or below. It
    rearms when AC returns or the percentage rises above its level. When a
    reading jumps below both levels, only the critical alarm goes off; the
    low one is latched with it. On APM, the BIOS's Battery Low event (0005h)
    sets off the low alarm (if not latched) even without a percentage.
15. Actions, in order: one sound, the message (a desktop message window
    with the warning icon), then the action: Stand by or Shut down. With a
    message the window's own warning sound is the alarm; without one, Sound
    alarm plays Information (low) or Error (critical). Dedicated "Low/Critical
    Battery Alarm" sound events need new sound-pack payloads and are a gap. Shut
    down runs G3's orderly session (every program asked); with "Force
    standby or shutdown even if a program stops responding" a program that
    stops responding during that session is ended as End Task would,
    instead of showing the "not responding" dialog. A Stand by that is not
    available falls back to Shut down. No action is taken while a session or
    power sequence already runs.
16. Win32 programs receive PBT_APMBATTERYLOW when the low alarm goes off.

### Power button, lid and BIOS requests (PW-8, SD-7)

17. "When I press the power button": Shut down (default) or Stand by (listed
    only when standby is available). A press (the ACPI record's
    `shutdown_requests` changes) runs the choice; a Stand by choice that is
    no longer available runs Shut down.
18. "When I close the lid": None (default), Stand by (when available) or Shut
    down. It acts on the record's `lid_open` changing from 1 to 0 (ACPI
    description; firmware-owned ACPI machines may never update it).
19. APM standby and suspend requests from the BIOS (0001h, 0002h, 0009h,
    000Ah: a sleep key, the lid on most APM laptops, BIOS idle timers) are
    still rejected by the kernel while standby is unqualified (fx-18). The
    kernel now counts them (call 73 EVENTS) and the desktop shows "Stand by
    is not available on this computer" once per burst (10 s).
20. "Prompt for password when computer goes off standby" is stored and
    shown; it is disabled with the note that passwords come with G32 (the
    screen-saver password is also G32).

### Stand by (PW-6, SD-1)

21. Stand by is available only when call 73 QUERY returns
    `CIUKI_POWER_CAN_STANDBY`. The kernel reports it only when all of these
    hold, and otherwise states the first missing one in the EVENTS record:
    - a provider is active (APM connected, or ACPI owned);
    - the provider reports and can enter a sleep state: APM 1.2 Get
      Capabilities (5310h) bit 0 or 1, or an evaluated ACPI `\_S1`/`\_S3`
      with a wake path;
    - every native driver that holds DMA or interrupt state (network
      including NDIS, audio, USB host controllers) has a suspend and a
      resume path that restores it.

    None of these drivers has a resume path today (`power_devices_quiesce`
    is the terminal S5 barrier and quarantines its claims), the f1-121 APM
    broker stops at 530Fh, and ACPI never offers S1/S3. So the kernel
    reports `CIUKI_POWER_STANDBY_NO_RESUME` (or an earlier reason) on every
    machine and `CIUKI_POWER_STANDBY` returns ENOTSUP. This is the honest
    state required by the Resource Kit ("the operating system uses standby
    only if the computer supports it"); the device resume work is the
    remaining gap of this item.
22. When available, the Shut Down dialog lists "Stand by" first, like
    Windows 98, and the desktop's Stand by sequence is:
    PBT_APMQUERYSUSPEND to every Win32 window (lParam 1 when the user asked,
    0 for timers, alarms, buttons and the lid); a `BROADCAST_QUERY_DENY`
    ends it with PBT_APMQUERYSUSPENDFAILED to the windows that agreed; else
    PBT_APMSUSPEND, call 73 STANDBY, then PBT_APMRESUMEAUTOMATIC and
    PBT_APMRESUMESUSPEND (lParam `PBTF_APMRESUMEFROMFAILURE` when the call
    failed). A query left unanswered for 20 s counts as agreement. Native
    and DOS windows are not queried (gap: no native message yet).

### Applications (PW-9)

23. Win32 windows receive WM_POWERBROADCAST, delivered on their own USER
    thread through the w32srv broker's existing session poll (no new RPC per
    message loop): PBT_APMPOWERSTATUSCHANGE on a change of AC state, of the
    SYSTEM_POWER_STATUS battery flag, on the remaining time falling below
    5 minutes, on the percentage falling below 10 % and on a change of 3 %
    or more since the last broadcast; PBT_APMBATTERYLOW (16); the Stand by
    sequence (22). DefWindowProc returns TRUE. One broadcast runs at a time;
    a status change arriving during one is coalesced into one more.
24. GetSystemPowerStatus (KERNEL32) returns SYSTEM_POWER_STATUS from the
    call-73 record through a new Win32 system action: ACLineStatus 0, 1 or
    255 (APM backup power is 0: the machine is not on mains); BatteryFlag
    from the combined percentage (1 above 66 %, 2 below 33 %, 4 below 5 %, 8
    while charging, 128 with no battery, 255 unknown); BatteryLifePercent or
    255; BatteryLifeTime from the firmware time on battery, otherwise -1;
    BatteryFullLifeTime -1.

### Lead decisions (2026-10-24)

- Turn off hard disks starts at Never in every built-in scheme until the ATA
  standby and wake path is qualified on hardware; the user can still choose a
  time. The other Resource Kit relations are unchanged.
- Stand by stays unqualified on every machine until a separate directive adds
  suspend and resume to the network (NDIS), audio and USB drivers, APM Get
  Capabilities (5310h) and the provider standby operation.

## Interfaces

### Call 73 (no new syscall number)

The highest call-73 action on `main` is STATUS (4). G11 adds:

| Action | Value | Arguments | Result |
| --- | --- | --- | --- |
| `CIUKI_POWER_STANDBY` | 5 | none | ENOTSUP while not qualified (always, today); desktop only |
| `CIUKI_POWER_POLICY` | 6 | C = `struct ciuki_power_policy`, D = its size | 0, EINVAL, EFAULT, ENODEV (no ATA disk); desktop only |
| `CIUKI_POWER_EVENTS` | 7 | C = `struct ciuki_power_events`, D = its size | 0; any native client, cached, no firmware call |

QUERY gains `CIUKI_POWER_CAN_STANDBY` (16).

`struct ciuki_power_policy` (version 1, 32 bytes): `version`, `size`,
`disk_seconds` (in), `disks`, `refused`, `applied_seconds`, `error`
(negative errno of the first failure) (out), `reserved` (zero).

`struct ciuki_power_events` (version 1, 40 bytes): `version`, `size`,
`sleep_requests` (APM 0001h, 0002h, 0009h, 000Ah), `battery_low` (0005h),
`critical` (0008h), `resumes` (0003h, 0004h, 000Bh), `status_changes`
(0006h, 000Ch), `standby_reason` (`CIUKI_POWER_STANDBY_*`: AVAILABLE 0,
NO_PROVIDER 1, NO_FIRMWARE 2, NO_RESUME 3), `disk_seconds` (the last
programmed time, `CIUKI_POWER_UNKNOWN` before any), `reserved`. Counters are
monotonic for the boot; the desktop acts on changes.

### Win32 broker (no new op)

`W32R_ENDSESSION` gains indices 8 `W32R_POWER_BEGIN` (desktop only: type =
PBT event, flags = lParam), 9 `W32R_POWER_STATUS` (key 0 running, 1 done, 2
denied; count = windows that granted) and 10 `W32R_POWER_RESET`. The USER
thread's existing session POLL returns a pending power message (type
0x0218, count = wParam, disposition = lParam) when no end-of-session
message is pending; its REPLY carries flags 0 for `BROADCAST_QUERY_DENY`,
1 otherwise. An end of session always has priority.

### Win32 system action

`W32_S_POWER` (after `W32_S_TIME_ZONE`): low = AC (0/1/255), high = battery
flag, value = percent (255 unknown), extra = remaining seconds
(0xffffffff unknown), result = full-life seconds (0xffffffff).

## Evidence

Host tests only (no QEMU, no image): `tests/host/desktop/power_policy_test.c`
(schemes, persistence, tooltip, alarms, idle, broadcast rules),
`tests/host/power_g11_test.c` (call-73 actions, counters, standby reasons),
`tests/host/ata_test.c` (IDLE timer encoding, ABRT without quarantine),
the desktop and Control Panel tests and the window-model row "power". The
hardware behaviour (drive spin-down on the T23, the APM counters) needs the
lead's T23 qualification.

## Implementation map

- Kernel: `core/power.c` (POLICY on the `power-policy` task, EVENTS,
  STANDBY), `core/power_source.c` (APM event counters, standby reason,
  `power_source_win32_status`), `core/power_quiesce.c`
  (`power_devices_resume_ready`, false), `drivers/ata.c` (`ata_standby_timer`,
  ABRT classification), `include/ciuki/ata_power.h` (count encoding),
  `proc/win32/system.c` (`W32_S_POWER`).
- SDK: `ciuki_power_events`, `ciuki_power_policy` (`<ciuki/power.h>`
  version 2); KERNEL32 `GetSystemPowerStatus`; USER `DefWindowProc` and the
  session pump answer WM_POWERBROADCAST.
- w32srv: `session_impl.h` (`wr_power_*`), `W32R_POWER_*` in `w32/server.h`.
- Desktop: `apps/desktop/power_policy.c` (pure policy), `shell.c` (tray,
  menus, alarms, button, lid, broadcast queue, Stand by sequence, idle
  timer), `desktop.c` (call-73 polling and the drive timer),
  `win32_native.c` (broker seam), `client.c` (alarm Force).
- Control Panel: `apps/control/power.inc`; `--panel=power-meter` opens the
  Power Meter tab (the tray's "Open Power Meter").
