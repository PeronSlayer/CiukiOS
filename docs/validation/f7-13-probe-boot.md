# Savage background qualification (f7-13)

Research checked on 2026-10-26 before implementation. The directive is
[f7-13](../directives/f7-13-probe-boot.md). Hardware acceptance is pending.

The observed `L:SELECT_ERROR` has a direct source explanation: the loader's
`config_read` accepts only its exact option table or a valid `mode=0xNNNN`.
`3d=probe` was absent from that table and therefore jumped to `fatal` before
the kernel was loaded. The fix adds only this exact NUL-terminated option;
it does not broaden unknown-option acceptance. [NASM initialized-data
semantics](https://www.nasm.us/doc/nasm03.html#section-3.2.1) were checked,
and the assembled-parser host fixture covers acceptance, handoff preservation,
flag preservation, and malformed lookalikes. This explains the reported final
loader line and missing kernel/disk records without assuming a GPU stall.

The previous `.record` hook called the complete qualification from the display
worker. Its preparation pin allowed presentation but excluded mode changes;
its engine mutex also held kernel preemption across uploads and readback.
Scalar reference slices ran consecutively without a scheduling point.
This proves a synchronous latency hazard, not the failed trial's exact cause:
with the supplied 15,532,032-byte VRAM census, cursor at `vram-4096` and
2D rows ending at 2048, 1024x768 leaves only 7,139,328 bytes after the 8 MiB
2D reservation. The unchanged gate requires 8,183,808 bytes and must refuse
before issuing any 3D command. Host tests now cover that exact census.
The missing serial capture of a `3d=probe` boot is essential to resolve the
discrepancy. No VRAM reservation or memory limit is changed here.

Primary sources and decisions:

- [S3 RE046-B, sections 1 and 13](https://vgamuseum.info/images/doc/s3/savage4_registers.pdf):
  shared tiled windows and engine/event status require serialization and proven
  completion before readback. Keep the existing packet, idle, guard and restore
  rules. A scheduling timeout alone is not a hardware fault.
- [X.Org Savage, `GetTileAperturePitch` and tiled surface setup](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c):
  retain the existing SuperSavage alias and descriptor programming. Readback
  borrows the alias for at most 128 pixels, restores its registers, and releases
  the presenter before yielding. No new physical tiling formula is introduced.
- [OpenGL 1.1, sections 3.5, 3.8, 4.1 and 5.5](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf):
  preserve projective/filter/depth tolerances and completion semantics. A
  reference raster need only compute the rows actually compared: fragments
  have no dependency on other rows. Precision checks retain their independent
  per-pixel envelope; redundant full-frame reference rendering is unnecessary.
- Windows 98 Resource Kit, chapter 28, Process Scheduler (`wrkc28_4.htm`,
  read in the owner's local extracted text): background work belongs on its
  own schedulable thread so foreground interaction continues. No private text
  or capture is redistributed.

Policy: queue from `.record`, start only after a successful full desktop
frame, use an idle-class worker, and keep hardware admission closed until the
attempt is completely finished and its marker cleared. The proposed total
budget is 5 seconds including preparation/persistence, with 500 ms per render
phase and a 1 second marker budget. CPU reference, texture preparation,
comparison and VRAM transfers yield in bounded chunks. The existing 20 ms
engine/restore limits and 1 ms read chunks remain unchanged. Deadline refusal
restores/releases a healthy engine and retains the attempt marker, preventing
an automatic retry on the next boot. Integrity or unproven-writer failures
retain the existing quarantine policy. A deadline cannot recover an actual
machine/bus hang; the previously committed marker protects the next boot.
If the display generation changes before that first frame, the queued job
cancels before acquiring a pin or claim. It never waits for an obsolete mode.
An early capability query can create an empty software render service before
qualification completes. The display worker may attach that empty service
after admission, preserving monotonically allocated handles and fence sequence.
Live software contexts keep their existing resources and policy; they close
before a later empty-service attachment. No grant or lease is issued by this
refresh.

The 100 MiB/s readback and PIII operation costs are repository modeling
assumptions, not measured T23 read bandwidth. The earlier 108–134 MB/s LFB
measurements describe writes. No read-rate measurement occurs in the supplied
serial capture. The implementation report must distinguish them.

## Administration and T23 retest

Keep `safe=0 serial=1 3d=probe` in BOOT.CFG for the trial. Capture serial
before power-on. The desktop must present before `phase=prepare`; the queued
record announces `after=desktop_frame priority=idle`. Expect begin/end records
with `elapsed_us`, `total_us` and `error` for preparation, marker I/O, minimum,
four full-size phases and 23 precision phases. A successful attempt finishes
with `hardware=1 marker=clear`. A deadline refusal has `hardware=0`, a
`budget_total`, `budget_phase` or `budget_work` reason, and `marker=retained`
if the attempt was armed. Verify continued desktop repaint and input, disk
BOOT.LOG records, working 2D after a clean refusal, and no late admission.

A nonempty `C:/system/settings/accel-3d-s3savage-5333-8c2e.cfg` prevents
retry. The existing declarative attempt policy also makes the next boot use
software 2D; it does not wait for the previous qualification. Keep that
conservative policy. A previous attempt is never automatically cleared.
An ambiguous BEGIN commit/close error is reported as `marker=unknown`; it
never authorizes a clear or a hardware operation.

Use Control Panel's **Retry acceleration after restart** for an explicit
retry, then restart with `3d=probe` still enabled. The existing request file
`C:/system/settings/accel-retry.cfg` contains exactly:

```text
driver=s3savage
pci=5333:8c2e
```

It must end with a newline and be committed/closed before restart. The
registry consumes the request and clears this device's declared 2D/3D
markers; other devices' markers remain. An administrator working offline may
instead truncate the named 3D attempt file to zero bytes after reviewing the
failed phase. Never remove a retained marker automatically at boot.

Before retrying a real hang, first boot with the retained marker and confirm
that the desktop starts using software rendering. The host model verifies
the scheduling, bounded refusal and marker policy; it cannot identify the
unrecorded probe boot's exact hardware failure or prove T23 timing.

## Integration with f7-12 and review fixes (2026-10-26)

The merge with the f7-12 default formats keeps both features; the review of
both branches added these rules (design detail in
[savage-default-formats.md](../design/savage-default-formats.md)).

* The optional RGB565 and D24/S8 gates follow the full-size and precision
  phases through the same paced helpers. Expect `rgb565_prepare`, `rgb565`
  1–4, `d24s8_prepare` and `d24s8` 1–34 begin/end records, then
  `[3d] attachment format=RGB565|D24S8 result=... budget=ok|<reason>`.
  A budget overrun anywhere, including an optional gate, refuses the attempt
  as above (`hardware=0`, budget reason, `marker=retained`); no optional gate
  starts after one. A clean optional refusal leaves the other formats.
* A clean full-size refusal (allocation or texture-pool preflight `-ENOMEM`,
  a numeric mismatch or a local transfer timeout, all restored) keeps the
  qualified minimum, as [the f7-11 contract](../design/savage-fullsize-admission.md)
  requires. Only budget overruns and integrity failures (device lost) reset
  admission and release the claims.
* The marker request passes microseconds: `now` is the probe clock and the
  deadline the lesser of the total and marker-phase deadlines, unscaled. The
  block layer converts the remainder to milliseconds (`blkreq.c`); the earlier
  division by 1000 turned the 1 s marker budget into a 1 ms request timeout.
  The owner test now runs the marker I/O through the real request scope.
* The finished worker is reaped by the next display notification or record
  hook on another task, exactly once, after `task_exit` made it a zombie.
* Partial D24/S8 clears of ordinary jobs are bounded windows with a 20 ms
  absolute deadline; between windows only the presenter mutex is yielded.
