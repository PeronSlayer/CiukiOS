# f1-95: SuperSavage BCI self-test at 32 bpp (`4a4a4a4a`)

Written 2026-10-22. Host-tested candidate; T23 acceptance remains open.
Supersedes the f1-69 reading of the sweep (configuration 0 versus 1) and
classifies the f1-88 preamble-depth hypothesis as unsupported.

## Evidence

T23 image b453bc92 (serial `build/codex/t23-b453bc92-serial.txt`, lines
226–289; the lead's copy is `build/lead-scratch/t23-b453bc92-serial.txt`):

```
[accel] savage descriptor wrote=10200401 read=10200401 mask=13ff1ffd cr40=01
[accel] savage bci_test phase=fill x=1023 y=767 wrote=0042b94a before=00181c10 cmd=4bcc8000
[accel] savage bci_test result=failed error=-5 wrote=0042b94a read=4a4a4a4a read_valid=1 restored=1 status=07e00000 status_valid=1
[accel] savage depth firmware_cr50=30 bpp=32 descriptor=10200401 order=cr50_before_gbd
[accel] savage completion reg=48c64 before=00000001 requested=0002 observed=00000002 pending=0 status=07e00000 idle_bits=7 fifo_used=0
```

The fill executed and completed (exact event tag, idle) at the right
address; only its colour lanes are wrong. `firmware_cr50=30` means CR50
already had pixel length `11` (32 bpp, RE046-B p. 82) before any CiukiOS
write, so the f1-88 hypothesis (old depth captured at the PM preamble) is
unsupported, as its protocol step 3 anticipated.

Every production T23 self-test so far failed the same way, through four
different setups. Each was the single fill, i.e. the first colour RECT
after the CR66 engine reset:

| Image | Setup at the time | Marker → read |
| --- | --- | --- |
| 445e91c7 (f4-22) | `DEST_PBD_NEW`, descriptor inline in the command | `004a8166` → `66666666` |
| ce2e6ce9 (f4-26) | as above, CR50 `f1`, GBD `10200401` read back | `004a8166` → `66666666` |
| a738204a (f1-65) | `DEST_GBD` fill `4bcc8000`, GBD `10200401` | `008641b6` → `b6b6b6b6` |
| b51f034f (f1-88) | plus two queued E0/E1 pairs, completion fence | `00cd00e8` → `e8e8e8e8` |
| b453bc92 (f1-95) | plus CR50 before the PM preamble | `0042b94a` → `4a4a4a4a` |

The pre-f4-26 command path is in `git show d8b4c006^:src/kernel/drivers/savage2d.c`
(`fill_rect` sends `RECT|XP|YP|DEST_NEW|COLOR`, offset, descriptor).

The a395ffa8 hardware-check sweep (`legacy/local/physical/a395ffa8/disk-BOOT2.LOG`,
lines 198–270) ran 72 fills in one engine session, in configuration order:

- configuration 0 (the first fill): `008641b6` → `b6b6b6b6`;
- configuration 8 has the **same descriptor** (`10200409`) as 0, with CR50
  depth `00`, and read back exactly; so did 9–11 and 16–19 (CR50 `00`/`01`);
- every later linear or tile-2 pixel-stride candidate (bit 3 on or off,
  BW_DISABLE on or off) read back exactly: 23 of 23;
- qword-stride and tile-1 candidates left the probe pixel untouched.

Neither the descriptor, CR50, bit 3, BW_DISABLE nor the destination path
separates passing from failing fills. The one consistent discriminator is
the position: the first colour RECT after the engine reset repeats the
colour's low byte over the pixel; later fills with the same descriptor are
exact. f1-69's reading (bit 3 plus BW_DISABLE fails) is contradicted by
configuration 8. This is an inference from measurements. No available
primary source documents such a first-command state.

Related upstream facts, none of which states this behaviour:

- savagefb sends the GBD twice because one copy "fixes the initial black
  screen ... in Toshiba laptops with SavageIX" (`savagefb_driver.c:384-387`),
  then issues a BCI `NOP|CLIP_NEW` command before drawing
  (`savagefb_set_clip`, lines 398–409, called from `savagefb_set_par`, 1532).
- RE046-B p. 143: writes to the BEE8H 2D registers are pipelined; reading
  one back requires issuing an engine command and waiting for idle.
- X.Org EXA refuses 32-bpp solid fills ("HW seems to ignore alpha",
  [savage_exa.c:253](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_exa.c));
  X.Org XAA replaces a colour-0 copy fill with `GXclear`
  ([savage_xaa.c:542-548](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c)).
  A first fill of colour 0 is invariant under low-byte repetition, so a
  console clear would not show the effect.

## Descriptor, colour and pixel format (item 1)

Checked against xf86-video-savage (local copies in `build/downloads/savage/`,
upstream [savage_accel.c](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c))
and the S3 Savage4 register manual RE046-B
([PDF](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf)):

| Field | Upstream source | RE046-B | CiukiOS (1024x768x32, pitch 4096) |
| --- | --- | --- | --- |
| GBD selection for SuperSavage | `SavageSetGBD` → `SavageSetGBD_PM` (`savage_accel.c:319-320`) | — | PM path |
| Preamble | `bci_enable \| S3_BD64 \| 0x10000000` (`:1030`), `BCI_ENABLE_TWISTER=0`, `S3_BD64=1` (`savage_bci.h:28,32`) | — | `10000001` |
| CR50 | `\|= 0xC1` use GBD (`:1034`) | p. 82: bits 7-6,0 `111` GBD, bits 5-4 `11` 32 bpp | `f1` |
| Stride | `HighPart.Stride = lDelta / Bpp` (`:1066`), struct `HIGH {Stride; Bpp; ResBWTile}` (`savage_driver.h:260-264`) | p. 148: bits 12-4, pixels in units of 16 | `0x400` (1024 pixels) |
| Colour depth | `HighPart.Bpp = bitsPerPixel` (`:1067`) | p. 148: bits 23-16 bits/pixel | `0x20` |
| Tile, block write | `ResBWTile = TILE_FORMAT_LINEAR` then `\|= 0x10` (`:1043,1064`) | p. 148: bits 25-24 `00` linear, bit 28 BW disable | `0`, bit 28 set |
| Size, endian, BCI enable | `HiPart \| bci_enable \| S3_LITTLE_ENDIAN \| S3_BD64` (`:1082`) | p. 148: bit 0 BDS 64-bit, bit 2 little endian, bit 3 Savage4 BCI enable | bit 0 set, bits 2-3 clear |
| GBD offset | `S3_GLB_BD_LOW = LoPart` = `fbOffset` (`:1081`) | p. 148: byte offset | `0` |
| Fill packet | `savagefb_fillrect`: `RECT\|XP\|YP\|DEST_GBD\|SRC_SOLID\|SEND_COLOR`, ROP `CC`, colour, XY, WH (`savagefb_accel.c:65-90`) | — | `4bcc8000`, colour, XY, WH |
| Copy packet | `savagefb_copyarea`: `RECT\|DEST_GBD\|SRC_GBD`, ROP `CC`, XP/YP by direction (`savagefb_accel.c:31-63`) | — | `4bcc0020` forward |

Result: `10200401` is X.Org's PM descriptor bit for bit, the colour operand
is the full pixel value as savagefb sends it, and CR50 selects 32 bpp. No
descriptor, depth or command-encoding change is justified; the host test
`f1_95_first_fill_after_reset` derives the X.Org value independently and
compares it with `savage2d_config(1, …)` for 32 and 16 bpp.

## Change

`bci_test` in `src/kernel/drivers/savage2d.c` now uses an 8x2 block in the
bottom-right corner (x 1016–1023, y 766–767 at 1024x768), inside the same
presenter/console exclusion, without serial output or yields:

1. save the 16 pixels;
2. prime: fill the whole block with `00000000` (byte-uniform: the same
   result whether or not the low byte is repeated); all 16 must read 0;
3. marker fill: right 4x2 half (contains the historical pixel 1023,767)
   with `before ^ 5aa55a`; a byte-uniform marker is altered so it can never
   hide the repetition. Right half must equal the marker, left half 0;
4. copy: right half to left half (forward GBD BitBLT `4bcc0020`); all 16
   must equal the marker;
5. restore the 16 pixels after proven idle; any mismatch quarantines with
   `bci_test_readback_failed` as before.

No register value, register write or command format is added; the prime is
the existing fill packet. Every activation passes through this test after
its CR66 reset (mode changes stop acceleration and reschedule the attempt:
`fbdev_change_begin`/`fbdev_change_end`), so the prime precedes the first
desktop fill after each reset. The H-page sweep is unchanged; its first
candidate remains the unprimed first fill, which explains its configuration-0
failure.

Not adopted: savagefb's `NOP|CLIP_NEW` (our commands use `CLIP_NONE`; whether
a NOP clears the measured state is unknown; next candidate if the prime fails);
MONO_PAT_0/1 writes (patterns are sent inline); X.Org's PBD/SBD writes (GBD
only; the inline-descriptor path failed on 445e91c7/ce2e6ce9); removing the
CR66 reset (X.Org does not reset at init, but the reset is the established
recovery step and no evidence implicates it alone).

