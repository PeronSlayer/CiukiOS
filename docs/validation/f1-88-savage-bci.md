# f1-88: SuperSavage BCI completion candidate

Status: host implementation candidate; T23 root cause and hardware acceptance
remain open. Research checked online and against `build/downloads/savage/` on
2026-10-21. The directive in this checkout has no appended amendment or lead
note; its abbreviated b51f034f records do not include the pixel readback or
register block. Request those before attributing a silicon failure.

## Source comparison and decision

| Our step before f1-88 | Primary reference | Difference and consequence |
| --- | --- | --- |
| Select IGA1, unlock ports, CR66.1 assert/deassert with 10 ms delays, preserve CR66.0 | X.Org `SavagePreInit` and `SavageGEReset` [driver](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c) | Reset polarity and delays agree. The reported 177346 us includes scheduling/readback transport; duration alone does not establish disabled hardware. |
| Poll MM48C60 with `e1ffff/e00000` | Same driver's `SavageInitStatus`, `WaitIdleEmpty4`, `WaitQueue4` | Correct family and address. Queue polling uses a separate 21-bit used-count mask `1fffff`; it is not a free-entry field. |
| Disable MM48C18, thresholds `00700040`, shadow address zero, enable bit 3 only | Linux `SavageSetup2DEngine` [fbdev driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c) | Matches the no-COB path. `savage_init_hw` discovers RAM; `savage_init_fb_info` selects family callbacks; `savagefb_set_par` commits mode state before acceleration setup. |
| Reset CR66.1 only; keep firmware scanout | Linux `savage_init_hw` also toggles CR3F.3 after CR66 reset | This broader reset includes the memory interface, AGP/PCI masters, 3D and master engine. X.Org's pre-init/2D initializer does not require it. Do not import a whole-device initialization reset into a live firmware-mode engine attachment without evidence. |
| MM48C10=`00700040` | X.Org `SavageInitialize2DEngine` with `DisableCOB` | X.Org's high=32/low=0 thresholds encode as `00000001`. CiukiOS follows fbdev; threshold-driven shadow updates are disabled by MM48C18.1=0 and MM48C0C.0=0, so this difference does not explain an idle marker failure. |
| GBD `10200401` at pitch 4096/32 bpp, bit 3 clear, BW_DISABLE, CR50=f1 | X.Org `SavageSetGBD_PM` [acceleration source](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c) | Agrees with PM's linear pixel stride 1024 and SuperSavage bit-3 policy. The PM routine also programs scanout; transplanting it would exceed this directive. |
| MMIO GBD plus two E0/E1 command pairs at MM10000 | Linux setup above; local `savagefb.h` and X.Org `savage_bci.h` | Linux sends the pair twice through BCI; its generic descriptor sets bit 3, unlike PM. Keep the measured configuration 1 and both transports. |
| Empty/idle immediately after command writes, then CPU pixel read | X.Org `ShadowWait` above; Linux DRM `savage_bci_emit_event`/`savage_bci_wait_event_reg` [BCI source](https://kernel.googlesource.com/pub/scm/linux/kernel/git/gregkh/driver-core/+/7cfde0af731c14664e3882c7ba77ace1059f2c5e/drivers/gpu/drm/savage/savage_bci.c) | Missing command-specific acknowledgement. Candidate: append WAIT_2D then a fresh nonzero event; require exact MM48C64 tag equality and idle before CPU access. |

The X.Org queue limit `MAXFIFO=7f00` covers its COB configuration. Its
`DisableCOB` branch has zero COB bytes and threshold 32; fbdev uses no COB for
SuperSavage. CiukiOS submits at most eight setup or seven drawing DWORDs,
plus two fence words, from proven idle, within the 32-entry on-chip budget.
No VRAM overflow buffer, DMA or shadow status storage is introduced. X.Org's
`ShadowStatus` substitutes memory status/tag polling; direct tag polling in
the DRM source works without it. DRM reads MM48C04; RE046-B defines MM48C64
as its alternate alias, used here beside the family's MM48C60 status word.
BIOS tag zero is deliberately not accepted
as command completion here: there is one owner and no concurrent mode set.

[S3 RE046-B](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
printed pp. 131–133, 224–227 and 231, separates subsystem interrupt/reset
controls MM8504 and buffer status MM8508 from BCI enable MM48C18. Neither
named initializer enables BCI at 8504/8508. It puts event tags in status word
1 (MM48C04/alternate MM48C64), not word 0. This is a Savage4 manual;
SuperSavage family choices come from the chip-specific upstream branches.
MM850C.0 aliases CR66.0 for engine enable and requires blank/retrace when
changed (printed p. 134); preserve the already enabled firmware state.

`07e00000 & 00e1ffff = 00e00000`: all three upstream idle bits (23:21)
are set, and `07e00000 & 001fffff = 0`: zero used entries. With COB disabled,
this corresponds to the 32-entry on-chip capacity being available; `7f00`
is not the physical no-COB FIFO size. Bits 26:24 are outside these masks.
There is **no event tag to decode from this word**. Applying RE046-B's
Savage4 alternate layout literally would instead report page-flip pending
and different idle bits; it cannot replace the SuperSavage driver's layout.
The old adaptive manual-layout fallback is retained, not newly qualified.

The existing `phase=marker_write error=0` confirms the durable attempt file,
not a BCI write. `bci_test_readback_failed` means the old idle check passed
but the saved pixel differed. Without the actual read value, stale completion,
wrong colour depth and a disabled command path remain distinguishable
hypotheses. In particular, f1-65 recorded `read=b6b6b6b6`, rather than an
unchanged saved pixel; its measured depth failure must not be relabelled as
stale completion. The f1-69 sweep selected configuration 1, already present
in this boot. The new abbreviated log omits the pixel value.

The host transport injects a 10 us parser acceptance delay independently
of which register is read, while word 0 initially reports idle. The old
sequence reads unchanged pixels and quarantines; advancing the model clock
then executes the command without any event-register read. WAIT_2D followed
by event publication lets the new sequence wait for that command. This is
a conservative scheduling hypothesis, not a documented chip erratum or a
measured T23 latency. RE046-B says direct status reads are exact; neither
that manual nor the upstream initializers proves this delay exists on the
T23. Model acceptance supports the candidate's ordering, not attribution of
the physical failure. Repeated-low-byte readback would refute that model as
the sole explanation.

Decision before implementation: keep reset, descriptor, quarantine and mode
programming policies; add a bounded SuperSavage completion fence and its
diagnostics. The exact pixel test remains the capability gate. A fence
failure retains an unsafe-writer guard even if word 0 says idle. No retry,
reset after submission, or automatic quarantine clearing is permitted.
These restrictions describe production activation/runtime; the existing H
diagnostic retains its separate bounded reset/recovery protocol and status
polling. Its host transport now understands the fenced production setup.

## T23 experiment protocol (lead)

1. Record canonical image SHA-256, cold/warm boot, display geometry, and
   whether the attempt marker was already retained. Preserve the original
   logs. A retained marker yields `previous_attempt` and no new evidence.
2. Safe boot: set the existing standalone `accel=0` token in `BOOT.CFG` on
   the boot volume. Verify `reason=boot_disabled`, zero caps and no engine
   initialization records. Keep this recovery option available. For an
   explicitly authorized new attempt, remove `accel=0`; clear the retained
   `C:/system/settings/accel.cfg` only by the established recovery procedure.
3. Capture every `savage init ... bank=port/cr/sr/mmio/bci` record in order.
   Check SR26=40, CR66 reset asserted then cleared with enable bit preserved,
   CR40=01, CR31=0c, CR50=f1 for 32 bpp; MM48C18 low nibble 0 then 8,
   MM48C0C=0, GBD `10200401`, and both queued descriptor readbacks.
4. Read `savage completion` records: register 48c64, before/requested/observed
   tags, pending flag and idle status decode. Requested must be nonzero,
   differ from the sampled old tag, and match the low 16 observed bits.
   Require both completed setup and marker fences. Word 0 alone is
   insufficient. Dead/stale/zero/wrong tags must quarantine within the bound.
5. Require `bci_test result=ok error=0`, exact wrote/read equality,
   `read_valid=1 restored=1`, nonzero caps, `init_result=0`, `marker_clear`
   success and `savage ready`. On failure collect the full `savage readback`
   block, including whether it is `idle_snapshot` or `cached_init`, and
   quarantine timings. Cached records must not be mistaken for live reads.
6. Classify: completed event + repeated-low-byte pixel refutes a mere stale
   completion explanation and points back to depth/descriptor handling;
   unchanged pixel with completed event needs command/address investigation;
   idle word 0 + missing event implicates command acceptance/completion, not
   the status address. A passing boot supports the candidate on that image
   only. Then check cursor, fills, overlap copies, window moves with music,
   engine ops/s and presentation costs. Host tests establish no CPU/audio
   improvement on hardware.

`supersavage_mode.c` saves/restores CR40/50/66 and rejects an asserted reset;
its native scanout transaction runs after acceleration handover. Neither it
nor `supersavage_display.c` submits BCI commands. Their programming remains
unchanged and their host tests must pass. Lead review and the trial remain
necessary before this candidate can be called the T23 root-cause fix.
