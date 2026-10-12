# f2-41 — Control Panel: Device Manager, Drivers and the remaining system pages

- **Model/effort:** `gpt-6.1-sol`, `xhigh` (kernel query + UI). **Worktree:** `../CiukiOS-wt-f2-41` from `main`. Host-testable; the lead qualifies on QEMU/T23.

## Owner request (2026-10-12)

"Manca proprio la cartella del pannello di controllo dove modificare altre cose come Gestione dispositivi e Driver ed eventuali cose che ha un OS normale."

## Scope

1. **Kernel query** (call 75 `ciuki_devices`, SDK header `ciuki/devices.h`): a read-only snapshot of the device registry (`src/kernel/core/registry*`, PCI functions, ISA/legacy devices: 8042, PIT, RTC, UART, ATA channels and units, framebuffer, audio, NIC if present) with for each: class, PCI ids, bus/slot/function, resources (I/O ranges, MMIO ranges, IRQ), bound driver name and its state (ready/absent/quarantined/firmware-first with the reason), activation sequence and generation. Bounded (at most 64 entries), versioned struct like `ciuki_display_info`.
2. **Control Panel becomes a folder-style panel**: the main window lists applets as icons (Display, Mouse, Keyboard, Sounds, Date & Time, System, Device Manager, Drivers, Network (placeholder until F6), Power) and opens each as a page, like Windows 98's Control Panel; the existing tabbed pages become applets without losing behaviour. Files > Places gets "Control Panel".
3. **Device Manager applet**: tree by class (Computer → System devices, Display adapters, Keyboards, Mice, Disk drives, CD-ROM drives, Sound, Network adapters, Ports) with Tango icons, a Properties dialog per device (General: name, ids, driver, status text; Resources: I/O, memory, IRQ), Refresh. Driver names from a small table (PCI vendor/device → product name for the hardware baseline chips; class names otherwise).
4. **Drivers applet**: the kernel's driver list (name, version/build, devices bound, firmware-first policy where it applies) and the loader/firmware leases.
5. **System applet**: General (CiukiOS version, build, CPU from CPUID brand/family, RAM, uptime), Hardware (summary), Advanced (boot flags, serial log on/off written to BOOT.CFG for the next boot).
6. Tests: registry snapshot marshalling and bounds, applet navigation, tree building from a synthetic snapshot, renders to `build/f2-41/*.png`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`. Report to `build/codex/f2-41-report.md`.
