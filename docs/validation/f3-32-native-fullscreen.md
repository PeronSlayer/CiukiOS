# F3-32 native full-screen provider: F3-36 restore amendment

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
