# T23 graphics recovery and native engine integration

The 2026-10-07 read-only capture (disk sequence 57, serial H551120730) proves
that S3VBEFIX installed INT10 and native ICH3 AC97 fetched all 245760 startup
bytes. The display remains VGA mode12h. SYSTEM/DISPLAY.LOG records valid
banked/linear descriptors for modes013E,0117 and0173, but their previews fail
and roll back to0012. The single CVB1 file reflects the last rollback, so its
failure stage must not be mistaken for the failure of every initial candidate.

The runtime will select a real BIOS LFB mode (4F02 D14 and matching4F03), map
it in the existing CVSESSION monitor, and route every pixel operation through
the protected mapping. This keeps local CR0 transitions outside V86 and does
not add S3 to the QEMU-only banked/LFB alias list. See [VBE3](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf)
and [the protected transport design](protected-desktop-lfb.md).

Native GPU commands are a separate backend, based on the researched upstream
Savage register protocol. Firmware establishes scanout timings. Engine-private
VRAM must fit the firmware's usable extent (237×64KiB on the captured T23),
even if CR36 reports16MiB physically installed. Preserve the firmware's
reserved upper VRAM, including command-overflow and shadow areas. See
[upstream research](savage-upstream-research.md).

The resident SHELL has only149 payload bytes available before this change.
Move its existing compressed help text to SYSTEM/HELP.RLE, keeping the same
printer and output with a512-byte streaming buffer already owned by the shell.
COMMAND.COM retains its embedded text. Full HDD/CD builds install the resource.
Open/read/close use the existing DOS contracts, cross-checked against the
[FreeDOS INT21 implementation](https://github.com/FDOS/kernel/blob/master/kernel/inthndlr.c).
This frees resident code space for protected transport without shrinking the
runtime stack or changing guest memory ceilings. A missing resource reports
the existing file error. Read failure or premature EOF closes the opened handle.

Setup retains its existing real-mode LFB/banked transport. Its COM arena is
already almost full, so it omits the newly added protected-LFB discovery/fill
routines through `VC_PROTECTED_LFB=0`; its stack and arena limits are unchanged.

Every SHELL mode attempt writes a 272-byte `CVT1` record to
`SYSTEM/VIDEO/VBE.TRC`, capped at 128 records per boot. Each record preserves
the 256-byte ModeInfo as validated at that stage (some pitch and mask fields
may have been normalized), raw 4F01 AX, mode ID, stage, result and selected access
path before fallback can overwrite them. Stage7 is protected framebuffer bind.
The16-byte header's internal failure-status word is meaningful on failed
records only; it is not always a BIOS error code. The first desktop paint also
captures the192-byte runtime driver snapshot in `SYSTEM/VIDEO/GPU.LOG`.
`SAV3D` independently records a hardware triangle and before/after counters.

Startup sound initialization remains before video initialization, but pending
playback waits until the first completed desktop frame. The original photo,
startup track, stack allocation and DOS memory limits remain unchanged.

AUTO first uses a verified preferred EDID timing. If it is unavailable, the
protected SuperSavage driver reads the active LCD's native dimensions using
the [upstream panel-register report](t23-panel-detection-research.md). This
distinguishes1024×768 and1400×1050 panels without guessing from the PCI ID.
Only stable, plausible reads from the identified active LCD are accepted.
AUTO chooses the highest already validated VBE mode within that ceiling,
preferring the greatest color depth at equal area. Unknown displays keep the
existing1024×768 fallback. Display Properties reports observed dimensions;
it no longer infers a14.1-inch XGA panel or60Hz refresh from the GPU ID.

The first final Doom cursor gate failed because its harness submitted Enter
twice: `VM.text()` already submits the Run command, and the extra key reached
the new DOS window's focused Close control before Doom initialization. The
serial sequence showed `WINDOW 11 CLOSE` and `DOSVM closed`, with no guest
exit marker. Removing the duplicate key fixes the launch sequence without a
production change. The corrected QEMU rerun passed: Doom emitted `ST_Init`
and `startmap: 1`; measured desktop pointer response was 0.181 s before launch
and 0.199 s during the running level. This is QEMU evidence, not a T23 speed
measurement. Future runs also record elapsed Doom startup time separately.
Native CAPP DOSVM tests observe the current VM-backed window lifecycle rather
than the former `dos_host_active` execution path. Failure reports now preserve
the normalized serial tail for diagnosis.
