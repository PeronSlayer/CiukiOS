# Native DOS windows: architecture and implementation boundary

Status, 2026-09-26: a restricted foreground **BIOS-text runtime is implemented
and qualified in QEMU for the packaged COMMAND.COM and BIOS-text COM/MZ
fixtures**. Later work added a cooperative graphics API for Doom/Wolf source
ports. Neither tier implements the requested execution of original DOS graphics
binaries with audio. It is not a V86 monitor and does not isolate direct video
memory, video ports, arbitrary DPMI or background DOS processes. See
[the text contract and its historical ABI record](dos-window-runtime-2026-09-26.md)
and [the current desktop evidence](native-desktop-2026-09-26.md).
Existing fullscreen execution remains the compatibility path for applications
outside the restricted window contract; physical T23/E500 qualification remains
open.

## Decision

A Pentium III can execute ordinary real-mode DOS code in virtual-8086 mode at native instruction speed. A protected-mode monitor can intercept selected I/O and map a private text or graphics surface into that application's address space. This is the appropriate long-term route for CiukiOS; an incomplete 8086 interpreter would not meet the requirements for Doom, DOS extenders and protected-mode software.

Capturing a text screen after a child exits does not implement a running DOS window. The implemented restricted tier hooks BIOS text calls and updates a private live surface; IRQ0 services retained host windows while that child is still running. Its supported-program contract excludes physical video access. General DOS compatibility additionally requires a monitor that can stop direct memory/port access from changing the physical display behind the compositor.

There is no small isolated module providing general DOS virtualization with the present kernel. Jemm supplies useful monitor infrastructure, and HDPMI supplies protected-mode client services, but neither supplies a ready-to-use CiukiOS window manager or an independent DOS virtual machine. The implemented first tier is **one running foreground BIOS-text session in a retained graphical desktop**, with an explicit supported-program boundary. Graphics, direct B800 text, DPMI and multiple independently running DOS sessions are later compatibility gates, not implied capabilities of that tier.

## What exists today

Source pointers use labels because the shared assembly files continue to change:

| Component | Current behavior | Consequence |
| --- | --- | --- |
| `src/com/shell.asm`, `exec_run_candidate` | Ends the graphics console, then calls `INT 21h AX=4B00h` synchronously. | A child owns the machine until it returns. |
| `src/com/shell_gui.inc`, `ui_loop` | Polls input, file operations, window state and painting in the shell's main loop. | The native event loop is suspended during `EXEC`. |
| `shell_gui.inc`, `ui_queue_command`, `ui_command_return` | Leaves graphics before dispatch; restores the desktop after command completion. | Returning to the same windows is session restoration, not concurrent DOS execution. |
| `src/com/vbe_console.inc`, `vc_capture_text` | Imports a text screen into the graphical console. | A captured screen is not a live virtual display. |
| `src/boot/floppy_stage1.asm`, `int21_exec`, `int21_exec_run_com` | Saves nested parent state, switches PSP/segments/stack, and jumps to the child. Implements execute and overlay subfunctions. | Nested execution is not a scheduler; the existing four saved contexts are not four runnable tasks. |
| Kernel `dos_sda`, global FAT/open-handle/allocator state | Exposes a small DOS state structure and saves selected state around nested execution. | This is not a complete process/VM image. Arbitrary interleaving of DOS file operations is unsafe. |
| `src/com/dos_window_runtime.asm`, `runtime_entry`, `timer_handler`, `video_handler`, `keyboard_handler` | Installs reversible BIOS-text/input hooks and an IRQ0 presenter around one foreground child. Owns a private text surface and 8 KiB interrupt stack; blocked keyboard waits also service the host. | Qualified text fixtures repaint, drag, minimize, restore, execute children and return normally in QEMU. It does not trap arbitrary video or privileged access. |
| `src/com/shell_dos_window.inc`, `dw_launch`; `shell_dos_window_host.inc`, `dos_host_tick` | Loads the external runtime, starts the child through raw kernel EXEC, and provides a restricted memory/LFB-only presenter. | It bypasses the ordinary fullscreen teardown only for the explicitly selected text session. Host actions that require DOS remain deferred. |
| `src/com/vbe_modes.inc`, `vc_validate_layout` | Allows the current LFB backend only when `CR0.PE` is clear. | Enabling a V86 monitor changes renderer availability. |
| `src/com/vbe_fb.inc`, `vc_fb_begin`, `vc_fb_limits` | Briefly changes CR0/GDTR/segment limits for real-mode flat framebuffer transfers. | This cannot simply execute inside a V86 guest. A monitor-owned protected-mode renderer is needed. |

