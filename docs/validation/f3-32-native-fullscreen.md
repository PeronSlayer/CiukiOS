# F3-32 native full-screen provider: F3-36 and F3-39 amendments

Research and host validation: 2026-10-16. This record accompanies the
[provider design amendment](../design/supersavage-fullscreen.md#f3-36-restore-correction-2026-10-16).
There was no earlier F3-32 file in `docs/validation/` in this worktree.

## Physical evidence supplied to the implementer

T23 image a79f840c, third boot, serial log from line 6668: Quake native
entry and first aperture-write frame complete. The owner's visible entry
assessment is positive. The watchdog return then records native restoration
failure: `reg=158 expected=00000000 actual=00000008 mask=000000ff`,
`restore_us=156307`, `lease_return_ms=1180 budget_ms=200 result=-5`.
The desktop continues composing with zero completed presents.
This evidence belongs to the old image, not the amended sources.

## Source comparison and decision

- `sw()` encodes SR indices with 100h. The error is **SR58**, not CR67 or
  MM8158. The recorded stage used to drift to the last recovery checkpoint;
  capture the first mismatch's stage and register space explicitly.
- [S3 RE046-B, printed pp. 58–60](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf#page=62)
  describes SR58/59/5A as read-only panel border/expansion results and SR5B
  bits 7:1 as derived status. The SuperSavage application of this shared
  layout is supported by the corresponding panel names in X.Org's
  [savage_regs.h](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_regs.h).
  Keep these saved for diagnostics; skip writes/comparison for SR58–5A
  and compare only SR5B bit 0. Keep writable register readback strict.
- X.Org's [SavageWriteMode](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
  restores the mobile panel block without asserting readback equality.
  [SavageStreamsOff/On](https://github.com/NetBSD/xsrc/blob/trunk/external/mit/xf86-video-savage/dist/src/savage_streams.c)
  operates CR67 enables, independently of the SR58 panel result. Linux's
  [savagefb_set_par_int](https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/savage/savagefb_driver.c)
  also disables streams before timing restoration and enables them last.
  Preserve that order. No MM8158 programming or new register recipe is added.
- The supplied IBM `Driver/D/A0400/SSAV1014.INF` identifies
  5333:8c2e/01fc:1014 and panel preferences; it supplies no bit recipe.
- Failed scanout proof must not retain the provider's CPU access leases:
  revoke aliases, quiesce/release the synchronous provider, retain the RAM
  snapshot and disable further native entry. Then attempt one mediated VBE
  switch for the saved desktop mode, decode/validate it and remap only on
  success. Registry/firmware quarantine is never reset or bypassed. Record
  the native result, fallback attempt/result and final result separately.
- The supplied 1180 ms complete return disproves the original 200 ms target.
  Register restoration took 156307 us. Keep the 200 ms register target and
  report complete lease/first-frame budgets of 2000 ms native, 4000 ms with
  the existing 2000 ms VBE transaction. Targets are diagnostic, not cancellation
  deadlines; physical timing qualification remains outstanding.

## Host coverage

The production register model checks retained SR58 values 08h, 80h and FFh,
derived SR59/5A/5B changes, no write to read-only panel registers, and a lost
writable SR54 write that still returns EIO with its first-failure stage.
Provider tests prove revoked aliases and released leases before a subsequent
firmware claimant, retained failure snapshot, blocked native retry, and
unchanged quarantine on registry activation failure.

Display service tests cover successful and failed/invalid VBE fallback,
partial-entry rollback, both results, grant return, framebuffer writes and
completion of the first returned frame. Production VM tests check clean and
watchdog `return-or-fault` ordering: guest ports/PTEs revoked before capture,
one desktop restoration, exact original PTEs, owner/hold cleanup and exposed
restore errors. The compositor test retains damage while presentation fails
and completes a frame after its transport becomes available again. These
are layered host tests, not proof of physical panel scanout.

Commands and exact results are in `build/codex/f3-36-report.md`; full local
logs are adjacent to that report. No QEMU or hardware validation is claimed.

## Qualification remaining with the lead and owner

Run `vm-fullscreen` on the canonical integrated QEMU image; retain the known
standard-VGA readback failure tracked by f3-29 as a separate finding. On the
T23, repeat Quake and DOOM entry/Alt+Enter/Alt+Tab, clean guest exit and
watchdog/fault return. Verify the desktop visibly repaints and accepts input,
native/fallback results and resource cleanup are recorded, writable readback
remains strict, and `restore_us`, `lease_return_ms`, `desktop_frame_ms` meet
their reported targets. Successful native return must issue no VBE call.
If fallback is needed, require VBE success and completed desktop presents;
if it also fails or the firmware is quarantined, preserve the failure rather
than report recovery or map an unverified framebuffer.

## F3-39 research and implementation decision

The per-boot `poisoned` flag conflates failed scanout proof with retained
resource ownership. F3-39 supersedes the F3-36 boot-long refusal described
above. Replace it with three strikes per boot, at most one per
session: record an entry failure immediately, or a restore failure after
the display service confirms recovery. Discard a failed RAM snapshot only
after that confirmation and release of its CPU leases. Retained/quarantined
claims continue to block entry; observing their release (or a replacement
generation) permits retry without resetting quarantine. A recovered entry
refusal has an explicit windowed display result; the VM resumes its private
video state without reporting EIO. Actual restoration failures remain errors.

Rechecked upstream [SavageWriteMode](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
and Linux [savagefb_set_par_int](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c),
and compared the read-only local copies under `build/downloads/savage/` in
the main checkout. The final recheck received d598bf4c BOOT1.LOG: the
CR33 correction and source comparison are recorded below. No unrelated
register programming is changed.

The [Creative hardware guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf),
chapters 2–3, separates DMA progress from guest interrupt acknowledgement.
Keep virtual DMA on the physical timer, including while native VGA bypasses
the monitor. Linux's [intel8x0 period/IRQ path](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/intel8x0.c)
also keeps DMA progress independent of display ownership. The existing
Ciuki refill worker has P_DEVICE priority and a 4 ms running poll; the busy
focused VM has an 8 ms quantum. Full-screen audio uses a 2 ms VM quantum
and 2 ms refill safety deadline, retaining P_DEVICE and the scheduler's
device-runtime guard. If an IRQ has already made the refill wait condition
true, block for one tick before rechecking: a successful wait need not have
slept, and repeated ready returns otherwise accumulate the 10 ms device
allowance and demote refill below the focused VM. See Linux's
[sleep documentation](https://docs.kernel.org/timers/delay_sleep_functions.html)
for the distinction between blocking and busy waits; the numerical policy
here is Ciuki's, to be checked with host replay and physical records.

### F3-39 host evidence

The production provider model exercises entry strikes and recovered restore
strikes 1/2/3, no register access on the fourth request, successful sessions
preserving the count, and a failed entry plus failed restore costing one
strike. Failed recovery retains its snapshot. Resource conflicts permit
retry after release; activation quarantine and foreign registry generations
are preserved. The display service forwards final recovery success/failure
to the provider. The internal display/VM entry result distinguishes refused
windowed continuation from actual entry, so probes still reject a refused
entry. The owner syscall and queued hotkey path translate that result to
success with no fullscreen-error status. Failed desktop restoration remains
an error, including during partial-entry rollback.

The production scheduler replay records **11 ms** maximum between services
for an always-ready refill worker that is demoted by the existing device
guard; a real block between services keeps P_DEVICE and a **2 ms** maximum
under the same continuously ready focused VM. The production refill wait
test observes windowed 4 ms, full-screen 2 ms, pending IRQ 1 ms with pending
work retained, and idle 50 ms. The VM quantum test covers active SB16,
queued audio events, silent full screen, and smaller caller-supplied bounds.

A separate production timer/DMA/PCM/mixer replay uses a busy full-screen VM
with no window presents or VGA traps, 1 ms physical ticks and 2 ms guest IRQ
service/refill opportunities. All 32 double-buffer blocks contain fresh
samples, with zero late/stale/starved/skipped blocks. Observed maximum DSP
backlog is 93 timeline frames and IRQ service latency 48 frames (48 kHz);
the former includes the intentional block-boundary acknowledgement wait.
Using thread CPU time for the existing performance clock, one sanitized run
records `vm_perf_service ticks=180 audio_us=137`. This measures CPU cost,
not dispatch delay or T23 timing. The full test log retains the actual cost
for each run.

`native_refill us=… gap_us=… sb_lag_frames=… irq_service_frames=… late=…`
adds a periodic diagnostic row while full screen owns the display. Refill
cost/gap values are existing lifetime maxima; no formatting is added to
timer or refill callbacks. Compare these rows with `vm_perf_service` and
the SB16 block/IRQ records during the next hardware qualification.

Command outcomes and the final physical-log availability check are recorded
in `build/codex/f3-39-report.md`. The amended sources still need QEMU,
image and T23 validation with the lead/owner.

### F3-39 disk logs received before completion: CR33

The final recheck received d598bf4c BOOT1.LOG. Its first refusal (line 850)
is entry `stage=native-vga-mode reg=33 expected=0 actual=1 mask=ff`;
native restore succeeds. DOOM later enters/restores successfully (989/1109).
The next refusal (1162/1168) reads entry `00 -> 81` and restore `00 -> 80`
at CR33. VBE fallback succeeds (1176), then the old poison refuses retries.
Thus the next physical mismatch is **CR33**, not another panel SR register.

[RE046-B printed p.72, PDF p.76](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf#page=76)
defines CR33 bits 0, 2 and 7 as reserved; writable controls are 1 and 3–6.
X.Org `SavageModeInit` selects CR33=00 for mobile chips, and its
`SavageWriteMode` saves/restores CR33 without an equality assertion. The
local savagefb source likewise saves/restores the byte; its non-mobile
CR33 preset is not a SuperSavage recipe. Keep existing programming and
compare **7Ah**, covering every documented writable field while excluding
all reserved bits, for both entry and restore. Host tests add exact
01/80/81 readback replay,
reserved bit 2 coverage, and lost writable CR33 writes that still fail.

BOOT1.LOG's available `vm_perf_service` rows have 4860/5003 ticks with
4299/3186 us audio CPU (generation 17595), and four 5000-tick rows with
19506–22145 us (generation 11049). These are about 0.064–0.443% of wall time,
not evidence of costly virtual DMA. No native-refill gap/cost row exists in
this old image. CPU records around the successful native session retain
audio-refill at device priority and show zero device overruns. Consequently
the ready-return demotion is a host-proven failure mechanism and the shorter
cadence is implemented, but these logs do not prove demotion caused the
owner's lag. The new native-refill row is needed for that physical comparison.
