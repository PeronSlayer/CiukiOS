# F1-63 volume resolution with optical and failed removable media

## Sources and implementation decision

Researched 2026-10-14 before implementation:

- [Microsoft FAT specification 1.03](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf),
  BPB, volume bounds, directory layout and FAT[1] clean/error flags. A failed
  preparation is not permission to publish an uncertain writable filesystem.
- [QEMU USB documentation](https://www.qemu.org/docs/master/system/devices/usb.html),
  explicit `if=none` block backend plus `usb-storage` on a named controller
  bus. A USB fixture must not silently become an IDE disk.
- Local contracts: `docs/design/posix-subset.md` (namespace) and
  `docs/design/registry-contract.md` (persistent roots and durability).

The Open Group path-resolution and fsync pages were requested, but returned
HTTP 403. They are not claimed as successfully consulted sources.

The directive's T23 evidence establishes a writable C:, read-only D:, failed
USB E:, EROFS on preference persistence, and no DOS VM creation-stage record.
It does **not** establish which syscall failed, which volume it reached, or
that the CD/USB changed the native root. Missing launch records alone do not
prove that a process was never spawned.

Audit: `files_attach` and `px_resolve_locked` anchor `/` to FAT drive index 2
(C:). Other drives are reachable through `/mnt/<letter>`. The legacy VFS has
a per-table current drive; publishing letters does not change it. Optical
slot 2 and drive index 3 (D:) are separate indices. Registry selection is the
absolute `/System/Registry` directory, not the most recently attached drive;
`reg_open_root_at` preserves that directory. The desktop's private libreg
instance uses the same SDK grammar and storage transaction. File FSYNC
commits the description's own FAT volume. `/Programs/dosbox`,
`/Games/CIUKIGAM.IMG` and `/System/cache/wallpaper` therefore target C:.
The kernel DOS disk selector separately uses explicit drive-qualified paths.

`storage_add_removable_bounded` retains error records but does not publish a
failed preparation. Its publication loop rolls back all successful attaches
if any attach fails. `usb_storage_service` quarantines a failed mount;
published devices are detached on a later service turn after a fault. Existing
descriptions may report EIO across surprise removal, while fresh paths through
the removed letter must report ENOENT. That is intentional lifetime handling.

Decision: exercise these invariants with production filesystem and registry
code and independent media counters before changing routing. Add desktop
failure records at the existing launch boundary, preserving errno through
diagnostic output and descriptor cleanup. Do not change USB bind stages (the
parallel F1-62 task owns those stages) or infer a routing repair from errno
alone. Host and lead verification results follow below.

## Host regression and result

`tests/host/test_volume_resolution.py` links production mount/cache/FAT/ISO,
native path/fd operations and `apps/desktop/master_storage.c` (its private
instance of the actual SDK libreg). Host stdio/syscall adapters send registry
transactions through those native operations. The ISO is independently
generated with xorriso. The FAT fixtures use mkfs.fat and mtools.

548 ASan/UBSan checks pass. C: remains writable; optical slot 2 publishes D:;
a terminal FAT BPB read failure retains an E: diagnostic but publishes no
letter. Master saves, stat/read of dosbox, the games disk and the wallpaper
cache all use C:, with no CD/USB reads, writes or flushes. Native cwd is on D:
and then a live/expired removable F:; legacy current drive stays D:. Fresh
opens of failed E: or removed F: report ENOENT; a retained removed-volume
description reports EIO. The test reproduces the topology, **not** the T23's
EROFS/EIO symptoms. No mount, USB publication or libreg routing change is
justified by this result; those production files remain unchanged.

Desktop failures now report `[launch] name=<name> error=<positive errno>
path=<executable> operation=<operation>` at the first failed launch boundary:
power, child slot, channel pair, channel inheritance, client slot or spawn.
The native raw debug syscall bypasses FILE/VFS locks, as the desktop watchdog
already does. At `operation=spawn`, the path identifies the executable
passed to the loader; it does not identify an internal loader read/stat stage.
Records fit the debug syscall's 240-byte cap. Inventory paths are complete;
a longer Documents executable path has a 108-byte prefix followed by `...`.
The existing F1-61 VM-create and disk records cover subsequent DOS disk work.
The desktop does not pre-open the games image, autorun data or a loading-panel
file on its Games-menu launch route.

## QEMU case definition for the lead

The two cases in `tests/suites/f4-media.json` are:

- `volume-resolution-cd-usb-qemu-t23`: independent Joliet CD and healthy FAT16
  USB stick, C: system / D: optical / E: removable.
- `volume-resolution-cd-usb-failed-qemu-t23`: same topology, but the generated
  stick has an invalid BPB. Require the USB mount refusal, not a silent absence
  of a USB device. This tests failed mounting; it does not simulate the T23's
  controller timeout or replace F1-62's bind evidence.

The runner now gives `transport: usb` FAT fixtures their own named UHCI bus,
an `if=none` backend and a BOT `usb-storage` frontend. Their fixture ordinals
are independent of IDE connector numbers. A CD used as background media has
its SHA-256 checked unchanged; only the `optical` parser probe claims the full
independent directory/hash comparison.

Both cases reuse `f3:vm-window` on the canonical image. That starts the desktop
and a DOS window through the ordinary launch path. At `window_input` ARM the
runner uses Ctrl+Esc, five Down presses and Enter to open Run; types
`control --panel=sounds`; then two Tab presses and Space apply the **master
mute preference**, keeping the canonical 20/20 levels. After the async save
wait, Alt+F4 closes Control Panel and the original VMVIDEO input/frame/cleanup
checks proceed. Thus both operations occur with CD and USB attached; the DOS
window is already open during the preference save. The master record must
contain `[System\Audio]` and `"Master" = 70676` (20 | 20<<8 | 1<<16) on **C:**
after teardown. A dirty-only fsck result is classified; structural filesystem
errors fail. Serial predicates require mounted D:, the healthy/refused USB
mount outcome, and the DOS VM-create stage. No commercial payload is used.

Run after the lead's single canonical capped build (host memory checked first),
using the runner's normal capped, sequential execution:

```bash
nice -n 19 python3 scripts/test/run.py f4-media --profile qemu-t23
```

The suite runs the existing T23 media cases and the two new cases; the runner
has no individual-case CLI filter. No QEMU was run in this worktree. The lead
must review input timing and execute both new cases on the integrated image;
case declarations and host controls alone are not a QEMU pass.

On T23, repeat with the actual CD and failing stick, save a changed **volume
level** in Control Panel, then launch DOOM and Quake. Confirm the preference
survives restart, the wallpaper and Files remain responsive, and the new
first-failure record is captured if launching still fails. An EROFS preference
failure needs the first failed registry syscall and C:'s contemporaneous
readonly/reason/error state before a root cause can be claimed. Missing or
failing VM-create records must be reconciled with the new desktop boundary
record. F1-63 hardware acceptance remains open.

Additional host checks passed: full filesystem and USB-storage suites; common
runner (90 tests, four missing canonical-image/map prerequisite skips); optical
(14 tests); registry suite; five new runner controls. Native `desktop.c` also
compiles with the installed i686 SDK flags/headers and `-O2 -Wall -Wextra
-Werror`, without rebuilding the SDK or image. The final desktop suite passes
23 tests with three missing canonical desktop-build prerequisite skips,
including first-failure logging/errno preservation/cleanup and the actual
Control Panel input sequence. Full evidence is in `build/codex/f1-63-report.md`.
