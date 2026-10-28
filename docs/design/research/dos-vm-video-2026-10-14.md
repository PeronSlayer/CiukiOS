# DOS VM video: full screen on hardware and exact windowed VGA (research, 2026-10-14)

Status: research and proposal for the lead; no code changed. Author: Claude
(research agent). Inputs: `docs/directives/f3-14-dos-speed-t23.md`,
`docs/directives/f3-16-vga-planar-direct-window.md`,
`docs/validation/2026-10-14-f3-16-host.md`, T23 disk logs
`legacy/local/physical/fb3147dc/disk-log/BOOT.LOG.clean` and
`build/lead-scratch/t23-vm-perf-8c88b1ab.txt`. Every cost marked "est." is an
estimate to be replaced by a TSC record on the T23.

## 1. Summary

1. **The f3-16 premise is wrong.** DOS DOOM 1.9 does not copy four 16000-byte
   plane passes per frame. It draws the 3D view straight into VGA memory: one
   `out dx,al` to 3C5h (map mask) per wall/sprite column and one per plane for
   each floor span (`planar.asm` R_DrawColumn_/R_DrawSpan_). The fuzz drawer also writes GC 04h (read map).
2. The T23 confirms it: 8c88b1ab sample n=56 has 18 flips, 23019 #GP exits,
   13715 harvests and 18002 dirty pages in one second. That is about 1279
   port traps and 762 harvests per frame. Monitor time was 301843 µs (12.4 µs
   per exit) and guest time only 73692 µs (7 %). Present took 158226 µs
   (8.8 ms per frame). In fb3147dc steady gameplay, `faults` stays flat: page
   faults are not the cost.
3. At DOOM's 35 fps cap, 44.8k map-mask traps per second at 12.4 µs each
   would use 556 ms/s. The fix is a sub-1.5 µs VGA port fast path that does
   not change architecture, plus a cheaper presenter. Est. windowed overhead
   at 35 fps: about 5 % ports, about 10 % present and about 7 % compositor.
4. VME does not help this. It removes only real-mode INT/IRET/PUSHF/POPF/CLI/STI
   exits (about 980/s on the T23, under 1.2 % CPU). It never removes I/O traps
   (TSS I/O bitmap), and PVI does not cover POPF/IRET (f3-15). QEMU TCG does
   not offer VME. It is optional, last, and can only be proven on hardware.
