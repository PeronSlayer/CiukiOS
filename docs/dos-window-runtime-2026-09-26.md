# Foreground BIOS-text DOS window

Status: implemented and qualified in QEMU on 2026-09-26 for the packaged
COMMAND.COM and the BIOS-text COM/MZ fixtures described below. This does not
establish compatibility with every DOS application or physical laptops.

This document preserves the **initial BIOS-text implementation and ABI 1
qualification record**. The selected native-desktop build now packages ABI 3
with a 256-byte header and a 31,536-byte DOSWIN.DRV, plus a separate 64,000-byte
cooperative graphics buffer. It also hooks INT15/INT2F and services blocked
keyboard waits more frequently. Use `src/com/dos_window_abi.inc` for the current
interface and [the cooperative graphics record](windowed-games-2026-09-26.md)
for those extensions. Historical sizes, four-vector checks and commands below
refer to their named earlier artifacts, not the current binary.

The [selected desktop build](native-desktop-2026-09-26.md), image SHA-256
`08bc6df6510e55e3f501d49f9414e2d0f0a2006eccdda5dbf8f78bdc1e244bd6`,
passes eleven focused integration reports. Its original DOS graphics, DPMI and
audio-device virtualization is still unimplemented; silent source-port previews
are a separate compatibility tier, not evidence that original games run in
windows.

## Supported execution model

One real-mode child runs through the existing synchronous `INT 21h AX=4B00h` service. Its BIOS text calls update an 80×25 character/attribute surface owned by an external module. The normal shell loop is suspended, but an IRQ0 callback presents that live surface and services a restricted set of retained-window mouse actions. The child has not exited while those updates occur.

The intended first client is the project's BIOS-text command interpreter and explicitly qualified BIOS/DOS text utilities. File operations still execute serially in the foreground child. The host callback never calls DOS, BIOS, EXEC, allocation or ordinary shell command dispatch. Closing the window records a sticky cooperative request and queues Escape for the foreground child. The owned command interpreter can query that request and exit normally when its prompt regains control; its opt-in `/W` integration is separate from ordinary command-interpreter execution. The runtime does not destroy a running process or release its memory asynchronously.

This is a shared-address-space compatibility feature, not a virtual machine, scheduler or protection boundary. The ordinary fullscreen launch path remains necessary for applications outside this contract.

## Using the window

- The desktop's **DOS** icon/button opens the packaged `COMMAND.COM` in a
  native window. `DIR`, command-line editing and BIOS-text child execution use
  the actual DOS kernel.
- **Run → Window** executes the entered command through `COMMAND.COM /W /C`
  and retains its output after normal return. The existing **Run** button
  continues to use fullscreen execution.
- **F4** opens the ordinary fullscreen DOS console. Use this path for direct
  video applications, games, DOS extenders and Windows.
- `EXIT` terminates the window's command interpreter normally. Its final
  output remains until the window closes. The window's X requests normal exit;
  an application must cooperate with Escape or exit through its own command.
- While the child runs, retained windows can be moved, focused, minimized and
  restored with the mouse. Other DOS/file/settings launches are deferred until
  the foreground child returns. There is one DOS session, not multiple
  independently scheduled DOS processes.

Windowed presentation requires an active LFB compositor at least 800×600.
Banked/recovery graphics continue to provide the fullscreen DOS path.

## Source ownership and lifecycle

| Source | Responsibility |
| --- | --- |
| `src/com/dos_window_abi.inc` | Versioned module header, entry operations, callback packet and error bits. |
| `src/com/dos_window_runtime.asm` | Raw org-0 module; private cells, row damage, keyboard routing, text BIOS emulation, IRQ0/private stack and reversible interrupt hooks. |
| `src/com/shell_dos_window.inc` | Foreground allocation/loading, header initialization, expanded-font loading, selected child launch and cleanup. |
| `src/com/shell_dos_window_host.inc` | Restricted retained-window presentation and mouse control. No DOS/BIOS calls in the callback. |

