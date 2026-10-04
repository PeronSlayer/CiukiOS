# DOS upper-memory resident boundary (2 October 2026)

## Source behavior and decision

- [Microsoft Q71865](https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/71/865.HTM) requires the UMB provider to be installed before `DEVICEHIGH` entries. `DOS=UMB` links the upper region into DOS's MCB chain; paging alone does not make it allocatable by DOS.
- [The pinned Jemm manual](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Readme.txt) says that command-line `LOAD` leaves DOS unaware of supplied UMBs. Jemm's own XMS UMB bookkeeping is a table in extended memory, not a DOS MCB chain (sections 1 and 5).
- [The FreeDOS kernel's UMB initializer](https://github.com/FDOS/kernel/blob/master/kernel/config.c) requests UMBs from XMS, creates a conventional-to-upper MCB bridge, and then initializes the actual upper MCB. [Its allocator](https://github.com/FDOS/kernel/blob/master/kernel/memmgr.c) carries the link state through memory allocation and process cleanup. These are DOS ownership operations, distinct from Jemm moving its own resident body high.
- [Ralf Brown's INT 21h/58h reference](https://fd.lod.bz/rbil/interrup/dos_kernel/2158.html) defines the high allocation strategies and UMB link state. A high strategy requires a linked UMB chain; the API returns CF and error 1 for an unsupported request.

In CiukiOS, `src/com/shell.asm` starts `AUXSTACK.COM`, then `LFN.COM`, then `VMSTART.COM`. `VMSTART.COM` loads JemmEx from the command line with `I=CD00-EBFF`; it cannot offer its XMS UMBs to the two programs which have already become resident. `src/boot/floppy_stage1.asm` builds and rebuilds only the conventional MCB arena below the BIOS/EBDA limit. Its previous `INT 21h/58h` implementation falsely acknowledged `AX=5803h, BX=1` while never linking an upper arena, and silently changed unsupported high strategies to low first fit.

The kernel now reports link state 0, accepts a no-op unlink, and returns CF with error 1 for link requests and high/invalid strategies. The focused `MEMSTRAT.COM` probe checks those results and verifies the unchanged low strategy before exercising first, best and last placement. This fixes the false DOS API contract; it does **not** move `AUXSTACK` or `LFN` high or raise the measured conventional block.

## Required resident-memory work

To gain conventional memory from those residents, CiukiDOS must acquire and retain a Jemm XMS UMB region, build a DOS-visible MCB bridge and allocator for it, support upper block ownership and cleanup across `EXEC` and DOS VM snapshots, then provide a high loader for the resident programs. The startup order and inherited interrupt vectors must be tested with `LFN.COM` and `AUXSTACK.COM`, including forked DOS VMs. `src/vm/session_vmm.inc` makes pages `00h..9Fh` private but leaves Jemm-mapped UMBs global; its separate `C0h..EFh` cloning covers only identity-mapped writable BIOS RAM. An LFN hook moved into a Jemm UMB would therefore share its writable resident state across DOS VMs unless the monitor also gained per-VM UMB mapping. Moving these programs after the V86 monitor or copying only their code into an XMS UMB would not establish the required DOS ownership or per-VM hook behavior.

The existing VMFORK compaction moves known residents *within the private conventional-memory copy* and is validated at a 543 KiB forked largest block in the 256 MiB QEMU full profile. It is an independent gain and remains in place.

## Measured Jemm UMB layout and teardown obstacle

On a disposable copy of the full HDD image booted with 128 MiB QEMU RAM,
the post-Jemm XMS `AH=10h` largest-block query (`DX=FFFFh`) returned failure
`AX=0, BL=B0h` and `DX=0F00h`: the largest single free block was 3,840
paragraphs, or 60 KiB. A 1,024-paragraph request then succeeded at
`CD2Ch` and `AH=11h` released it. In a separate boot, four successive
1,152-paragraph (18 KiB) requests all succeeded at `CD2Ch`, `DC2Ch`,
`D1ACh` and `E0ACh`; the probe released each before exit. The addresses
show several Jemm-managed UMB blocks rather than one contiguous DOS arena.
The serial evidence is in
`build/tests/umb-map-probe-20261002/retest/serial.log` and
`build/tests/umb-map-probe-20261002/multi/serial.log`. Both probes only
modified disposable FAT image copies.

Disjoint XMS allocations could give separate DOS VMs separate writable LFN
images without cloning the monitor's own page at `CD00h`. However, a child
VM's ordinary `VMFORK` return can release its allocation only on normal
exit. `VMM_KILL` in `src/vm/session_vmm.inc` destroys the target without
running its V86 cleanup code. The pinned JLOAD `VMM.ASM` service table has
null `Simulate_Int` and `Simulate_Far_Call` entries; CVSESSION has no safe
way to issue that target's XMS `AH=11h` from ring 0. Jemm's `UMB_free` and
`UMBsegments` in `src/UMB.ASM` are private implementation symbols, not
exported VMM services. Repeated forced closes would therefore retain global
UMB allocations. A fixed pool per VM slot would bound that reservation but
would still require explicit module-unload ownership and reserve most of the
small UMB supply while no DOS VM uses it. No high relocation was enabled
without a verified close, kill and unload lifecycle.
