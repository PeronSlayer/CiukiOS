# Keyboard, language and mouse preferences (G12)

The Windows 98 reference is the owner's Resource Kit, chapter 23,
`wrkc23_2.htm` (Keyboard and Mouse), appendix A `wrkappa_3.htm`
(alternate keyboards), and appendix H `wrkapph_3.htm` (pointer size, colour,
speed, trails and schemes). The extracted book is read-only and is not copied.

Primary online references consulted before implementation:

- [Microsoft SystemParametersInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfoa):
  repeat delay 0..3 means approximately 250..1000 ms; speed 0..31 means
  approximately 2.5..30 Hz. Button swap changes logical button meaning;
  wheel lines may be zero or WHEEL_PAGESCROLL. SPIF_UPDATEINIFILE requests
  persistence; SPIF_SENDCHANGE requests WM_SETTINGCHANGE.
- [Microsoft keyboard input](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input):
  a dead character waits for the following character, combines when possible,
  otherwise emits both; space emits the spacing accent. State belongs to the
  input context and is discarded on focus/layout reset.
- [Microsoft cursors](https://learn.microsoft.com/en-us/windows/win32/menurc/about-cursors):
  schemes replace system cursors; application custom cursors remain theirs.
- [IBM PS/2 controller reference](https://www.ardent-tool.com/docs/pdf/ibm_hitrc07.pdf)
  and [Linux atkbd](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/input/keyboard/atkbd.c):
  device commands use the existing serialized ACK/RESEND transaction, outside
  IRQ exclusion. Typematic is F3 followed by delay/rate, LEDs ED followed by
  Scroll/Num/Caps bits. Never interpret command replies as keystrokes.

Repository comparison: desktop.cfg currently has layout, speed and double
click time only; USB repeats use constants; PS/2 has a bounded command engine
but no preference commands. USER keeps SPI settings in each process.

Implementation contract: extend the existing desktop preferences, retaining
unknown keys and existing defaults. Apply the same PS/2 rate quantization to
USB repeats. SPI speed is linear in frequency between approximately 2.5 and
30 Hz; enumerate the PS/2 periods and select the closest period (400 ms at
speed 0, 33 ms at speed 31). Simply reversing the F3 code would be markedly
nonlinear at intermediate settings. A runtime input devctl is confined to the input-grant owner.
LED state is shared between PS/2 and USB lock transitions. Missing hardware
does not prevent saving preferences; errors are returned without a panic.
Left Alt+Shift switches US/Italian once per chord and updates the tray;
right Alt continues to mean AltGr. Pointer variants are deterministic
transformations of the existing fx-13 rasters, never new artwork. Trail
footprints must be restored by normal scene damage, including scheme changes.

The Italian composition extension uses Shift+the circumflex key as a dead
circumflex; existing accented vowels and AltGr remain available. This is an
explicit G12 extension, rather than a claim that every Italian Windows layout
has an identical dead-key table. Animated cursor import and additional
language layouts are inventory gaps outside the enumerated G12 deliverables.

## Implementation and verification boundary

Desktop protocol operations 80/81 carry a setting request and a bounded
snapshot for both native v2 and Win32 v3 clients. Only the input-grant owner
programs hardware through devctl 0x4905. No syscall number is added. The
desktop owns the current session values, while a separate saved-value mask
keeps SPIF_UPDATEINIFILE=0 changes out of unrelated file saves. Requested
profile writes use the existing durable desktop.cfg writer; its asynchronous
completion is not a synchronous durability acknowledgement to the SPI caller.

G12 bridges keyboard speed/delay, mouse swap/trails/wheel/speed/double-click
time and caret blink. Other SPI groups retain their existing implementations
and belong to their respective feature directives. WM_SETTINGCHANGE is
forwarded on requested changes, and layout snapshots issue
WM_INPUTLANGCHANGE to running Win32 top-level windows.

The existing hardware defaults remain untouched by an ordinary input read;
an explicit policy request initializes F3/ED. Lock transitions schedule LED
programming in the consuming thread, allowing the IRQ worker to deliver ACKs.
A failed device transaction uses the existing quarantine path, since a failed
parameter ACK leaves the keyboard's command state uncertain. USB policy still
accepts the requested values. No reset, panic or unbounded retry is added.

Window-model rows 28 and 29 cover pointer mode changes, trail insertion and
expiry, language switching, global v3 settings, volatile/durable snapshots,
native UTF-8 composition, widget/Run/rename caret blink and wheel lines/page scrolling. The
PS/2 tests check every speed against the documented tolerance, exact F3/ED
bytes, lock repeats, malformed records and a bounded failed transaction.
The USB controller model checks configurable timing on UHCI/OHCI, root/hub.
Control Panel tests cover save/merge/apply and the double-click test. USER
tests cover the input SPI protocol and ToAsciiEx dead-key results.

These are host results. Physical PS/2/USB devices and real desktop operation
remain unqualified; the lead owns that evidence. Leak detection stays enabled
in scripts that enable it: their assertion-complete runs cannot exit cleanly
in the current ptrace-restricted sandbox because LeakSanitizer requires ptrace.
