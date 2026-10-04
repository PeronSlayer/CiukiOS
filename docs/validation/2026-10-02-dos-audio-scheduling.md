# DOS-window audio and scheduling, 2026-10-02

## Source contracts checked

- The [Intel 8259A specification](https://www.pcjs.org/documents/datasheets/intel/INTEL_8259A_PIC.pdf) describes an edge-triggered request latched in the PIC's IRR until interrupt acknowledgement. Reading the Sound Blaster DSP's IRQ-acknowledge register lowers the device request; it must not erase the PIC's already latched edge.
- The [Creative Sound Blaster hardware programming guide](https://pdos.csail.mit.edu/6.828/2005/readings/hardware/SoundBlaster.pdf) documents an interrupt at each auto-init DMA block and separate DSP acknowledgement for 8-bit and 16-bit transfers. The device model now preserves the PIC IRR when either DSP acknowledge register is read.
- The pinned [Jemm source](https://github.com/Baron-von-Riedesel/Jemm) and local `patches/jemm-ciukios-vm-scheduler.patch` establish that profile callback 6 runs on return to V86, before virtual IRQ injection, with a `Client_Reg_Struc` frame in EBP. `jlm_poll` uses that boundary after device polling to select a ready VM with a held PM audio interrupt. It calls the existing `vmm_switch`, which retains the InDOS and FAT write-back guards.
- The pinned [HX/HDPMI source](https://github.com/Baron-von-Riedesel/HX) and local `patches/hdpmi-ciukios-session-adapter.patch` establish the protected-mode IRQ checks on trapped I/O, client interrupt return and physical IRQ0. The observed failure was not caused by a continuously disabled client IF.

## Reproduction and decision

The original M4 test sometimes reported silent PCM without checking whether its injected Ctrl keys reached DoomVan. It was then changed to compare on-screen ammo after Ctrl, but that check was also insufficient: the game was launched without arguments, so [id Software's Doom startup path](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/d_main.c) enters the attract/demo loop (`D_StartTitle`) unless `-warp`, `-skill`, `-episode` or a similar autostart option is supplied. Demo playback can change ammo without player input. A valid input/audio gate must start a real level, for example `PCDMCORE.EXE -warp 1 1 -nomusic`, then verify that Ctrl changes ammo from a stable player scene. The harness now rejects a Doom audio gate without one of those gameplay arguments.

An audio failure during Doom's attract/demo playback in `build/tests/audio-root-gatetrace-run-1/report.json` started SB16 auto-init DMA at 4 ms, raised the first IRQ7 at 38 ms, and then left the desktop VM running from 42 to 259 ms with the Doom VM marked urgent. The Doom VM's held PM IRQ remained pending while further DMA blocks completed; when it finally ran, Doom sent DSP command D5 and stopped audio. During that interval, the desktop VM's InDOS and FAT-dirty flags were zero and the scheduler's gate counter did not advance. The long interval therefore came from waiting for a time-slice boundary, not from the DOS/FAT safety gate. The HDPMI IF-blocked counter was zero in that failure.

The audio model now targets the owning VM's PM state and shared video header even when a different VM is current. A background PM Sound Blaster IRQ marks that owner urgent. At the end of Jemm's return-to-V86 poll, `jlm_poll` switches to that ready owner from the canonical client frame. The scheduler still defers a switch while the current VM is inside an unsafe DOS operation or has a dirty FAT cache. The final regular quantum is two IRQ0 ticks; see the second-VM input regression below.

## Focused validation

- `scripts/test_guest_peripherals.py`: 1,079 assertions passed, including the edge-latched Sound Blaster PIC regression.
- Clean CVSESSION and HDPMI 3.24 builds passed in capped scopes; their binaries were tested from a scratch full HDD image.
- DoomVan M4 audio with the poll switch produced nonzero PCM in three consecutive attract/demo runs, with more than 600 Sound Blaster blocks per run and no AC'97 underruns in the sampled reports. A subsequent clean-binary run produced sampled PCM energy 14,066,186 and passed game close, desktop mouse, two DOS windows and click focus. These runs do not establish player-triggered effects because they did not autostart a level.
- With one-tick quantum, the VMM clock probe recorded 182 ticks in VM1 and 181 in VM0 over 10 seconds (`build/tests/audio-root-slice1-clock-4/report.json`). That focused run used the earlier VMFORK binary because the then-current compaction routine failed to allocate its auxiliary relocation stack; the VMFORK owner corrected that separate issue afterward.

**Open regression:** a later run against the canonical full image showed an ammo change but recorded silent PCM. Because attract/demo playback can change ammo, neither that run nor the three scratch-image passes establish stable player-triggered audio. A new M4 gate with level autostart is required. The clean M4 run also did not complete its final keyboard-to-second-VM marker: the second DOS window opened and became focused, but the injected `echo M4 SECOND VM` did not appear. The separate canonical `qemu_test_vmm.py --parts multi` gate passed exact single-key routing to VM1 and VM2; the longer `COMMAND.COM` sequence remains unresolved. Clock accounting, window creation and focus transitions passed in the runs above.

## Virtual PIC EOI follow-up

The longer M4 run showed the second VM's `CvInService` stuck at IRQ1 while its keyboard model forwarded 54 keys without drops. Disassembly identified its saved V86 frame at a DOS interrupt wrapper epilogue, with IF set in the pending IRET flags; this frame alone does not show whether the keyboard handler issued EOI. A separate QEMU snapshot showed physical PIC IRQ0 in service concurrently with virtual IRQ0. In the pinned Jemm profile, `CvIO` previously sent every EOI to the physical PIC whenever *any* physical ISR bit was set, leaving the virtual ISR unchanged. The physical IRQ can be unrelated to the virtual handler that issued that EOI.

An experimental Jemm change gave V86 EOIs priority over physical EOIs while leaving protected-mode service 7 physical-first. It compiled in a capped scratch scope. A canonical M4 run with `PCDMCORE.EXE -warp 1 1 -nomusic` then passed actual player input and PCM (sampled energy 12,732,925) but still failed the second VM's keyboard marker with virtual IRQ1 in service. The change was reverted because it did not fix the regression and could alter HDPMI physical IRQ pass-up.

A later raw snapshot showed that the TSS I/O bitmap traps both PIC ports 20h and 21h (bitmap byte `03h`). VM2's INT 09h vector points to a resident wrapper at `0300:88E2`; its busy flag at `0300:961D` changes from `00h` to `80h` after the first key and remains set. The wrapper chains to the BIOS handler at `F000:E987` and clears the flag only after that handler returns. This narrows the open regression to completion of the BIOS keyboard chain or its saved execution state. The earlier saved `1713:063C` frame is a DOS interrupt wrapper epilogue, not proof that the keyboard handler reached its EOI. The full raw snapshot and decoded vectors are in `build/tests/audio-root-vm2-irq9-chain/`. DOS/4GW IRQ regression remains to be repeated on the next finalized image.

The [SeaBIOS PS/2 handler](https://github.com/coreboot/seabios/blob/master/src/hw/ps2port.c) reads the controller status and data, processes the key, reenables the keyboard with controller command `AEh`, then acknowledges the PIC. Its [key processing path](https://github.com/coreboot/seabios/blob/master/src/kbd.c) calls INT 15h AH=4F. The local controller model supports `AEh`; its error and pending-command counters were zero in the failure. VM2's BIOS data area keyboard ring was nearly full (head `30h`, tail `2Eh`) and contained `echo M4 SECOND ` without the final Enter. This shows keys reached the BIOS ring while the second COMMAND shell did not drain them through INT 16h. A final valid M4 run passed Doom input and PCM (sampled energy 13,091,046) but again failed the VM2 marker. A live CPU capture during VM2 scheduling landed in Jemm's protected-mode port-read routine (`0008:F840387A`), not in the guest shell, so it did not identify the shell's stalled instruction. Raw data and report are in `build/tests/audio-root-vm2-live-final/`. The VM2 keyboard regression remained unresolved at that point.

## Second-VM input regression and two-tick correction

SeaBIOS's [INT 16h implementation](https://github.com/coreboot/seabios/blob/master/src/kbd.c) checks the BIOS data area head and tail, then waits for an IRQ when the queue is empty. Its [wait path](https://github.com/coreboot/seabios/blob/master/src/stacks.c) uses `sti; hlt; cli` when hardware IRQs are available. This makes resumed BIOS waits a relevant scheduling case; the source does not by itself prove the cause in CiukiOS.

The canonical full M4 run `build/tests/final-m4-bda-20261002` passed DoomVan player input and PCM (sampled energy 14,106,495) and opened both DOS windows, but the focused VM2 did not execute `echo M4 SECOND VM`. Its private BIOS keyboard queue changed from empty (head=tail=46) to nearly full (head=46, tail=44) while the head stayed fixed. The active VM2 page-table entry for the BIOS data area matched the saved VM2 entry. A disposable COMMAND.COM that logged its input loop reached `INT 16h/AH=00h` (`PQI` on COM1) and never logged the return marker. A disposable nonblocking BIOS poll and a direct BIOS data area dequeue also left the second window blank. A shorter two-window test reproduced the failure without Doom, whereas the same shell accepted `echo HELLO` with one DOS window.

The one-tick VMM quantum had been introduced for audio latency. With only `VMM_SLICE` restored from 1 to 2 in a disposable CVSESSION.DLL, the short two-window test displayed and executed `echo HELLO` in VM2 (`build/tests/m4-instrument/quick-two-slice2/after.png`). The full M4 gate with this module then passed player input, PCM, two DOS windows, VM2 keyboard routing and independent close (`build/tests/m4-instrument/m4-slice2`). This comparison justifies keeping a two-tick regular quantum while the return-to-V86 urgent-audio switch provides low-latency service for an active protected-mode audio owner. The exact point at which a one-tick switch interrupts SeaBIOS's wait remains a hypothesis; the change is supported by the isolated A/B runtime result.

## Rare silent start after the native-process and UMB changes (evening)

On a copy of the canonical full image built after the native-process and
VMFORK UMB changes (with only the corrected `CVSESS.DLL`), the first full M4
run (`build/tests/nres-fix/m4`) confirmed player fire but recorded silent PCM.
Its device report showed 18 DSP commands, `sb_blocks=0` (no Sound Blaster
IRQ was ever raised), `unsupported_count=0`, `buffers_rendered=2625` and no
AC'97 underrun. The SB16 registers were at reset defaults and DMA channel 5
was programmed for auto-init at `0x2CEF0`, 4 KiB, then masked.

`PCDMCORE.EXE` uses the Apogee Sound System 1.1, not DMX. Its
[`MV_TestPlayback`](https://github.com/jimdose/Apogee_Sound_System/blob/master/SOURCE/MULTIVOC.C)
waits `clock() + CLOCKS_PER_SEC * 2` for `MV_MixPage`, which only the SB
interrupt service advances; on failure `MV_Init` calls `MV_Shutdown` and the
game runs silent. OpenWatcom's [`clock()`](https://github.com/open-watcom/open-watcom-v2/blob/master/bld/clib/time/c/clock.c)
for DOS uses the DOS date/time. CiukiDOS `int21_get_time` takes seconds from
INT 1Ah AH=02h and synthesizes hundredths (+7 per call), so the window is one
to two real RTC seconds. A failed run therefore needs no SB IRQ for at least
one second after DSP `B6h`.

A diagnostic CVSESSION (never installed in the canonical image) recorded SB
and DMA port events and IRQs per device instance. The normal Apogee start is
two DSP resets, version `E1h` (4.05), `41h/42h` 11,000 Hz, `D0h`, `D3h`,
DMA channel 5 mode `59h` at page 2, count `07FFh`, unmask, `41h/42h` 11,025
Hz, `D1h`, then `B6h 30h FFh 01h`. In the traced runs, the first IRQ 7 followed
`B6h` after 6.5–34.5 ms in all 21 measured runs. No owed IRQ0 ticks were delivered in that interval.

Excluded by measurement or source: a masked DMA channel stopping the model
(would increment `unsupported_count`), a stopped AC'97 stream with an active
owner (would count underruns), a host stall of QEMU (would drain the eight
queued AC'97 buffers), a burst of owed IRQ0 ticks advancing `clock()` (that
`clock()` reads the RTC), the stored DMA mode (`59h & FCh` is `58h`), and the
TSC wrap in `advance()`. A one-byte A/B that disabled the VMFORK UMB
relocation passed once; the unchanged image then passed in 42 further runs (untraced and traced), so
the UMB relocation is not established as the cause.

Totals on this image family: 1 silent start in 43 runs. The cause of the
missing first interrupt in that run remains unidentified; the traced build
did not reproduce it.

Separate defect found: [SeaBIOS `handle_1a02`](https://github.com/coreboot/seabios/blob/master/src/clock.c)
returns CF with CH/CL/DH unchanged when `rtc_updating()` sees UIP for more
than 15 ms by its TSC timer, which a V86 VM can exceed if it loses the CPU in
that loop. `int21_get_time` ignores CF and returns the caller's registers as
the time. It also synthesizes non-monotonic hundredths. Both should be
corrected in CiukiDOS (retry or last good time, and hundredths from the BIOS
tick count); the change alters the kernel layout and was deferred.