Foreground launch checks that the native LFB compositor is available. It loads `SYSTEM\DOSWIN.DRV` into a DOS-owned allocation, checks the header, supplies the shell callback/context and screen dimensions, and populates the reserved expanded font. Runtime installation saves INT10, INT16, INT08 and INT33, disables the physical mouse callback while retaining its original mask/address, and installs its own handlers. It does not reset the physical mouse or change the physical VBE mode.

The shell sets `DW_GUEST_LIVE=1` immediately around raw kernel EXEC. After the child returns, it clears that flag, uninstalls the module, restores the original vectors and mouse callback, and finishes host presentation. A normal return retains the unhooked text surface until the output window closes, another windowed command replaces it, or fullscreen execution starts. A cooperative close releases it immediately after normal termination. The allocation belongs to the parent shell, so child termination must not free it. The runtime accepts an arbitrary caller DS at its FAR entry and establishes its own DS internally.

Failure before installation must unwind the loader's allocation without dispatching a child. A normal close must allow the child to terminate itself. Force-kill, TSR retention and nested background sessions are not supported lifecycle operations.

## Interrupt and presentation contract

IRQ0 first chains the saved BIOS handler, including its tick accounting and EOI. It saves the complete 32-bit general registers, DS/ES/FS/GS and EFLAGS around both the firmware chain and the host callback. Before presentation it checks installation, active foreground child, host busy/unsafe state, virtual text-update depth, CR0.PE and the interrupted CS. Firmware addresses at or above A000h and protected-mode execution are skipped.

The callback runs on the module's private 8192-byte stack. `host_busy` is set before switching stacks and enabling interrupts. Nested ticks still reach the BIOS but cannot reenter the presenter or overwrite its saved SS:SP. The presenter must use retained memory and the LFB only. These gates do not virtualize arbitrary firmware or third-party resident drivers; supported binaries must obey the foreground real-mode contract.

There is deliberately no blanket InDOS gate: DOS blocking keyboard services can wait with InDOS set, and a pure memory presenter must remain live while they wait. Neither the IRQ handler nor the callback reenters DOS.

Rows carry a 25-bit dirty mask. Cursor movement marks the old and new row; ordinary character output marks affected rows; scrolling marks its rectangle. The host must consume a snapshot of damage without discarding updates arriving during nested interrupt activity. Display rendering must clip to the DOS client's window, not expose the entire surface outside its bounds.

The host atomically exchanges the mask with zero before painting and retains
new damage raised by nested interrupt activity. It bounds text repainting to
the first and last changed rows. The visible framebuffer page remains selected
during the session; normal page flipping resumes with a complete hidden-page
invalidation after the child returns.

The private stack makes default-segment assumptions observable: a BP-based
global-data reference defaults to SS, not DS. Actual execution exposed this in
the taskbar and the same pattern was corrected in Tasks rendering. The fix
uses explicit DS for those globals; genuine BP-relative stack locals remain
SS-relative. The failed minimize/restore test is retained in
`build/full/dos-window-2026-09-26/window-acceptance-3/`.

## ABI version 1

The module starts with a near entry jump, padding to offset 4, and the eight-byte magic `CWRT0001`. Its fixed header is 128 bytes. All offsets below are relative to the module segment; words and dwords are little-endian. Use the assembly include as the authoritative interface.

| Offset | Field |
| --- | --- |
| 12 / 14 / 16 | Version word / header-size word / complete image-size word. |
| 18 / 20 | Cell-buffer offset word / dirty-dword offset word. |
| 24 / 26 / 28 | Host callback offset / segment / host DS. |
| 30 / 32 | Optional InDOS pointer, diagnostic only. |
| 34–37 | Installed, busy, keyboard focus and foreground-child-live bytes. |
| 38 / 40 / 42 / 43 | Logical cursor DX, cursor shape CX, mode byte, page byte. |
| 44 / 46 | Sticky errors / last unsupported graphics request. |
| 48 / 52 | IRQ observation count / completed callback count, dwords. |
| 56 | Offset of 8192 bytes reserved for the host's expanded font. |
| 58 / 60 | Host screen width / height. |
| 62 / 64 / 66 | Host pointer X / Y / button word. |
| 68–73 | Host unsafe, pending close, keyboard wait, logical columns, rows and virtual-video depth bytes. |
| 74 | Last host callback result word. |
| 76 | Sticky cooperative-close request byte; reset by installation. |