## Host evidence

`tests/host/savage2d_test.c`, register model: new flag `first_rect_8bpp`
(armed by the CR66 reset, consumed by the next RECT; labelled as measured
behaviour, not a documented erratum), `colour_8bpp`, `ignore_copy`, and a log
of submitted RECT packets. Test `f1_95_first_fill_after_reset`:

- X.Org PM encoding equals the production descriptor (`10200401`/`10100401`),
  RE046-B fields decoded, preamble `10000001`;
- old sequence on the T23 state (CR50 `30`, CR66 `89`, MM48C18 `8`, status
  `07e00000`): start, then one fill of `0042b94a` at 1023,767 → completed,
  tag 2, status `07e00000`, read `4a4a4a4a`; the same fill again → exact;
- production activation on the same model, 32 and 16 bpp: the 27 setup
  records match the T23 trace (bank, register, width, value; serial lines
  232–262), then event tags 2–4; prime, marker fill and copy packets exact;
  `result=ok`, caps nonzero, 16 pixels restored; later desktop fill and copy
  exact;
- safety: an engine that stays at 8 bpp (`phase=fill ... read=4a4a4a4a`), a
  dropped copy (`phase=copy`) and a partial fill (`phase=prime`) still
  quarantine with the block restored;
- the marker is never byte-uniform.

