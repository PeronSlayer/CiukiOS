# T23 malformed scanline query recovery

The physical disk capture on 2026-10-08, disk sequence 58 and serial
H551120730, contains ten failed AUTO attempts in `SYSTEM/VIDEO/VBE.TRC`.
Every attempt passed descriptor and firmware mode/access readback, then
failed stage 6. The 1024x768x32 attempt has a post-query pitch of 512 bytes
and a validation failure value of 4096; 1024x768x16 has 256 versus 2048.
The other eight attempts have the same factor-of-eight discrepancy.
The trace holds the descriptor after the query overwrote its pitch, not
the untouched original 4F01 descriptor. GPU.LOG records zero bindings and
commands, so native BCI initialization did not execute in this failed boot.
The read-only panel probe correctly reports 1024x768.

## Sources and decision before implementation

[VBE 3.0 Function 06h, pages 48-49](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf)
specifies BX as the byte increment between logical scanlines, CX as complete
pixels per line, and DX as the available logical scanline count. Firmware
return status alone does not establish that these outputs are consistent.
[S3VBEFIX's source](https://github.com/wbcbz7/S3VBEFIX/blob/master/src/S3VBEFIX.ASM)
forwards BX/CX and only overrides DX when a fake memory size is configured;
the captured driver was launched without that option. The capture does not
prove whether the anomalous BX originated in the ROM or another BIOS path.

Validate the whole 4F06 tuple before committing any output: CX must equal
floor(BX / bytes-per-pixel), cover the visible width, and DX must cover the
visible height. BX * DX must fit the controller's reported usable VRAM.
Use division with a checked 16-bit quotient boundary to avoid an overflow
exception for large advertised controller memory sizes.

For a nonzero checked pitch P and TotalMemory M in 64 KiB blocks, capacity
is floor(M * 65536 / P) rows. If M >= P, capacity is at least 65536 and
every 16-bit DX row count fits. Otherwise, the high word M of the dividend
M:0 is less than P, so unsigned 16-bit DIV cannot overflow its quotient.
Unknown memory (M = 0) cannot authorize extra rows and retains the checked
visible-frame layout. A zero BX reply fails the visible-width check before
the capacity division.

An unavailable or inconsistent query retains the descriptor pitch already
validated for the selected access path and only one visible frame's rows.
It must neither invalidate an otherwise valid mode nor authorize extra
display pages. Do not multiply the malformed BX by eight: the specification
does not define that alternate unit. Valid padded and nonaligned strides
remain supported when their complete-pixel count and memory extent agree.

The resident shell has five bytes of payload slack before this correction.
Its normal build can share the existing VBE BIOS-call wrapper with
`shell_bios`: both preserve flags, general and segment registers, return AX,
and snapshot query outputs. COMMAND.COM retains its own wrapper because it
omits the desktop VBE code. This removes duplicate code without reducing
the stack, buffers, guest memory or COM arena limits. BIOS register semantics
are cross-checked against the
[Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

## Validation recorded on 2026-10-08

The [physical capture](../../build/full/t23-next/physical-logs-20261008-diskseq58/parsed.json)
contains ten failed protected-mode attempts and zero native GPU bindings,
self-tests or commands. Using the pre-fix production NASM instructions,
[six CPU reproductions](../../build/tests/vbe3-t23-stride-20261008/pre-fix-reproduction.json)
replay the recorded 512/256-byte query anomaly at 1024x768x32 and x16 for
local, banked and protected transports. All six fail stage 6 before binding;
the fixture hash is recorded in the report.

After the correction, the production-instruction
[metadata suite](../../build/tests/t23-stride-20261008/metadata/results.json)
passes 49 groups. The malformed T23 replies retain the checked 4096/2048-byte
descriptor pitches and 768 logical rows in all three transports. Valid
larger and nonaligned strides are accepted atomically; failed, short,
inconsistent and over-capacity replies leave the original layout intact.
The capacity cases cover BX = 0 without a divide trap, unknown TotalMemory,
4095 blocks rejecting 65535 rows at pitch 4096 (capacity 65520), and 4096
blocks accepting 65535 rows without division overflow (capacity 65536).
The existing [AUTO suite](../../build/tests/t23-stride-20261008/auto/result.json)
passes all 38 groups, including panel bounds, colour-depth ranking, retry
and retained-ownership quarantine. These CPU fixtures supply BIOS replies
and service boundaries; they execute the actual client selection and
startup instructions. Both suites ran in systemd scopes capped at 512 MiB
RAM and 128 MiB swap.

The [full HDD build log](../../build/full/t23-next/build-full-scanline-20261008.log)
ends with successful creation of `build/full/ciukios-full.img`. The resident
SHELL is 60909 bytes, leaving 19 bytes of payload slack. The normal SHELL
wrapper alias and COMMAND.COM's separate wrapper preserve the same register
and flag contract; checked query consumers read their snapshots before a
subsequent BIOS call.

The [pristine AUTO QEMU gate](../../build/full/t23-next/qemu-scanline-auto-20261008/report.json)
passes with a Pentium III CPU profile, 128 MiB RAM, EDID disabled, standard
VGA and AC97. It uses the built image without guest binary overrides, checks
its freshly assembled SHELL/SFX bytes, and leaves the source image unchanged.
The image SHA-256 is
`27da9d223e14f6b9210f496c2a61f78531fa8141d23f624f530f775ac076f513`.
AUTO reaches 1024x768x32 with a 4096-byte pitch, matching 4F03 linear-mode
readback and the protected renderer (`vc_lfb = 2`). The log records 207
protected row transfers and no local-LFB or bank-window path. Ciuk1's 36
sampled RGB values match exactly before and after returning from DOS.
The native ICH startup path reads the complete track larger than 64 KiB,
releases its audio ownership, and produces a changing PCM waveform. HELP,
the system-shell SAV3D client, and an F4 shell with typed input all return
to the same desktop. SAV3D correctly reports unsupported hardware in this
QEMU profile; this result does not qualify SuperSavage acceleration.

The [private-image fault-injection gate](../../build/full/t23-next/qemu-scanline-fault-20261008/report.json)
passes with a test-only INT 10h TSR installed after S3VBEFIX. It observed two
successful 4F06 calls and changed the returned BX from 5120 to 640 bytes per
scanline. AUTO retained the independently validated 5120-byte descriptor
pitch, and read-only SHELL RAM showed `vc_scan_lines = 800` and
`ui_page_enabled = 0`. All 36 sampled Ciuk1 RGB values matched before F4 and
after returning from DOS. The report confirms the pristine source image
remained unchanged; only the private test copy contained the TSR and config
entry. This demonstrates recovery from a deliberately malformed query reply
in QEMU, not physical native-engine operation or the cause of the earlier
T23 VGA fallback strip. The corrected production image still needs physical
T23 validation; fresh DISPLAY.LOG, VBE.TRC and GPU.LOG are required after its
next boot.

The [VBE aperture runtime gate](../../build/full/t23-next/qemu-aperture-scanline-20261008/results.json)
passes all three full-HDD cases at 1024x768: standard VGA and Cirrus with
the protected session (`vc_lfb = 2`, bound), and standard VGA without the
monitor (`vc_lfb = 1`, unbound). Read-only RAM snapshots confirm the expected
active renderer and geometry. Moving the Run window across scanline 512
changes 73253 pixels below that line in each case; its title and frame
remain present, and the taskbar's top rule spans all 1024 pixels. The gate
uses the same freshly assembled SHELL hash as the AUTO test, performs no
guest memory writes and leaves the source image unchanged.

The [full CD build](../../build/full/t23-next/build-full-cd-scanline-20261008.log)
completes the canonical GRUB4DOS RAM-boot ISO and the uncompressed recovery
and direct-ATAPI diagnostic variants. The
[CD-only runtime gate](../../build/full/t23-next/qemu-cd-scanline-20261008/report.json)
passes from the read-only CD ISO with no hard-disk device and 512 MiB RAM.
It verifies the freshly assembled SHELL, WALLP application, Ciuk1 asset,
six-entry wallpaper catalog and default configuration against the CD
partition. The desktop module loads, startup About opens and closes with
Escape, and the 1280x800 desktop renders Ciuk1 in Fill mode with all 36 RGB
samples matching exactly. The tested ISO SHA-256 is
`aad99595eb68f7d4c9e8965b258d30106d6880c2dfbad216286ebde9220a0f81`.
These aperture and CD results are emulator evidence; physical T23
qualification remains pending.

The [Display Properties mode gate](../../build/full/t23-next/qemu-properties-scanline-20261008/provenance.json)
passes on standard and VirtIO VGA profiles. Real pointer/key input previews
640x480, rolls back to 1280x800, then keeps 640x480 and cold-boots into that
saved mode. Ciuk1 is resampled at each geometry and its selected style is
preserved; the 1280x800 samples match exactly, and the 640x480 comparisons
remain within the gate's expected colour quantization tolerance. The source
image is unchanged, guest binaries are not overridden, and RAM observation
is read-only. These are emulator results.

The [DOOM window gate](../../build/full/t23-next/qemu-doom-scanline-20261008/report.json)
passes from the normal Run dialog on the unmodified production HDD copy.
The actual first level and HUD are visible in the recorded screenshot.
Startup takes 57.206 seconds in this capped QEMU case; the pointer observation
is 0.163 seconds on the desktop and 0.382 seconds while the game renders,
below the existing 0.5-second gate. This does not measure T23 game performance.
The original HDD SHA-256 remains the value recorded above after these tests.

## Delivery and remaining physical validation

Normal publication rebuilds the full HDD and sanitized Windows bundle from
the clean commit through the existing pre-push hook, then runs its Linux
boot smoke. The final disk write keeps a 128 MiB backup, checks the physical
disk identity/sequence and performs direct-I/O hash readback; delivery and
release hashes are recorded in the local build reports. Windows archive
checks do not constitute a Windows runtime test.

The corrected production image still requires a fresh boot on the T23.
The captured failure proves that the native engine never initialized;
these emulator gates do not qualify its physical 2D or 3D acceleration.
Fresh DISPLAY.LOG, VBE.TRC and GPU.LOG must show the selected mode, validated
pitch, successful session binding and native engine results on that boot.