FAR entry operations in AX: 0 install, 1 uninstall, 2 clear cells, 3 pop physical BIOS-ring key, 4 peek key. Lifecycle success returns AX=0/CF=0. Key operations return the BIOS scan/ASCII word in AX; CF=1 means no available key. Key operations protect ring updates with interrupts disabled.

Host callback input: DS=host shell, ES=module, AX bit 0=text dirty, BX=buttons, CX=X and DX=Y. Return AX bit 0=guest keyboard focus, bit 1=request normal close, bit 2=host repaint still pending. The runtime restores the interrupted CPU state even if the callback changes general and segment registers.

The optional header InDOS pointer is not a synchronization primitive. The host must set the unsafe flag before a foreground mutation that its retained callback cannot observe coherently.

An exact private INT10 query, AX=FF43h and BX=5744h, returns AX=4357h and DX bit0 indicating the sticky close request. All other registers are preserved. Other argument combinations remain unsupported video calls. This query does not terminate a child; the opted-in interpreter must call normal DOS termination itself. Pending Escape is retried only while the guest has keyboard focus and the BIOS queue has room. Physical keys collected while another retained host window is focused are discarded, so they cannot be replayed into DOS after refocusing.

## Implemented BIOS services and limits

| Interface | Implemented behavior |
| --- | --- |
| INT10 AH00 | Logical text modes 0–3 only, including no-clear bit. Unsupported graphics requests set an error without touching physical video. |
| INT10 AH01/02/03/05 | Cursor shape, set/get position, page 0 selection only. |
| INT10 AH06/07 | Clipped inclusive text-rectangle scroll or clear. |
| INT10 AH08/09/0A/0E | Character/attribute read and writes, bounded repetition, teletype controls, wrap and scroll. Bell is silent. |
| INT10 AH0F/13 | Logical mode/columns query; BIOS string writes with optional attributes/cursor update. |
| INT10 AX1130 | Saved real BIOS 8×16 font pointer. The host's expanded font is not presented as a BIOS font. |
| INT10 AX1114/1003 | Logical compatibility no-ops; no physical font, palette or blink change. |
| INT10 AH4F | Returns 014F and records unsupported graphics use. |
| INT16 AH00/10,01/11 | Blocking read and status from the validated BIOS keyboard ring, gated by guest focus. Blocking wait admits interrupts. Other services chain the original BIOS handler. |
| INT33 | Physical driver is reserved for the host. Guest reset reports absent and polling is nonblocking/empty; callback registration cannot modify the physical driver. |

Only page 0 is represented. Modes 0/1 report 40 logical columns using the same 80-cell storage stride. The physical video BDA fields are not falsified. Direct B800/A000 writes, direct video ports, raw controller access, graphics modes, DPMI/VCPI, protected mode, guest mouse applications and TSRs are **outside this contract**. A child that disables interrupts can also stop host servicing; this runtime is not a monitor that can trap CLI.

The runtime cannot prevent an arbitrary program from directly changing physical video or corrupting shared DOS memory. Those applications require the monitored tier in [the architecture document](dos-window-architecture-2026-09-26.md) or fullscreen execution. Successful text execution cannot establish Doom, Windows 3.1 or Windows 95 windowed compatibility.

## Memory and build record

The cooperative-close/focus candidate at `build/full/dos-window-runtime-2026-09-26/doswin-close.bin` is 22,992 bytes, SHA-256 `b8c2d67f72500dfbe8edf4a2a5658ceba6fc739d5e7f4b78e4cfe8ca126a3394`. It includes 4000 bytes of cells, 8192 bytes of expanded font and 8192 bytes of private stack; code/header/state/alignment use the remaining 2608 bytes. The DOS allocation rounds this image to paragraphs and adds the allocator's own metadata. It does not create a full-screen pixel backbuffer; the existing compositor retains that ownership.

The earlier first-run candidate remains immutable at `doswin.bin`, 22,896 bytes, SHA-256 `29e9829420430522dcebe39611ba062c5fe2808e624e0b4f51aeb205bd998c39`. Evidence from that candidate does not qualify the later cooperative-close behavior.

