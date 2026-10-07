# VMFORK low-memory reclamation

This note records the DOS allocation contract and the bounded root-PSP reclaim
for the full HDD image. The goal is to release the inactive desktop shell's
large conventional-memory block inside the VM fork without releasing the DOS
kernel, corrupting an interrupt vector, or leaving a live process with a
parent pointer into freed memory.

## Research and decision

Microsoft's archived MS-DOS system-call reference describes INT 21h/AH=48h as
allocating the requested DOS block and returning the largest available block
in BX when it fails. INT 21h/AH=4Ah resizes an allocated block; on a failed
growth, BX gives the maximum size. This matches the captured guest MCB chain:
the game's 256,000-byte request exceeded its largest 176,112-byte free block.
See [Microsoft's MS-DOS 2.0 SYSCALL.DOC](https://github.com/Microsoft/MS-DOS/blob/main/v2.0/bin/SYSCALL.DOC)
and the [source-format system-call reference](https://github.com/microsoft/MS-DOS/blob/main/v2.0/source/SYSCALL.txt).

The full build loads the resident CIUKIDOS runtime at segment 0300h, bounded
to 0A900h bytes and ending immediately below SYSVARS at 0D90h. Its fixed
SYSVARS, EXEC snapshots, boot buffers, and environment occupy the low-memory
layout through segment 1180h. `COM_LOAD_SEG` and the first user MCB are 1180h
and 117Fh respectively. `MZ_LOAD_LIMIT_SEG` (5200h in this profile) is the
primary MZ load window boundary, not the resident-kernel boundary. The kernel
source explicitly permits EXEC and transient DOS allocations in an otherwise
free low interval below that boundary after the parent shell has shrunk.

The captured guest's first MCB is 117Fh, owned by PSP 1180h. That PSP is the
resident `SHELL.COM`; its MCB spans 47F0h paragraphs and contains the parent
application PSP at 588Eh. The shell code at PSP+100h matches the shipped
`SHELL.COM`. The active VMFORK process can itself be embedded in that block, so
it must copy its PSP, COM image, and stack to an AH=48h allocation and rebase
before resizing the shell. It then moves only the recognized AUXSTACK and LFN
COM hooks while their source MCBs are still valid, verifies that no interrupt
vector still targets an ancestor-owned block, keeps the root PSP and its
256-byte process table, and releases only the block tail. Retained descendants
whose parent PSP lies in the released tail have both PSP parent fields
(offsets 16h and 3Ah) redirected to the kept root;
the original VMFORK PSP is freed if it still has a separate allocated block.
Invalid MCB provenance, unknown vector targets, failed relocation, or failed
old-PSP release aborts the child VM before it launches the requested program.

The memory reclaim must not impose a new 5801h floor. That segment is the MZ
copy-window limit, not the boundary of resident kernel code or DOS fixed data.
Nor may the old unconditional root-to-10h resize be used without validating
the DOS MCB, shell PSP, process chain, vectors, and VMFORK's own code/stack.

## Captured allocation evidence

The failure snapshot is a 512 MiB physical-memory file. Guest conventional
addresses were translated through the active CR3 (436000h) before reading the
MCB chain; direct physical reads show the master VM and are not the guest's
DOS arena. The guest MCB chain starts at 117Fh and ends at 9FC0h, matching the
reported 636 KiB conventional-memory boundary. Its largest free MCB is 2AFFh
paragraphs, or 176,112 bytes. The retained root shell block is 47F0h
paragraphs (294,656 bytes); DPMIRUN, HDPMI, secondary COMMAND.COM, and
DOOMCORE have distinct blocks above it. DOS4G's `I_AllocLow` requested
3E800h bytes (256,000) and received the exact largest-block value 2AFF0h.

In the first post-reclaim QEMU run, the rebuilt MCB chain had a free block at
1190h of 4AE5h paragraphs (306,768 bytes), but DOS/4GW still saw only the
trailing 2685h-paragraph block (157,776 bytes). The kernel's global largest-
block and allocation fallback started at the active PSP's end, hiding valid
free MCBs below that process. Microsoft's INT 21h/AH=48h contract reports the
largest contiguous DOS block in BX when an allocation fails, so both the
largest-block query and allocation fallback now scan free intervals beginning
at the lowest active PSP, bounded below by `COM_LOAD_SEG`. The scan validates
that PSP's preceding MCB, then protects each live PSP and tracked allocated
extent while finding gaps up to the active chain ceiling. Its occupied-end
segment is retained across gap-size selection so the next scan step cannot
skip or overlap a live block. Before carving a coalesced interval, only cached
free-table entries below the chain ceiling are discarded; chain rebuild then
emits the resulting MCBs. When no process PSP exists, the scan falls back to
the kernel's established `DOS_HEAP_USER_SEG` floor. An invalid first PSP MCB
fails closed. [Microsoft MS-DOS system-call reference](https://github.com/Microsoft/MS-DOS/blob/main/v2.0/bin/SYSCALL.DOC)

## Validation

The bounded `memory-gap-diagnostics` QEMU matrix passed at both 128 MiB and
256 MiB. In each run, `[MEMSTRAT] PASS first/best/last MCB placement` exercised
all three allocation strategies while the fixture occupied every other
conventional-memory gap with guards. This replaces the earlier fixture
assumption that guarding only the largest free interval was sufficient; the
probe now protects all remaining free gaps so it can detect scans that skip,
overlap, or consume a non-selected gap.

The serial measurements report a 484 KiB largest DOS block in the master
environment (`01E4h` KiB) and a 561 KiB block in the forked DOS environment
(`0231h` KiB), at both memory sizes. XMS allocation/free checks also passed.
These measurements provide bounded QEMU coverage for the normal memory
strategy and forked allocator path, in line with Microsoft's documented DOS
allocation behavior for INT 21h/AH=48h and AH=4Ah cited above. They do not
qualify physical hardware.

The direct full-HDD-profile NASM check produced a 43,261-byte resident image
against the `0A900h` (43,264-byte) kernel ceiling, leaving 3 bytes of headroom.
This is the complete kernel image size, not a standalone helper size. The
memory matrix and the separate DOOM wrapper/return test are QEMU evidence only;
neither claims that the physical T23's original DOOM stall is solved. The
wrapper test also does not qualify Windows runtime behavior.
