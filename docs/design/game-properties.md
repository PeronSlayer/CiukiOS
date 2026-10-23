# Per-game joystick and port properties (f2-125)

The existing Control Panel / Game controllers entry opens one Game properties
view for each catalog game. Desktop game shortcuts open the same view. The
controller page retains live testing, calibration and the global DOS route;
there is no DOS Box settings applet. Edits are staged until Apply. Game changes,
Refresh, return and Close discard unsaved edits. Saved changes take effect only
at creation of the next VM. Running VMs retain their existing snapshots.

## Research and implementation decisions

SeaBIOS [rel-1.16.3 src/serial.c](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/serial.c)
updates equipment-word bits 9–11 for COM and 14–15 for LPT independently.
Ciuki's previous branches hardcoded INT11 or replaced the whole equipment word
in LPT initialization. Both services now update only their field in the VM BDA;
INT11 reads that same word. On the integrated tree the VM BIOS initialises the
word to 822h (integ-periph), the floppy service owns bits 0 and 6–7, and the
UART and LPT services then rewrite only their own fields, so a default game
still reports 822h plus its LPT count. None removes the BDA base and count; a
null UART remains present. Unmapped register reads return FF and writes have no effect.
The same upstream file specifies INT17 write/init/status and the status-bit
inversion already implemented by f1-114. File printers report selected/not-busy
(90h) or timeout (01h), preserve AL/FLAGS, and never expose raw registers.

The owner's Windows 98 Resource Kit, read locally at
`/home/peronslayer/Desktop/CiukiOS/build/rskt/wrkc24_3.htm:704–742`, assigns
communications-resource access to protected-mode VCOMM. Its
`wrkc30_3.htm:19–75` maps HID reports into Vjoyd and game-port interfaces.
Ciuki therefore keeps physical COM/LPT leases in the VMM and reuses cached
native joystick state. This is the Windows 95/98 V86 device model. No physical
I/O permissions or host DMA become guest permissions.

Microsoft's [USB identity explanation](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-faq--introductory-level)
corroborates using USB identity rather than display names or VID/PID alone.
The panel reuses f1-113b's exact controller identity persistence and resolver;
it never rewrites a missing identity merely because a fallback is displayed.
No third-party implementation or Resource Kit text is imported.

## Controls and persistence

The joystick button cycles DOS default, none and connected controllers. The
existing DOS A/B selection can also be copied, preserving two-stick routing.
`CIUKI_JOY_GET_GAME=8` uses the unchanged 100-byte control record: request
axis=game, all other inputs zero. Output attached=0 means default, 1 disabled,
2 resolved saved identity, 3 saved identity missing. The existing route tuple
contains the resolved live choice (or first available fallback); queries do
not queue writes. The panel preserves the saved identity until the user
explicitly edits the joystick choice. Existing SAVE_GAME and SAVE_STATUS
perform identity storage and report durable completion.

Each guest COM1–COM4 selects none, null or a retained physical I/O base.
LPT1 selects none, INT17 printer device, raw physical lease or INT17 file.
Physical selection is by native I/O base, resolved to the current driver index
at creation. Device refresh is a cached query and never acquires a lease.
Current owners, unavailable states and duplicate COM selections are displayed.
The launch acquisition is authoritative; concurrent native/VM leases refuse
launch with EBUSY and unwind through normal VM cleanup. Missing physical COM
falls back to null; missing physical LPT falls back to none. A missing or
unwritable output file disables LPT. CREATE.changed carries PORT_FALLBACK=2
and PORT_SETTINGS_INVALID=4, composable with JOYSTICK_FALLBACK=1. The launcher
shows the resulting note. No missing device is silently rebound to a different
physical port. Live controller hotplug follows the f1-113b snapshot rules.

Ports default to four null UARTs and no LPT for catalog games. Interactive DOS
and existing trusted monitor probes retain their preexisting mapping.
Private `/system/settings/dos-game-N-ports.S.ini` records use version 1,
a sequence, explicitly little-endian fixed-width words, zero-padded path and
an FNV-1a checksum over the complete INI body. Slot S equals sequence & 1.
Load chooses the highest valid complete slot, falling back to the previous
slot or defaults with a visible record. Save commits the inactive slot with
fflush/fsync; sequence exhaustion is refused. Memory/network and joystick
files are untouched. Separate port and joystick stores can partially succeed;
the UI reports this explicitly and offers retry. Storage runs on a copied
Control Panel worker request, outside its event thread.

File output is append-only, under `/Documents/`, with bounded printable paths
and no empty, dot or parent components. It is opened once, with deny-write,
before boot; INT17 writes use that descriptor. Close commits and releases it
only after the VM worker is quiescent, including failed creation. Commit errors
enter the existing cleanup-error path. Existing device-specific limits on
raw timing-sensitive dongles remain in force. No memory limit is increased.

## Host evidence

`tests/host/desktop/test_game_properties.py` exercises the production panel,
persistence helpers and virtual-port launch services. Nine reviewed raster
frames are pinned in `game-properties-goldens.json`. Existing COM, joystick
and LPT harnesses retain their register/lease coverage. The serial lifecycle
fixture also calls the actual INT11 dispatcher, checking a combined 4822h word,
preserved FLAGS/high EAX and count changes when a COM slot is removed/restored.
The LPT fixture independently initializes its count over a BDA containing four
COM ports. The device-inventory test exposes COM, game port and LPT together.
The Control Panel job fixture checks copied worker requests and the actual
port-settings write/read round trip; completion wakes the native event loop.
These are host and target-compile checks, not hardware qualification.