Existing tests were updated only for the three-command self-test (packet,
operation, status-read, delay and event counts; one expected readback that
the prime now catches first).

## Expected T23 lines (next image)

1024x768x32 on the T23 with the cursor page mapped (`caps=3`: 2D plus
cursor). `<P>` is the incoming pixel at 1023,767 and `<M>` its marker
(`<P> ^ 005aa55a`, low 24 bits). Order as emitted:

```
[accel] savage descriptor wrote=10200401 read=10200401 mask=13ff1ffd cr40=01
[accel] savage bci_test phase=prime x=1016 y=766 w=8 h=2 wrote=00000000 cmd=4bcc8000
[accel] savage bci_test phase=fill x=1023 y=767 wrote=<M> before=<P> cmd=4bcc8000 block=1020,766+4x2
[accel] savage bci_test phase=copy src=1020,766 dst=1016,766 w=4 h=2 cmd=4bcc0020
[accel] savage bci_test result=ok error=0 wrote=<M> read=<M> read_valid=1 restored=1 status=07e00000 status_valid=1 phase=done pixels=16
[accel] savage pci=5333:8c2e mmio=c0100000 lfb=e8000000 mode=280 bpp=32 pitch=4096 caps=3 init_result=0
[accel] savage depth firmware_cr50=30 bpp=32 descriptor=10200401 order=cr50_before_gbd
[accel] savage completion reg=48c64 before=00000003 requested=0004 observed=00000004 pending=0 status=07e00000 idle_bits=7 fifo_used=0
[accel] savage init reg=...   (the 27 records of b453bc92 lines 232-262, unchanged)
[accel] savage init reg=48c64 wrote=00000002 read=00000002 bank=event width=32
[accel] savage init reg=48c64 wrote=00000003 read=00000003 bank=event width=32
[accel] savage init reg=48c64 wrote=00000004 read=00000004 bank=event width=32
[accel] savage ready us=... reset_us=... poll_us=... trace_us=... status=07e00000 status_valid=1
[accel] savage phase=marker_clear us=... error=0
```

No `bci_test mismatch` line, no `quarantined` line.

Reading a failure:

- `mismatch phase=fill x=1020 y=766 read=<low byte of M repeated>` after a
  passing prime refutes the first-RECT explanation: keep the quarantine and
  try savagefb's `NOP|CLIP_NEW` before the prime, or an H sweep whose first
  candidate is primed;
- `mismatch phase=prime` with a nonzero read: the prime itself was not
  written as colour 0 (check the read value against the original pixels);
- `mismatch phase=copy`: fills are exact but the GBD BitBLT is not; the
  record names the first wrong pixel.

After a pass, check window moves and fills on the desktop (fill/copy/pattern
correctness and `[accel] driver=savage ops=` growing with `L`), the hardware
cursor, full-screen enter/exit (re-activation repeats the self-test) and
the `accel=0` safe boot.

The lead owns review, image build, hardware trial and diary.