This footprint is additional conventional memory while the child runs. The parent shell, its external assets, the child PSP/environment/code/stack and kernel/resident drivers still count. Extended-memory availability of 128 MiB does not remove the conventional-memory limit. The external module avoids enlarging SHELL.COM beyond its 64 KiB segment; it does not erase its own memory cost.

Compile command:

```sh
nasm -f bin src/com/dos_window_runtime.asm \
  -o build/full/dos-window-runtime-2026-09-26/doswin-close.bin \
  -l build/full/dos-window-runtime-2026-09-26/doswin-close.lst
```

## Qualification record

The final packaged run passed with Pentium III/128 MiB configured in QEMU/KVM,
at 1280×800×32. Production binaries were not replaced, guest RAM was not
written, and the source image remained unchanged. This is not a physical
Pentium III performance measurement or an E500/T23 hardware qualification.

| Artifact | SHA-256 |
| --- | --- |
| `ciukios-dos-window.img` (134,217,728 bytes) | `4404a839df7e2556d1ddc117f22d16d96e4863535beff526ef320cebc2c08d4d` |
| Packaged `SHELL.COM` (60,464 bytes) | `9522d1814fa00ef02a4a7eb2661343a8dbee5d6a2c0d7d9fc14d5907f1ab390d` |
| Packaged `COMMAND.COM` (14,752 bytes) | `8161c1a97036fda0e2dabfe22b861828ebac418299932e05c43c4756d1422eed` |
| Packaged `DOSWIN.DRV` (22,992 bytes) | `b8c2d67f72500dfbe8edf4a2a5658ceba6fc739d5e7f4b78e4cfe8ca126a3394` |
| Unchanged kernel (43,169 bytes) | `116276101b269c46d7390eb9bbc6b5dd1da90a371a222ebe22f6ae65c1ce9ea3` |

Actual observations:

- Shipped COMMAND.COM executes DIR and ECHO in its native window; EXIT and
  the cooperative X close terminate normally.
- `DWBIOST.COM` runs for 728 BIOS ticks with 345 updates and 39 live serial
  samples before its programmed time limit. `DWBIOST.EXE` runs for 369 ticks
  with 166 updates and 19 samples, then exits normally on Escape.
- Each fixture retains the same parent PSP across dragging an About window,
  focus changes, keyboard input and DOS-window minimize/taskbar restore.
  Each also executes a distinct child PSP and checks file bytes before and
  midway through its run. Its scratch file is absent afterward.
- Ten actual captures compare 29,440 pixels to the firmware font, cell
  attributes and cursor, including changing counters and scrolled rows.
  The old and new About-window positions are checked during guest execution.
- Five exact comparisons verify restoration of INT08/10/16/33. A subsequent
  fullscreen child runs, and native keyboard/mouse input works after return.
- The final image also passes the existing Files/Tasks runtime lane:
  independent copied-file/FAT checks, directory operations, three fullscreen
  COMDEMO returns and native window controls. This covers the relocated hit
  table and mouse-save storage used to retain the original COM size guard.

Reports and real screenshots are under
`build/full/dos-window-2026-09-26/final-window-acceptance-stable/` and
`final-native-utilities/`. The renderer's taskbar segment bug is retained in
the earlier negative test. Other retained harness failures distinguish an
omitted cursor in the pixel oracle and asynchronous observation of a hit
table during repaint; the final oracle includes the caret and waits for a
stable idle table. No guest state was changed to satisfy those checks.

```sh
python3 scripts/qemu_test_dos_window.py \
  --image build/full/dos-window-2026-09-26/ciukios-dos-window.img \
  --listing build/full/dos-window-2026-09-26/final-shell.lst \
  --output build/full/dos-window-2026-09-26/final-window-acceptance-stable
```

The full build used `CIUKIOS_INCLUDE_DOS_WINDOW_PROBES=1` to package the three
BIOS-only acceptance executables. The separate direct-B800 fixtures remain
future monitor gates and are not represented as working windowed programs.
Owner-supplied wallpapers and existing personal application payloads retain
their existing distribution restrictions; this is a development HDD image.
