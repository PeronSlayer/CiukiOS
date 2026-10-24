# Startup programs (G21)

The Windows 98 Resource Kit, chapter 27, "Using the System Configuration
Utility" (`wrkc27_2.htm`, local read-only owner copy), specifies individual
Startup-group and RUN/RUN SERVICES enable/disable controls. Chapter 25 is the
inventory's application reference; chapter 27 also documents the next-start
RunOnce entry WUCheckShortcut. Microsoft's [Run and RunOnce documentation](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)
confirms machine/user Run keys, one-shot values deleted before dispatch, and
unspecified order within a key. Its NT administrator restriction does not
apply to CiukiOS's Windows 98 model. Microsoft's [keyboard reference](https://learn.microsoft.com/en-us/answers/questions/2427684/f-numbers-on-top-of-keyboard)
records Shift at startup bypassing the Startup folder. G21 explicitly requests
Shift to skip startup programs; CiukiOS applies it to the whole logon batch.

## Implementation contract

At each ordinary desktop start, after the first presented frame and a one-second
input grace interval, queue one background startup pass. A Shift press seen
before that pass skips it for this session without changing persistent state.
Safe Mode and test/gate desktops do not dispatch startup programs. Input queued
before the shell's creation is drained through the ordinary input stream.

Snapshot HKLM RunServicesOnce, RunServices, HKLM/HKCU RunOnce and HKLM/HKCU Run,
then `/WINDOWS/Start Menu/Programs/StartUp`. The existing .lnk codec and shared
native/DOS/PE launcher preserve arguments and show state. Regular executable
files in StartUp are supported too. Registry command lines use quoted target
parsing without a shell; bare names resolve in /WINDOWS, /WINDOWS/SYSTEM and
/Programs. Absolute native and C: paths are accepted. Malformed items remain visible for disabling and produce an explicit per-item
error rather than a truncated command; other valid entries still dispatch. The batch is bounded at 128
entries; overflow fails the snapshot rather than silently omitting entries.
The existing virtual-restart queue remains separate and unchanged.

Once values are re-read, compared with the snapshot, and deleted durably before
launch. Failed launches do not restore consumed entries. No automatic retry of
ambiguous mutations is permitted. Extended RunOnce prefixes keep their existing
virtual-restart semantics; the logon pass uses delete-before-launch semantics.

System > Startup lists the same snapshot with a selected-item enable checkbox.
Disabled records stay in their original locations; a Desktop registry sidecar
stores their source/name identity and enabled state. This preserves installer
ownership and prevents re-enabling one source from affecting another. Changes
affect the next desktop start and never kill a running process. An empty list,
unavailable registry, malformed command and overflow are distinct model states.
The System page groups snapshot failures into an unavailable message.
The canonical image already creates StartUp and advertises it in Shell Folders.
The rest of BT-11 (configuration-file editing and boot-menu selection) belongs
to G14 and remains a gap.

Startup-generated windows use the existing CREATE_WINDOW, activation and
client-presentation damage paths; no special full-screen repaint is added.

## Host validation and integration boundary

`tests/host/desktop/test_startup.py` exercises the production model and real
w32srv dispatcher/storage through a copied fake transport, including persistent
selection, once consumption, stale snapshots, failure continuation and bounds.
The Control Panel startup controls and two raster pins are in
`test_game_properties.py`; the fx-15 table and `test_window_model.py` include
background startup window creation/activation/close. No ABI, public SDK record,
syscall or message number changes. The implementation adds no panic paths.

The normal-logon runner currently applies delete-before-launch to all RunOnce
names, including `!` names; post-success deletion prefixes are not newly
implemented here. Per-user Startup folders follow the future user-profile
work (G32); the current HKCU remains w32srv's existing .DEFAULT profile.
Runtime image/hardware validation belongs to the lead.