The current kernel also has global physical-volume state rather than independent physical-volume binding for every open file handle. Windowing does not solve that storage limitation. Kernel services must remain serialized, and the file manager's current cross-drive restrictions remain relevant.

## Reusable upstream components

The investigation used primary source, not feature names inferred from API resemblance. Research checkouts are not included as new runtime payloads.

### Jemm and JLOAD

Inspected commit: `e96bb6bb80cbdc3e6585544db79a6b133a5566dd` of [Jemm](https://github.com/Baron-von-Riedesel/Jemm). Local research checkout: `build/full/dos-window-research-2026-09-26/Jemm`.

Useful pieces:

- `src/JEMM32.ASM`, `src/EMU.ASM` and `src/VCPI.ASM`: V86 monitor, privileged-instruction handling and VCPI services.
- `Include/JLM.H` and `Include/JLM.INC`: protected-mode extension interface, V86 interrupt/fault hooks, client register frames, page mapping and I/O handlers.
- `JLM/IOTRAP/IOTRAP.ASM`: a small actual port-trapping extension. This is a suitable starting point for a video-register trap module, not an existing VGA emulator.
- `JLM/HELLO2/HELLO2W.C`: protected-mode C extension and nested DOS-call example. Its nested-execution API is not permission to call DOS recursively from an arbitrary interrupt.
- `Tools/JLOAD/VMM.ASM`: a compatibility subset of VMM-style services useful for extension development.

Three limits are visible in the implementation:

1. `Get_Cur_VM_Handle` returns one static `vmcb`. It does not enumerate or create independent VMs.
2. `_LinMapIntoV86` changes mappings in the current page tables; its VM argument does not select a separate guest address space.
3. `src/JEMM32.ASM:Yield` briefly admits pending interrupts. It is not a runnable-task scheduler. Similarly, `Save_Client_State` saves a CPU client frame, not the DOS kernel, BIOS, files and peripheral state.

Jemm is therefore the leading candidate for **monitor reuse**, but a session broker and host rendering service still have to be built. Initialization compatibility with CiukiOS's DOS services, XMS implementation, memory map and resident audio components must be established before enabling it by default.

Licensing is component-specific. Upstream describes JEMM386/JEMMEX as partly under its included Artistic License. `Tools/JLOAD/license.txt` is MIT. The GENERIC, HELLO, HELLO2, IOTRAP, JCLOCK and QPIEMU samples are explicitly Public Domain; some unrelated driver modules are GPLv2. Preserve each component's notices and ship corresponding source and modification records for any modified monitor. Do not label the entire package GPL or Public Domain. See the pinned [license declaration](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Readme.txt), [Artistic terms](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Artistic.txt), [JLOAD terms](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Tools/JLOAD/license.txt) and [sample declarations](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/JLM/README.TXT).

### HDPMI

Inspected existing checkout: `build/external/HX`, commit `43182b4a0efd008ae5aab3ccfcc6d644437cd111`, [upstream HX/HDPMI](https://github.com/Baron-von-Riedesel/HX).

Relevant files are `Src/HDPMI/CLIENTS.ASM`, `SWITCH.ASM`, `I31SWT.ASM`, `INT21API.ASM`, `INT31API.ASM`, `HDPMIAPI.TXT` and `HDPMI.TXT`. These implement protected-mode clients, DOS/BIOS translation, context changes and a vendor API. The IOPL0 variants already relevant to CiukiOS audio expose I/O trapping and IRQ simulation.

The upstream documentation explicitly excludes virtual machines. Separate address contexts still share conventional memory through `10FFFFh`, DOS files and IVT vectors. Nested client contexts do not provide independent task scheduling. The public vendor API has no create-independent-DOS-VM operation. These are decisive limits for treating HDPMI as a complete desktop DOS-box engine. See [HDPMI documentation, sections 1 and 6.12–6.13](https://github.com/Baron-von-Riedesel/HX/blob/43182b4a0efd008ae5aab3ccfcc6d644437cd111/Src/HDPMI/HDPMI.TXT) and [vendor API](https://github.com/Baron-von-Riedesel/HX/blob/43182b4a0efd008ae5aab3ccfcc6d644437cd111/Src/HDPMI/HDPMIAPI.TXT).

Its useful role is the DPMI service/translation layer for a later integrated host. It must coordinate with the selected monitor and video/audio traps; installing another protected-mode owner alongside it without that contract is not a solution. Upstream describes HDPMI as freeware and provides source, but this is not a blanket GPL declaration. Preserve existing distribution terms and establish the terms for a modified core before shipping a new fork; source availability alone does not justify assigning it a new license.

### CPU emulators

An 8086-only emulator such as [emu2](https://github.com/dmsc/emu2) or [8086tiny](https://github.com/adriancable/8086tiny) cannot execute a 386 protected-mode DOS extender. A full PC emulator would need a substantially larger device and CPU integration plus measured performance on the target Pentium III. Neither is a shortcut to the requested native-speed games. No partial CPU interpreter was added to the project.

The hardware execution model comes from Intel's virtual-8086 architecture: a protected-mode monitor supervises a V86 task and mediates privileged operations. Protected-mode DPMI applications require additional handling beyond that V86 task. Pentium III does not require VT-x for this design. See [Intel SDM Volume 3B, virtual-8086 mode](https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf).

## Next tier: monitored text and direct video memory

The current IRQ/BIOS-text implementation does not trap direct B800 writes, video ports, privileged execution or a guest that disables interrupts. Extending it to those applications requires a monitored profile. That next profile should still keep **one DOS execution context** and provide a protected-mode host presentation service. It should not attempt independent background filesystem workloads or multiple DOS sessions yet.

1. A monitor starts before launching the session and owns the physical display, interrupt virtualization and protected-mode transitions. Initialization is explicit and reversible on failure; the existing native boot profile stays available.
2. The DOS child still uses the existing synchronous `EXEC` and kernel file services. While it runs, a monitor-side service updates the retained desktop and handles the session window. That service must not reenter the suspended shell's main loop or call DOS in an interrupt. Native Files operations and other DOS-dependent actions are deferred until the child returns.
3. The host presentation service has its own protected-mode stack, code/data and framebuffer mapping. It paints from retained, explicitly shared window data. The real-mode LFB burst implementation is not used from V86. The long-term option is moving more of the compositor into this service, rather than repeatedly switching the whole shell between execution models.
4. The child sees a RAM-backed `B8000h` text surface and virtual cursor/mode state. INT 10h text services and CRTC I/O operate on that state. Intercepting INT 10h alone is insufficient because many DOS applications write text memory and video ports directly.
5. Hardware keyboard/mouse input remains owned by the host. Only events routed to the focused DOS window enter the guest queue; implement INT 16h/BDA semantics and a clearly bounded raw keyboard-controller policy. BIOS routines expecting physical video or controller state cannot be blindly passed through.
6. A timer/service boundary gives the host a bounded opportunity to process input and damage without invoking DOS. Guest virtual IF and PIC state must not let a guest CLI permanently disable the host's own scheduling. Preserve general registers, segment state, FPU state and nested interrupt ownership at each transition.
7. A normal guest exit unwinds the original `EXEC`; the host releases its surface only after all outstanding callbacks and DMA users have stopped. A window close initially requests a normal application exit. Arbitrary force-kill cannot be implemented by overwriting a saved PSP/return address; safe cancellation requires explicit ownership of every resource it would release.

This design is a **shared DOS session**, not a security boundary. A guest still shares the kernel's low-memory context, so arbitrary bad DOS pointers can damage that context. Independent sessions require separate low-memory/IVT/BDA mappings and a well-defined DOS broker or private kernel instances, together with file/allocator/device ownership. Merely copying 1 MiB of RAM is insufficient because interrupt vectors, physical DMA addresses, absolute kernel pointers, resident services and live handles must remain coherent.

The monitored tier's acceptance set should include a long-running COM program that writes B800 directly, a BIOS-text MZ program, keyboard focus changes, normal termination and a file-reading application. Direct B800 is deliberately excluded from the current restricted tier. For either tier, during the same uninterrupted supported guest execution, move another host window over it, expose it again, and demonstrate that host input and repainting continue. Those observations distinguish real execution from a screenshot shown after termination.

## Graphics and protected-mode acceptance gates

Graphics programs require at least virtual mode 13h and relevant planar VGA modes, A000 mapping, palette/register emulation, and coherent timing/status reads. Guest pixel changes must be converted into damage on the host surface. A program may access VGA registers without using any BIOS function.

Doom and similar extenders also require a tested DPMI/VCPI contract: protected-mode client mappings, real-mode callbacks, BIOS translations and IOPL0 I/O routing must remain attached to the same virtual display. A DPMI client's physical-memory mapping request must not expose the host framebuffer or overwrite the virtual VGA policy. Audio's VSBHDA/HDPMI traps must use the same ownership chain; two unrelated handlers replacing the same port or IRQ path are not safe integration.

Windows 3.1 is a separate gate. Its mode changes, DPMI usage, possible enhanced-mode monitor, display driver and audio ownership cannot be inferred from successful COM or Doom execution. Keep the current fullscreen Windows path until the selected Windows mode is explicitly qualified within the host. Windows 95 is not a capability conferred by this proposal.

No acceptance statement should say “all DOS applications.” Record actual binaries, versions, execution modes, hardware configurations, failures and fallbacks. A program with unimplemented device access should receive an explicit fullscreen option before launch, not an unresponsive decorative window.

## Memory and CPU constraints

The minimum target remains Pentium III with 128 MiB. The main advantage of V86 here is native execution of ordinary guest instructions; port traps, repainting and mode transitions still have a cost that must be measured on that hardware.

At 2560×1440, an 8-bit full-size surface needs 3.52 MiB and a 32-bit surface needs 14.06 MiB; two such surfaces need 7.03 or 28.13 MiB respectively. These are arithmetic storage requirements, **not measured runtime usage or a claim that an ATI 9200 exposes every desired VBE mode**. Use dirty rectangles and avoid copying a complete 2K surface for every text cell or input event.

Jemm's documented monitor footprint is small, but its default DMA/EMS pools, the guest's extended memory and the host's buffers also count. The live-CD RAM disk consumes RAM independently of an installed HDD configuration. Reserve from the actual available-memory map and existing allocators; fail session creation cleanly if its declared budget is unavailable. A 128 MiB installed system and a 128 MiB live system must not be treated as having identical spare memory.

## Three bounded implementation work packages

These are the remaining work packages for general DOS graphics/direct-video/DPMI support beyond the implemented restricted BIOS-text tier. They can begin in parallel once their monitor ABI is frozen. Their final integration is sequential: video and DPMI qualification depend on the monitor/session foundation. No runtime claim is valid until that integration runs on the actual binary being released.

### 1. Monitor and DOS session ownership

Prompt: “Implement an opt-in native-speed V86 session host for CiukiOS using the pinned Jemm/JLOAD sources where appropriate. Preserve existing native boot and synchronous EXEC. Define a versioned session ABI with lifecycle, client frame, virtual interrupt state, focus/input queue, text/video surface descriptors, ownership and error reporting. Establish bounded host servicing without reentering DOS or the suspended shell. Start with one foreground real-mode text session. Audit XMS/VCPI, BIOS, allocator, IVT/BDA and TSR interactions. Do not advertise independent VMs or DPMI support until implemented. Deliver modified sources/licenses, exact integration hooks and a real child-execution demonstration.”

### 2. Host renderer and virtual display/input

Prompt: “Implement the protected-mode host presentation backend for the agreed session ABI. Do not execute the existing CR0-toggling real-mode LFB routine under V86. Map and own the physical framebuffer, render retained desktop damage, virtualize B800 text memory, INT 10h text calls, CRTC/cursor state and focused INT 16h/BDA keyboard input. Keep host input responsive during a running guest. Add VGA graphics only as explicit later capabilities. Demonstrate overlapping host windows and guest updates without tearing outside their bounds, including 800×600 and supported high-resolution hardware. Use actual input, not RAM-written events.”

### 3. DPMI/device integration and compatibility

Prompt: “Integrate the agreed session host with the existing HDPMI/VSBHDA launch paths without competing protected-mode or IRQ owners. Audit source/distribution terms. Implement and qualify protected-mode video mappings, I/O traps, callbacks, audio ownership and termination cleanup. Start with actual Doom/DOS extender binaries and preserve fullscreen fallback. Treat Windows 3.1 modes as a separate compatibility gate. Provide real binary/version/hardware results and failures; do not substitute a text screen capture for a running DOS session or infer universal compatibility from one program.”

## Work performed for this investigation

The initial investigation inspected EXEC, UI dispatch, DOS state and LFB transfer code; downloaded and pinned Jemm source; inspected existing HX source and upstream documentation; and reviewed the relevant licenses. It made no production changes. The subsequent implementation added the separate BIOS-text runtime and shell integration described above. It does not import Jemm or HDPMI as a new session monitor, and no CPU emulator was introduced. Integrated runtime tests are pending at the time of this document update; compilation alone is not an execution result.
