# DOS virtualization foundation — 26 September 2026

This work adds executable foundations for running **original DOS binaries**:
a real V86 monitor, private VGA memory, protected framebuffer transport and
a protected-mode VGA-port adapter. The 27 September extension additionally
executes the unmodified packaged doom-vanille DOS engine,
`APPS/DOOMVAN/PCDMCORE.EXE`, under the real Jemm/HDPMI stack. That executable is
neither the proprietary original Doom binary nor the cooperative CiukiOS window
port. It
does **not** yet put arbitrary DOS
graphics or audio into a native desktop window. The normal desktop launch
paths and boot configuration do not load the experimental monitor.

The desktop baseline was published as `c36be36`. Its selected image has SHA-256
`08bc6df6510e55e3f501d49f9414e2d0f0a2006eccdda5dbf8f78bdc1e244bd6`.
Each new emulator run uses a private copy with the new kernel and the named
experimental components. The baseline image remains unchanged. This record
supersedes neither its [eleven desktop checks](validation/2026-09-26/README.md)
nor the outstanding physical T23/E500 reports.

The [27 September integration audit](vm-integration-audit-2026-09-27.md)
distinguishes the artifacts from each track. Its initial combined PASS predates
the scheduler reentry correction and ongoing guest-IF fixes. Final runtime
qualification of those corrections is pending; earlier tick observations do
not establish that every guest CLI/POPF sequence preserves interrupt semantics.

## Kernel defects fixed during integration

`INT 21h/AH=52h` previously cleared the 1,792-byte DOS SYSVARS/CDS/SFT image
on **every** query. A resident driver could register successfully and then
disappear from the device chain when another component queried it. The
initializer now runs once; subsequent calls publish the live structures and
refresh the MCB anchor. The test executes the assembled kernel, links a driver,
changes live tables and verifies those bytes survive repeated calls. The old
kernel reproduces the destructive behavior.

The XMS provider advertised version 3.0 but lacked functions `88h` and `89h`.
Jemm therefore failed its memory allocation. The kernel now reports the
existing managed pool through the 32-bit query and accepts representable
allocation sizes through the existing allocator. Oversized requests fail
without truncation or allocation. The actual assembled routines are checked
for register widths, failure atomicity, handle exhaustion and legacy behavior.
This does not enlarge the native XMS pool or claim all physical RAM is managed.

The full-disk kernel is **43,217 bytes**, SHA-256
`2f53b6e63ccaa04f496b54c29f7e74d3628b1316245e180f21149269173380af`.
Only **47 bytes** remain before its existing `A900h` limit. New VM work belongs
in separately loaded modules; raising the limit without auditing the resident
memory map would be unsafe.

The patched Jemm profile also fixes an upstream emulated A20-port read that
used AH as scratch, although guest `IN AL` must preserve all bits above AL.
CiukiOS's XMS entry checks A20 before dispatch; that register corruption changed
the requested function number and produced false memory-query results. The
fix is kept in a separate, hash-recorded upstream patch. The kernel does not
special-case Jemm to conceal the problem.

## Implemented components and boundaries

| Component | Implemented | Boundary |
| --- | --- | --- |
| Pinned Jemm/JLOAD build | Verified sources and tools, isolated build directories, DOS device-query adapter, physical-IRQ0 host-scheduler service, runtime load/unload | One foreground V86 context; callbacks are services, not independent VMs |
| `CVSESSION.DLL` | Owns one Jemm CR3, shadows all 32 pages at A0000–BFFFF, owns VGA port/fault traps, logical BIOS modes, planar VGA state and bounded presentation/readback | Experimental foreground session; the desktop DOS window uses it only when Jemm and the JLM are loaded manually ([video record](vm-video-session-2026-09-27.md)) |
| Protected framebuffer transport | Validated physical framebuffer mapping, uncached protected-mode read/write, at most 4,096 bytes per call, explicit unbind | Not wired into the native UI compositor; trusted host API, not a security boundary |
| Freestanding VGA model | Four planes, latches, read/write modes, chain-4, odd/even, indexed registers, DAC, text/13h/Mode X/planar scanout, virtual video BIOS | Connected to trapped V86 and protected-mode memory and port cycles since 27 September |
| HDPMI session adapter | Official 3.24 API-6/API-7 callbacks, actual protected IN/OUT and VGA-memory instructions, separate per-client PTE snapshot, fault/mode-transition bridge, exact unwind | One owning 32-bit foreground client; no older-fork ABI and no background-VM claim |
| Peripheral groundwork | Exclusive video-port ownership, read-only host observations, DOS file I/O preserved, defined DPMI ownership contract | No virtual keyboard/mouse/PIC/PIT/DMA/Sound Blaster yet |

The ordinary `VMGUEST.COM` fixture uses only standard DOS, BIOS, VGA memory and
port operations. It contains no CiukiOS cooperative game API. `VMPARENT.COM`
is the host-side owner and validation driver. This proves execution and
isolation for those operations; it is not a Doom/Wolf3D compatibility claim.