5. Windows 9x did not run DOOM fast in a window. A windowed DOS box was
   virtualized by the VDD and redrawn by the grabber ("might be only two
   frames per second", Raymond Chen). Games were played full screen, with the
   VM owning the physical VGA and the VDD trapping nothing for the owner
   (KB Q75008).
6. **Proposal:** two modes, as in Windows 98.
   (a) **Full screen on hardware** for play. The desktop gives up its display
   lease. The VM gets 3C0h–3CFh/3DAh and the physical A0000h window, so the
   game runs at native video speed. Alt+Enter and Alt+Tab switch back, and
   state is captured into `cvga_state`.
   (b) **Windowed exact** for desktop use. Keep the model, add the port fast
   path, PM bursts, planar string fast paths and a Mode X/Y presenter.
7. The "game writes to real VGA while the desktop reads it back" hybrid is
   rejected. There is one VGA register file and one VRAM shared with the 1024x768x32 VBE scanout,
   the desktop's VRAM sits at the guest's A0000h offset, and UC readback is slow.
8. Exactness limit kept from f3-16: a native read through the direct window
   does not load the latches. DOOM and Wolf3D always read before a latch store,
   so it does not matter for them, but it is documented.
9. Full screen needs an amendment of `device-firmware-ownership.md`. Ordinary
   DOS VMs may not call host firmware (line 253), and `biosvm_video_switch`
   only accepts VBE LFB modes (`biosvm.c:925`).

## 2. The Windows 9x model and what CiukiOS needs

**How Windows did it.**
- The VDD (virtual display device) owns all display I/O. For the VM that
  *owns* the display, the VDD does physical I/O directly and keeps trapping
  off while that VM runs. The exceptions are small, for example a simulated
  VRTC bit. For a non-owner VM, the VDD keeps a register copy and emulates its
  I/O. Trapping and memory access are re-enabled when ownership changes.
  ([KB Q75008](https://jeffpar.github.io/kbarchive/kb/075/Q75008/), Windows
  3.x DDK; the Windows 95 VxD model keeps this split.)
- Display drivers cooperate on switches through INT 2Fh AX=4000h/4007h
  (VM-assisted save/restore). The VGA latches are saved through a reserved
  "latch address" byte in video memory, and video memory is not saved: the
  display driver redraws everything on return
  ([OS/2 Museum, Windows 3.x VDDVGA](https://www.os2museum.com/wp/windows-3-x-vddvga/)).
  AX=4001h/4002h are the background/foreground notifications
  ([RBIL 2F/4001](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-4747.htm),
  [2F/4005](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-4751.htm)).
- Windowed graphics: the VDD tells WINOLDAP that video memory changed, and the
  per-driver grabber reads the virtual video memory and repaints the window.
  Windows 95 added windowed EGA/VGA graphics; on low-end machines this ran at
  about 2 fps and was "not for DOOM"
  ([Raymond Chen, "what is a grabber"](https://devblogs.microsoft.com/oldnewthing/20231127-00/?p=109067)).
  Users report windowed Windows 95 graphics at about 3 fps, and some EGA games
  failing completely ([gekk.info](https://gekk.info/articles/dosapps.html)).
- Alt+Enter toggles window and full screen
  ([Microsoft Windows 98 DOS slide deck](https://www.slideserve.com/ashby/dos-applications-and-windows-98)).
  The PIF "Misc" tab lets a program reserve Windows shortcut keys (Alt+Tab,
  Ctrl+Esc, Alt+Enter, …). That list comes from the Windows 95/98 UI; I found
  no primary document for it.

**What CiukiOS has now.**
- All VGA I/O and A0000h accesses go through the model: `dosvm_vga.c:1-2`
  ("No physical VGA port, mode, font or firmware is used").
- The desktop holds the display lease. Mode switches go through
  `display.c:68-87` (`apply`/`restore`, `biosvm_video_switch`) with a 10 s
  frame watchdog (`display.c:94-101`).
- The f1-23 amendment requires stopped presentation, mapping revocation, VGA
  leases and serialized VBE calls with 2 s budgets
  (`device-firmware-ownership.md:395-405`).

**What full screen needs** (each item maps to section 5):

| Item | Design |
| --- | --- |
| Lease hand-off | Stop the compositor and revoke the LFB/console mappings (the f1-23 path, `fbdev_change_begin`). Prove the Savage 2D engine is idle (f4-26 cached idle proof), then grant the VM a *display lease*. A panic while the lease is held goes to serial only; the watchdog restores the desktop mode. |
| Enter VGA | The BIOS VM sets a standard VGA mode once (INT 10h AX=0013h, or 0003h for text). This needs a `biosvm_video_switch` variant for modes < 100h; today `biosvm.c:925` refuses them. Then the monitor loads `cvga_state` into the hardware: SR00 synchronous reset around the sequencer, CR11.7 unlock, ATC flip-flop through a 3DAh read, DAC, 4×64 KiB planes through the map mask, and latches through the write-mode-1 latch-byte trick. Guest INT 10h mode sets stay synthetic (`dosvm_vga_bios`); each one ends with the same model→hardware load, so no further firmware call is needed. |
| VM grants | The TSS I/O bitmap opens 3C0h–3CFh and 3DAh (3BAh in mono). 3D4h/3D5h stay trapped and are forwarded: 2–3 writes per frame keep the CRTC start/flip count in the model and the records. A0000h–BFFFFh map the physical frames (UC, as the firmware's fixed MTRRs set them; no WC for planar). |
| Leave (Alt+Enter, Alt+Tab, Ctrl+Esc, VM exit) | Close the I/O bitmap. Read the registers back into `cvga_state` (VGA registers are readable: 3CCh misc, indexed SR/GC/CRTC/ATC, DAC through 3C7h). Read the latches back through the latch byte. Read the four planes with GC04 read map and read mode 0 (256 KiB of UC reads). Restore the desktop VBE mode through `display.c` `apply`, then do a full compositor redraw: no VRAM save, like the VDD/display-driver contract. The VM continues windowed (exact model) or suspended, as a per-program option. |
| Hotkeys | Filter them in the host input path before the virtual 8042 (`dosvm_kbd.c` scan handling, around line 226). In full screen the focused VM gets all other keys. Alt+Enter is never delivered to the guest. Ctrl+Alt+Del stays a system key. |
| Scope limit | Standard VGA modes and their tweaks (13h, Mode X/Y, 12h, text) only. VBE modes requested by DOS games in full screen need a later amendment: guest 4F02h through the BIOS VM with an LFB grant. |

Est. switch latency on the T23:
- BIOS mode set: 10–100 ms.
- Plane load, 256 KiB UC writes: 5–25 ms.
- Capture, 256 KiB UC reads: 25–130 ms.
- Desktop 4F02h and redraw of 3 MiB to the WC LFB: about 10 ms plus composition.

Total: under 0.5 s, in line with Windows 98's black flash. Measure each part.

## 3. Fastest exact windowed strategy

**Measured cost model.** T23, 8c88b1ab n=56, one second of DOOM, 18 frames:

| Quantity | /s | /frame | Source |
| --- | ---: | ---: | --- |
| #GP exits (PM port traps) | 23019 | 1279 | `vm_perf_exits gp` |
| Harvests | 13715 | 762 | `vm_perf_video harvests` |
| Dirty pages | 18002 | 1000 | `vm_perf_video pages` |
| Monitor µs | 301843 | 16769 | `vm_perf_cpu`; 12.4 µs per exit |
| Guest µs | 73692 | 4094 | `vm_perf_cpu` |
| Present µs | 158226 | 8790 | `vm_perf_present`; 480 rows/frame, black borders included |

About 47 % of that second is in none of these counters: other tasks, the
compositor's LFB copy, or idle. Step 0 must attribute it.

**Why each OUT costs 12 µs.**
- A changed SR02 calls `dosvm_vga_direct` (`dosvm_vga.c:108,133`).
- That calls `video_harvest`: 16 `as_lookup` calls plus `as_map` for each
  dirty page (`:182-207`).
- Then `video_map`: 16 `as_map` calls (`:208-216`). Each one reads CR3 and
  executes INVLPG (`vmm.c:249-272`).
- All of this runs after the generic DPMI #GP decode (`dpmi_dispatch.c:1010-1021`).

DOOM's pattern (gamesrc-ver-recreation-doom
[`planar.asm`](https://github.com/fabiangreffrath/gamesrc-ver-recreation-doom/blob/master/planar.asm),
a reconstruction of the DOS source, GPL):
- `R_DrawColumn_` (lines 81-99) and `R_DrawColumnLow_` (58-77) do one OUT per
  column: mask `1<<(x&3)`, or 3/12 in low detail.
- `R_DrawSpan_` (217-267) and `R_DrawSpanLow_` (376-425) do one OUT per plane
  per span.
- [`r_draw.c`](https://github.com/fabiangreffrath/gamesrc-ver-recreation-doom/blob/master/r_draw.c):
  the fuzz column (174-192) writes `GC_READMAP` and reads VRAM. `R_VideoErase`
  (510-527) uses write mode 1 latch copies from 0xAC000.
- [`i_ibm.c`](https://github.com/fabiangreffrath/gamesrc-ver-recreation-doom/blob/master/i_ibm.c)
  (494-507) triple-buffers A0000/A4000/A8000 and flips through CRTC 0Ch.

Wolf3D
([`WL_DRAW.C`](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_DRAW.C)
970-1000, [`WL_SCALE.C`](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_SCALE.C)
255-384):
- `VGAClearScreen` sets mask 15 and does `rep stosw` per view row.
- The scaler does one OUT per strip, with masks of 1–4 planes.

**Reference implementations.** All of them are full emulators: every planar
access is a handler call, so exactness is free and speed is not.
- **DOSBox** ([`vga_memory.cpp`](https://sources.debian.org/src/dosbox/0.74-3-4/src/hardware/vga_memory.cpp/)):
  `ModeOperation` (97-137) handles write modes 0–3. The unchained read loads
  the 32-bit latch from dword-interleaved VRAM (139-158). `VGA_SetupHandlers`
  (757+) picks a per-mode page handler. Changes are tracked under `VGA_KEEP_CHANGES` (50).
- **DOSEMU2 VGAEMU** ([`vgaemu.c`](https://github.com/dosemu2/dosemu2/blob/devel/src/base/dev/vga/vgaemu.c)):
  `vga_emu_fault` (1085) leaves planar pages protected and calls
  `instr_emu_sim` (1188-1211). Linear modes unprotect the page and mark it
  dirty. [`instremu.c`](https://github.com/dosemu2/dosemu2/blob/devel/src/base/video/instremu.c)
  (header, 10-25) stays in emulation until COUNT instructions in a row do not
  touch VGA memory, and it **short-circuits IN/OUT to standard VGA ports**.
- **86Box** ([`vid_svga.c`](https://github.com/86Box/86Box/blob/master/src/video/vid_svga.c)
  1965-2144) and **QEMU** ([`vga.c`](https://github.com/qemu/qemu/blob/v10.1.0/hw/display/vga.c)
  869-1027) use the same per-access write modes with 4 KiB dirty tracking
  (`changedvram[addr>>12]`, `memory_region_set_dirty`).

**Candidates compared** (est. at 1.13 GHz; DOOM at 35 fps):

| Candidate | Exact? | DOOM cost | Wolf3D | Verdict |
| --- | --- | --- | --- | --- |
| A. Current path (12.4 µs per OUT) | yes¹ | 44.8k × 12.4 µs = 556 ms/s | strip OUTs plus bursts | too slow |
| B. **VGA port fast path.** The decoder checks the opcode first (EE/EF/E6/E7 and 3C4/3C5/3CE/3CF). A map-mask change between single planes swaps 16 PTE frames directly in the VM's low page table: read the D bits, then INVLPG ×16, or one CR3 reload, whichever measures cheaper. Same path in V86 and PM. | yes¹ | #GP round trip 0.3–0.5 µs + swap 0.4–0.8 µs ≈ 1–1.5 µs ⇒ 45–67 ms/s (5–7 %) | helps every strip OUT | **do first** |
| C. Per-instruction emulation of all planar stores (DOSEMU style) | yes | about 400k emulated instructions/frame × 60–120 cycles ≈ 20–40 ms/frame ⇒ more than 70 % CPU at 35 fps | acceptable only for latch/multi-plane runs | only for non-direct modes |
| D. Decoded-instruction cache for C | yes, if invalidated | DOOM patches its own inner loop on every column (`planar.asm` patch1/patch2), so the cache thrashes | small gain | reject |
| E. Extend bursts to PM, with VGA OUT/IN, register ALU and short branches inside the burst, plus REP STOS/MOVS planar fast paths (memset/memmove per selected plane for write mode 0 identity and write mode 1 mask 15) | yes | fuzz and erase only | `VGAClearScreen` drops from one fault per row to one per frame; latch copies become memmove | **do second** |
| F. Plane-coherent pages: a mask with several planes maps the lowest plane directly when all selected planes are byte-equal in that page, and the dirty page is copied to the other planes at harvest. Coherence is lost when a single-plane store dirties the page. | yes, by construction | low detail only | gains after `VGAClearScreen` are uncertain | experiment after E |
| G. Game writes to real VGA in the background while the desktop reads back | no (see §1.7) | readback est. 9 MB/s at 2–30 MB/s ⇒ 30–100 % | — | reject |
| H. VME/PVI | — | PM I/O unaffected; real-mode exits 980/s × 5–12 µs ≈ 0.5–1.2 % | IRQ reflection only | optional, hardware only |

¹ Except for the latches loaded by native reads through the direct window
(f3-16 record, "Findings"). A stale-latch store needs write mode 1, set/reset,
a logic op or a bit mask with no trapped read before it. The records should
count that case: when entering a latch-dependent mode with an open window
whose pages have A=1 and D=0.

**Presenter.** `dosvm_vga_present` (`dosvm_vga.c:584-680`) renders 480 rows
through the generic `cvga_render_row8` for each flip. The 2x Mode Y frame is
640x400; the 80 black rows are redrawn on every display change. Proposals:
- A Mode X/Y row converter: `out[x] = plane[x&3][start + x/4]`, then LUT and
  2x scaling.
- Skip unchanged border rows.
- Present only on a flip or a VGA frame tick.

Est. 0.3 ms conversion + 1–1.5 ms writing 1 MiB of 32 bpp to the RAM surface,
plus the compositor's about 2 ms to the WC LFB. That is about 3–4 ms/frame,
110–140 ms/s at 35 fps.

**VME facts** (Intel SDM Vol. 3B, "8086 Emulation": ch. 20 in pre-2023
revisions; `v86.c:1` cites ch. 23 of revision 090):
- CR4.VME, with IOPL<3 in V86, makes CLI/STI/PUSHF/POPF act on VIF.
- STI, or a POPF/IRET that sets IF while VIP=1, raises #GP.
- INT n is redirected through the V86 IVT without a monitor exit when its bit
  in the 32-byte interrupt redirection bitmap below the TSS I/O bitmap is 0.
- I/O instructions in V86 always consult the I/O bitmap and ignore IOPL.
- CR4.PVI covers only CLI/STI at CPL 3.
- QEMU v10.1.0 lists `CPUID_VME` as missing from `TCG_FEATURES`
  ([`target/i386/cpu.c:899-908`](https://github.com/qemu/qemu/blob/v10.1.0/target/i386/cpu.c)).

## 4. Decision table

| Situation | Mode | Video path | Hotkeys (Windows 98 model) |
| --- | --- | --- | --- |
| Game launched from a game shortcut, default per program | Full screen on hardware | VM owns the VGA ports and A0000h; CRTC trapped; model idle | Alt+Enter → window; Alt+Tab/Ctrl+Esc → desktop, VM windowed or suspended (per-program option) |
| Game or tool in a window | Windowed exact | Model, direct plane/linear window, port fast path, bursts | Alt+Enter → full screen |
| DOS prompt, text tools | Windowed exact (text) | Model; idle detection (f3-17) | Alt+Enter → full-screen text (same hand-off, mode 03h) |
| VBE mode requested in full screen | Windowed exact until amendment | Model VBE | Alt+Enter refused with a status message |
| Display lease cannot be taken (Savage busy, BIOS VM quarantined, failed mode set) | Stay windowed | Model | Show the reason; never leave the screen dark: watchdog → `display.c restore` |
| Second DOS VM while one is full screen | Background, windowed model | Model | Only the focused VM can own the display |

## 5. Implementation plan

Each step: host tests first, then QEMU, then T23. "HW" marks what only the T23
can prove.

0. **Attribution (no behaviour change).**
   - Add verbose TSC records: cost per VGA port (3C4/3C5/3CE/3CF counts and
     cycles), cost per harvest, compositor LFB copy µs, and VM runnable but
     not running µs.
   - Add a host replay from `planar.asm`: an OUT per column, two-pixel stores
     at stride 80, span OUTs per plane, triple-buffer flips.
   - Gate: kernel host suites; `f0-smoke`.
   - HW: one verbose DOOM run on the T23 fills the table in §3 (closes the 47 % gap).
1. **Port fast path (B).**
   - Opcode-first decode in the V86 and PM #GP handlers.
   - Cached pointer to the VM's low page table for the A0–AF PTEs.
   - D-bit read and PTE swap in one critical section.
   - Measure INVLPG ×16 against a CR3 reload.
   - Host: identical plane bytes to the full model on the DOOM replay and on
     the f3-16 vectors; exits per frame unchanged; cycles per exit recorded.
   - QEMU: `f3-vm` video cases and the DOS window goldens unchanged;
     `doom-qemu-t23`.
   - HW: port exits ≤ 1.5 µs average; DOOM flips/s.
2. **Presenter.**
   - Mode X/Y converter, border skip, present per flip.
   - Host: `dosvm_present_bench` present µs per frame; golden digests unchanged.
   - QEMU: `vm-window` and the compositor goldens.
   - HW: present ≤ 4 ms/frame.
3. **Bursts and string fast paths (E).**
   - Extend `video_burst` (`dosvm_vga.c:444-484`, V86 only at `:522`) to DPMI
     frames.
   - Handle VGA OUT/IN inside the burst, register-form ALU and Jcc/LOOP.
   - REP STOS/MOVS per-plane paths for write mode 0 identity and write mode 1
     mask 15.
   - Host: Wolf3D replays (near/mid/far/latch); faults per frame drop; exact
     plane and latch checks.
   - QEMU: `wolf3d-qemu-t23`.
   - HW: Wolf3D ≥ 70 fps.
4. **Full screen on hardware.**
   - **Needs owner approval.** Amend `device-firmware-ownership.md`
     (display lease for a DOS VM; BIOS VM standard-mode set) and
     `f3-acceptance.md` first, with a cross-review.
   - 4a. Hardware state module: save and load VGA registers, DAC, latches and
     planes. Host-tested against a fake port/VRAM backend built on
     `cvga_state`: round-trip identity and programming order.
   - 4b. A `biosvm_video_switch` variant for mode 13h/03h under the 2 s budget.
   - 4c. Lease hand-off: compositor stop, mapping revocation, I/O bitmap
     grant, physical A0000h map. Reverse path with capture, `apply` of the
     previous mode and a full redraw. Watchdog.
   - 4d. Hotkeys in the input path.
   - QEMU (`std` on `qemu-t23`/`qemu-e500`, `cirrus` on `qemu-desktop-1998`;
     both emulate real planar VGA): enter/exit ×10. A QMP screendump of the
     full-screen frame equals the model render of the captured state. Desktop
     frames resume without a watchdog restore. No leaked grants.
   - HW: SuperSavage BIOS mode set and LCD expansion; extended CR state after
     return; Savage 2D idle; switch latency; native DOOM fps (CRTC flip count).
5. **VME (optional, H).**
   - Detect with CPUID.1:EDX[1]; CR4.VME, redirection bitmap, VIP-driven
     delivery; record the active path.
   - Host: TSS layout and VIP logic.
   - QEMU proves only the trap path.
   - HW: real-mode exit counts and correctness on the T23.
6. **Plane-coherent pages (F, optional).** Only if Wolf3D or low-detail DOOM
   misses its target after step 3. Host replays decide; no QEMU-specific gate.

**Proven only on hardware:** VME/PVI behaviour; SuperSavage VGA mode entry,
LCD expansion and the return to VBE; real exception, INVLPG and UC VRAM
costs; switch latency; fps targets.

## 6. fx-doomwin (2026-10-27): step 1 at trap entry, and what still costs

Directive `docs/directives/fx-doomwin.md`; validation and figures in
`docs/validation/fx-doomwin.md`. T23 trial 6c3b7800 (Pentium III-M, DOOM in
the window, 5 s banks n=3/n=4): 103,224-137,500 VGA port exits at
5.4-5.9 µs (the `vm_perf_port` rank 1 is 03C5h with 102,888-132,973), and
the per-exit `remap_us` is about 2.06 µs. These selects already took
fx-27's lean exit; what remained was the generic exit around a one-byte
register store and one 16-entry PTE exchange.

**How the references handle a sequencer write.**
- Windows 3.x/9x VDD: for a VM in the background or a window it "traps all
  the video I/O, saving the output port values and emulating the input
  port values", and maps or hooks the video pages from the VM's virtual
  controller state in its page-fault service (Windows 3.0 DDK, *Virtual
  Device Adaptation Guide*, 18.1.3 "VDD I/O Trapping and Hooked Pages",
  [PCjs copy](https://www.pcjs.org/documents/books/mspl13/win/w3ddkvxd/);
  `Hook_V86_Page`, 19.2). CiukiOS follows that split: every SR02 write
  traps, the model keeps it, and the A0000h window maps the plane it
  selects. The Windows 9x VDD (VDD.VXD plus a vendor mini-VDD) keeps this
  architecture: M. Necasek, "Windows 3.x VDDVGA", OS/2 Museum, 2023-01-23
  ([article](https://www.os2museum.com/wp/windows-3-x-vddvga/)): "The VDD
  is meant to virtualize VGA registers and not let DOS applications touch
  the real hardware"; the VDDVGA.386 source shipped with the Windows 3.1
  DDK; Windows 9x "was, as you say, architecturally the same" (the
  author, in the article's comments).
- DOSBox Staging
  ([`vga_seq.cpp`](https://raw.githubusercontent.com/dosbox-staging/dosbox-staging/main/src/hardware/video/vga_seq.cpp)
  `write_p3c5`, case 2): stores `map_mask` and the expanded
  `full_map_mask`; no handler or mapping change (only SR04 calls
  `VGA_SetupHandlers`). Every unchained store goes through a memory handler
  that applies the mask, so the select itself is free and every store pays.
- QEMU ([`vga.c`](https://raw.githubusercontent.com/qemu/qemu/master/hw/display/vga.c)
  `vga_ioport_write`, `VGA_SEQ_D`): stores the register and calls
  `vga_update_memory_access`, which only changes the chain-4 alias;
  `vga_mem_writeb` applies `sr[VGA_SEQ_PLANE_WRITE]` per store. Under KVM
  the legacy window is coalesced MMIO (`memory_region_set_coalescing`), so
  its stores are batched instead of exiting one by one; the VGA port list
  flushes that buffer first (`portio_list_set_flush_coalesced`), so every
  port write still exits. Without VT-x the same holds here: every guest
  OUT to a port closed in the TSS I/O bitmap is a #GP (Intel SDM Vol. 1,
  "I/O Permission Bit Map").

**Decision.** Keep the CiukiOS model (trap, store, map the selected plane):
it lets DOOM's column stores run natively, which no reference does. Cut
the exit around the store instead: `dosvm_trap_fast` (`dosvm.c`) retires
OUT DX,AL/AX to 3C4h/3C5h (SR02) and 3CEh/3CFh (GR04/GR05) from
`trap_dispatch` before its per-exit task accounting, with physical IF
clear to the IRET, under the fx-27 lean conditions, in an unchained planar
window only (chain-4 off, so no linear seed or 64 KiB harvest can occur in
that IRQ-off section), reaching exactly `dosvm_vga_port`'s state
(`dosvm_vga_select_fast`). It leaves out the frame copy, `cpu_trap_enter`/
`cpu_user_return`, the second IRQ-off section, the generic port dispatch,
the burst probe and the two `cpu_path_clock` reads; every VM exit's
boundary stamps lose their CPUIDs (`dosvm_perf_stamp`), and a DPMI
client's exits no longer scan the 256-slot native thread table on their
way back to ring 3 (`proc_signal_return_to_user`). The 16-entry exchange
stays: fx-doswin3 already invalidates only entries with A set
(`vmm.c as_replace_pages`), and a lazily mapped window would trade the
other twelve locked exchanges for page faults whenever a column's page
range moves. OPL writes (388h/389h) could take the same entry; not done
here.
