# f3-63: unified DOS only and launch/close costs

The owner’s unified-only decision supersedes the old kernel-choice parts of
f3-51 and f3-59. The public CREATE ABI retains values 0 and 1 as compatibility
aliases for 2; neither selects an old guest. Kernel tokens in expert files accept
only `unified`; invalid or retired tokens use the unified pageable default.
The native DOS Box has no settings panel or settings writer. Existing separate
network files remain readable. There is no end-user configuration guide.

## Sources and implementation decisions

- Microsoft Windows 98 Resource Kit, local licensed extraction
  `/home/peronslayer/Desktop/CiukiOS/build/rskt/wrkc10_6.htm`, “VCACHE and CDFS
  Supplemental Cache”, and `wrkc28_5.htm`, file-system architecture: protected
  mode VFAT and VCACHE own file access, with dynamic caching. Keep the existing
  native IFS/shared cache; do not add a second DOS disk cache or change memory
  limits. The supplied T23 serial reports 523260 KiB usable RAM, 126048 free
  NORMAL pages and 992 free DMA pages at boot; these are boot values, not a
  measurement of free memory during the later game launch.
- [FreeDOS 1.3 pinned packages](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/):
  the production fork, FreeCOM and SHARE retain their pinned source/license
  chain. The pristine reference is available to the host conformance harness
  only. A canonical build must fail if unified qualification is unavailable;
  it must never substitute the classic or reference kernel.
- [T13 ATA8-ACS, revision 4a](https://tc.gts3.org/cs3210/2016/spring/r/hardware/ATA8-ACS.pdf),
  READ DMA and READ SECTOR(S): command service includes command/device latency,
  rather than a guaranteed proportional bytes/time relationship. The project’s
  reader budget is a scheduling policy, not an ATA transfer-size requirement.
  Preserve its contended-reader split and controller floor; restore the maximum
  admitted command size for an uncontended sequential stream. Do not extrapolate
  a 106 ms outlier into a permanent 4.5 KiB transfer size.
- [NIST FIPS 180-4](https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf),
  section 6.2: SHA-256 consumes 512-bit message blocks. Compress complete input
  blocks directly and stage only partial blocks. Preserve the full-file digest,
  deny-write open, version-keyed cache and sidecar comparison; a sidecar alone
  does not prove that the underlying file is unchanged.

The shell-readiness signal uses the existing monitor idle path, consistent
with [RBIL INT 2F/1680](https://fd.lod.bz/rbil/interrup/windows/2f1680.html).
The syscall reports readiness only for a DOS-ready shell blocked in that path,
with no child/DPMI client and empty key queues. The client waits for it before
each line, retaining the existing key-event pace. It no longer types an extra
CLS command while its own loading panel already hides the boot text.

## Evidence and fixes

`build/lead-scratch/t23-3423912f-serial.txt` and the matching BOOT.LOG show the
first DOOM create hashing before its `game_launch ... cached=0` record. The
Quake create had not completed before cancellation. Thus the observed client
and storage CPU percentages must not be reported as steady-state game CPU.
No source-level diagnosis alone proves the requested physical timing target.

The 64-location VFS change ledger conservatively invalidated a cached base
version after unrelated writes evicted its entry. A digest now retains a
bounded counter watch (16 per VFS, refcounted), without keeping a handle or
denying later writers. Real writes/truncation, rename/replacement, raw changes
and remount still invalidate the version; unrelated ledger eviction does not.
The watch API requires its VFS to outlive the watch token, as the resident
system VFS does. Watch exhaustion falls back to conservative version checking.

The old hash loop staged each byte before compression. Read-ahead retirement
also scanned every cache descriptor even for a stream with no resident
speculation. Final close/table destruction held an untagged volume reservation
while retiring speculative I/O and scanning the cache. Close now has operation
attribution; speculative retirement releases the drive guard while retaining
its description/file lifetime. The no-speculation case avoids the cache scan.

A normal close asks through guest keyboard input and returns EBUSY while a
program is active. The native client declines the desktop close deadline and
keeps forwarding input, so a game can save/quit. An application can ignore the
request; this must not imply permission to discard it. A disappeared owner
still requires supervisor cleanup and records the forced reason and dirty
state. Clean idle-shell close continues to use the ordinary EXIT path.

Host replay results, exact commands, environmental limitations and the lead’s
remaining same-image timing checks are recorded in
`build/codex/f3-63-report.md`. No QEMU or canonical image build is performed by
this implementation task.

The existing private COW disk remains transient: even an orderly session end
removes it. Dirty deletion now records `transient-session-end` or
`forced-session-end` and cleanup status. Persistent save-file policy is not
invented here; the lead must decide if the owner intends cross-session saves
in addition to graceful close and explicit loss reporting.