The JLM saves the **original PTE values**, including flags, and restores all
32 exactly. It refuses unload while video or framebuffer ownership remains.
The page-table self-map is specific to the pinned Jemm revision. The original
design mapped the guest VGA aperture to RAM, which could not reproduce VGA
latches, plane masks or write modes. Since 27 September the pages are
supervisor-only guards, and every guest access is executed against the VGA
model instead.

The framebuffer API accepts an aligned physical base at or above `80000000h`,
an extent up to 64 MiB and conventional transfer buffers. The parent must
obtain the extent from a validated VBE mode. These initial restrictions keep
native XMS allocations out of the mapping path. The existing native renderer's
real-mode/unreal-mode transition cannot simply be called while under V86.

The full ABI is in [`session_abi.inc`](../src/vm/session_abi.inc). The separate
[DPMI contract](vm-dpmi-contract-2026-09-26.md) documents the shipped HDPMI 3.24
API, ownership limits and why the older research fork's callbacks cannot be
used with it. Jemm and HDPMI have separate page tables and I/O permission maps;
a JLM port handler alone does not intercept a protected-mode game's ports.

The combined check loads packaged HDPMI with `-r -v` **after** BEGIN, so it
selects Jemm's VCPI pool and constructs its low-memory page table from the
shadowed aperture. Its actual protected-mode writes at A0000 and B8000 are
then read back through the JLM, alongside real intercepted CPU port accesses.
The host unloads before END restores the original mappings. Official HDPMI
returns TSR status `AH=3`, with `AL=0/1/2` identifying raw/XMS/VCPI success;
the parent also confirms DPMI discovery. Treating every nonzero AL as failure
was a probe defect, corrected from the pinned upstream definitions.

The later unmodified-client gate keeps the same transaction ordering but extends
it across repeated normal clients, a deliberate protected #UD failure and the
unchanged packaged doom-vanille DOS/4GW executable. Jemm provides bounded host
service from physical IRQ0; the probes observed ticks across their CLI and
blocked BIOS waits. Correct preservation of the guest's virtual IF, including
PUSHFD/CLI/POPFD, remains under integration review. HDPMI preserves its own IOPB handles, pre-client PIC
masks and all 32 low-memory PTEs for each client. Official DPMI 0300h INT 10h
and 0302h calls aimed exactly at the current INT 10h vector cross through the
owned JLM virtual BIOS; arbitrary real-mode procedures are unchanged.

V86 scalar `IN AL` and `IN AX` preserve the upper bits of EAX; canaries cover
both sizes, `IN EAX` and carry preservation. V86 string I/O to VGA ports
(3B0h–3DFh) now runs against the VGA model. String I/O to any other port is
still unsupported: it records a sticky fatal diagnostic, performs no device or
memory transfer and makes READBACK fail. The owner must END the session.
This diagnostic is not a preemptive child-termination facility. The PM adapter
has its own equivalent unsupported-string path. Neither silently claims REP
semantics.

## Recorded evidence

Reports are archived under [validation/2026-09-26-vm](validation/2026-09-26-vm/README.md).
Raw screenshots, PCM captures, private disk copies and physical memory dumps
remain in ignored `build/`; report paths identify those original locations.

| Check | Evidence and scope |
| --- | --- |
| XMS extended services | 13 cases against actual assembled kernel, plus old-kernel failure reproduction |
| Live DOS SYSVARS | Five cases against actual assembled kernel, plus old-kernel failure reproduction |
| Jemm query adapter | 15 CPU-level cases, actual strategy/interrupt request fixture, malformed chains and handle lifecycle |
| HDPMI I/O range allocator | 34 cases against extracted, unchanged shipped allocator routines; both client pointer formats |
| VGA model | 141,819 expected-value assertions, ASan/UBSan, OpenWatcom freestanding compilation/link (168,060 after the 27 September video extension) |
| V86 session and framebuffer | Real DOS child, DOS file create/read/delete, COM/MZ launches, all 32 PTEs changed then restored exactly, physical B800 surface unchanged, 4,096-byte protected LFB copy, bounds, unload and desktop return |
| Protected-mode I/O | Actual CPU instructions under packaged HDPMI, 218 exceptions including one deliberately rejected string-I/O instruction, host VGA unchanged, handler removal, resident-host unload and desktop return |
| Combined V86/VCPI/DPMI | Fresh resident HDPMI under an active session, actual protected-mode A000/B800 writes visible in private readback, protected I/O checks, host removal, ordinary V86 child and exact physical-video/PTE restoration |
| Unmodified HDPMI client lifecycle | Two normal DOS/4GW probes, one deliberate #UD unwind and fixed-hash `DOOMVAN/PCDMCORE.EXE` timedemo; separate Jemm/HDPMI CR3s, actual protected instructions and I/O, observed host ticks in the probe's CLI/BIOS wait, five exact callback installs/removes, zero PTE repairs, exact 32-PTE and 128-KiB physical-aperture restoration. Final guest-IF qualification is pending. |
| Windows regression | Two Windows 3.1 sessions on the new kernel, applications, resize/repaint, WAV/MIDI and return to native UI |
| Classic Doom regression | Original fullscreen executable, actual menus/gameplay/movement, non-silent AC97 audio, quit, COM execution and native desktop return; 38,507 code bytes unchanged outside the allowed XMS entry patch |

