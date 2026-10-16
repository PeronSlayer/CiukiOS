# F2-77 desktop observer handle lifetime

Research and implementation decision, 2026-10-16. Host evidence only;
image qualification and QEMU remain the lead's responsibility.

POSIX [open](https://pubs.opengroup.org/onlinepubs/9799919799/functions/open.html)
creates a descriptor and open file description; `O_CLOEXEC` prevents unintended
inheritance. [close](https://pubs.opengroup.org/onlinepubs/009604499/functions/close.html)
releases that descriptor. A snapshot between these calls therefore counts a
handle even if the application closes it immediately after the observation.

In this repository, `proc_snapshot` counts occupied process descriptor slots.
The desktop's media reader opened `/dev/media` twice per pass, its network
reader opened `/dev/net` once, and its mixer reader opened a public read-only
descriptor whenever no delegated control handle was available. Device queries
use the namespace's persistent PX_MEDIA/PX_NET/PX_MIXER nodes; they do not
require a fresh description per observation. Keep one close-on-exec observer
for each device, prepared before the desktop's first gate report. Reuse the
media descriptor for both state and volume queries and removal. Preserve the
separate delegated mixer control authority. Release observers on query errors
and retry on a later observation; release them at shutdown preparation, with
lazy restoration if shutdown is cancelled.

Keep the crash probe's global object and process comparisons exact. On a
handle-count mismatch, compare occupied slots against a per-process baseline
and emit separate bounded diagnostic records with PID, descriptor, and object
kind. No desktop-device allowance or retry may conceal a leaked handle.

The available read-only result is
`/home/peronslayer/Desktop/CiukiOS/build/test-runs/zz-usb/d395a9f8/result.json`:
cycle 33 has handles `19/20`, with equal object/memory counts and a restored
final ledger. It has no per-process descriptor evidence, so the historical
descriptor's identity cannot be recovered from it. The directive's
`zz-usb/badc5642` and `zz-usb/7fd10c05` directories are absent. New diagnostics
and host interleaving coverage supply the missing identification seam without
claiming a new guest observation.

Host validation completed serially with `nice -n 19`:

- `make CAP= desktop` built and inspected the native applications using a
  local copy of the installed SDK, without rebuilding the SDK or a system image.
- The desktop suite ran 102 tests: 99 passed initially; three required the
  missing pinned FreeType archive. After copying that archive from the main
  checkout, all three passed in a focused rerun. No golden files changed.
- The launcher regression interleaves production readers with `proc_snapshot`
  and the strict process comparison. The previous open/query/close sequence
  reports PID 2, net FD 20, media FD 22 and mixer FD 21. One hundred warmed
  polling rounds keep every query snapshot equal. A deliberate additional
  media FD 23 is rejected. Error recovery, public versus delegated mixer
  authority, shutdown cleanup and Cancel recovery also pass. The final focused
  launcher rerun passed after test-fixture cleanup.
- The gate suite passed 11 tests, including positive and negative predicate
  checks for all three `f1-usb` cases. Their definitions and exact restored/final
  ledger requirements remain unchanged.
- `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh` exited
  successfully. Its optional DOSBOOT integration run was skipped because the
  qualifying prebuilt image is absent; standalone host DOS tests ran.
- The modified probe compiled with production i686 flags, `-Werror` and the
  4 KiB frame limit. Its handle map is 8,448 bytes in BSS. `git diff --check`
  passed.

Detailed commands and logs are indexed in `build/codex/f2-77-report.md`.
No QEMU, systemd scope, commit or diary edit was performed. The integrated
image's guest runs and SHA-256 qualification remain with the lead.