The QEMU CPU model is Pentium III with 128 MiB RAM. These results do not
establish physical T23/E500 behavior, hardware acceleration or a 30 fps
guarantee. `graphics_virtualized: false` and `dpmi_virtualized: false` in the
monitor reports intentionally refer to the complete capability, despite the
narrow shadow and port checks passing.

## Reproduction

Build the pinned monitor and the extension without installing them in any
normal image:

```sh
bash scripts/build_jemm_monitor.sh --ciukios-device-query --ciukios-vm-scheduler
bash scripts/build_vm_session.sh
bash scripts/build_dpmi_video_probe.sh
bash scripts/build_dpmi_lifetime_probes.sh
python3 scripts/test_virtual_vga.py
```

The monitor build's `CURRENT` file names its successful output directory.
Use JEMM386/JLOAD from the **same** patched output. For a private runtime check:

```sh
python3 scripts/qemu_test_vm_session.py \
  --image build/full/native-desktop-2026-09-26/final/ciukios-native-desktop.img \
  --kernel build/full/vm-session-2026-09-26/ciukidos.sys \
  --jemm build/external/jemm-monitor/work-v4l0c_oz/output/JEMM386.EXE \
  --jload build/external/jemm-monitor/work-v4l0c_oz/output/JLOAD.EXE \
  --module build/full/dos-window-research-2026-09-26/session-final/CVSESSION.DLL \
  --dpmi-probe build/tests/dpmi-video/DPMIVGA.EXE \
  --framebuffer --output build/full/vm-session-new-run
```

The paths above identify the qualified local artifacts; after rebuilding,
substitute the newly generated output paths. The harness refuses to reuse
an output directory. It sends keyboard events to the running guest and reads
physical memory through QEMU for verification. It never injects success flags,
patches guest RAM, or stops the CPU to manufacture an observation.

The unmodified-client lifecycle gate is `scripts/qemu_test_dpmi_lifetime.py`.
It stages the packaged doom-vanille `APPS/DOOMVAN/PCDMCORE.EXE` byte-for-byte
(SHA-256 `efbe64359fb1dfe569cde2428f41ef15a40b8975b8eb36a1cfc5a2731b894980`),
uses a one-tic deterministic demo, and reads the live descriptor and both page
tables through QEMU. The deeply nested DOS parent remains halted after PASS so
those final bytes stay observable; returning that parent through CiukiOS's
current nested-EXEC path is a separate kernel limitation.

The manual monitor command used by the harness is:

```text
run \JEMM386.EXE LOAD NOEMS X=A000-FFFF NODYN MAX=32M MIN=32M NOVME
run \JLOAD.EXE \CVSESS.DLL
```

This preallocated profile deliberately avoids adopting UMBs or replacing the
native XMS owner. Load/unload is qualified in the emulator; it is not enabled
in live-CD or HDD startup. Never unload the monitor while a DPMI host or session
still owns its pages or callbacks.

## Remaining work after the scheduler/DPMI extension

1. **Video execution and presentation.** Implemented on 27 September; see
   [the video record](vm-video-session-2026-09-27.md) for results, frame
   times and remaining limits (no physical-hardware qualification). The
   window became resizable, with raw keyboard and a virtual mouse, later the
   same day; see [the device record](vm-input-audio-devices-2026-09-27.md).
2. **Native runtime integration.** Connect the qualified single-foreground
   scheduler and DPMI adapter to the desktop's launch/close path without
   turning callbacks or the one V86 context into fictitious background VMs.
   The normal fullscreen fallback remains required outside the advertised
   video/input/audio capability set.
3. **Input and sound devices.** Implement focused key make/break events and
   modifiers, mouse ownership, virtual IRQ/timer/DMA/SB/OPL behavior and an
   audio backend that shares resources with the host. Acceptance requires
   Ctrl/Space combinations, focus-loss release, audible original-game sound,
   simultaneous UI activity and clean restoration on exit.

These are independent implementation assignments around the documented
session ABI, not three claims of completed functionality. Integrate them only
after their checks pass together. Cooperative CiukiOS window ports remain
separate applications and do not establish unmodified DOS-binary execution.
Reusable assignment prompts are in [the parallel handoff](vm-next-steps-prompts-2026-09-26.md).
