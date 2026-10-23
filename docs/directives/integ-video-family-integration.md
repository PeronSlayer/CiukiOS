# integ-video — Integrate the display driver model parts A/B and the native 2D family drivers into one tree

- **Phase/model:** integration, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/integ-video` from `wt/f2-101` (part A, plus the lead's `memchr` fix in `apps/desktop/protocol.c`).
- **Why:** the 20 committed branches below all build on part A and all edit the same shared files. That covers the registry table, `scripts/build_kernel.py` source lists, `scripts/test/host_kernel_tests.sh`, `scripts/build_image.py` notices, the credits, the display-driver-model and video-families documents, the name test, `display_driver_legacy_fixture.h` and `src/kernel/core/hardware_check.c` (10 of them). Sequential git merges produce a long chain of conflicts, so one integrated tree is built instead.

## Branches to integrate (all relative to `wt/f2-101`)

| Group | Branches |
| --- | --- |
| Part B | `wt/f2-101b`: Savage files in `video/s3savage/`, keyed markers |
| Includes earlier branches | `wt/f2-112` (Savage family; includes f2-101b), `wt/f2-106` (Radeon R200–R400; includes f2-103), `wt/f2-105` (NVIDIA FX/GF6; includes f2-104) |
| Families | `wt/f2-107` (Intel gen2/gen3), `wt/f2-108` (VIA), `wt/f2-109` (SiS), `wt/f2-110` (Matrox), `wt/f2-111` (VMware SVGA), `wt/f2-113` (Trident), `wt/f2-114` (Mach64), `wt/f2-115` (Rage 128), `wt/f2-116` (Cirrus), `wt/f2-117` (3dfx), `wt/f2-118` (S3 Trio/ViRGE), `wt/f2-119` (Intel i810/i740), `wt/f2-121` (NeoMagic), `wt/f2-122` (C&T) |

## Required

1. **Bring every branch's changes** (`git diff wt/f2-101 wt/<branch>`) into this worktree. Use `git apply` on the working tree, or edit by hand; do not run commands that write `.git`, because the lead commits. Each driver directory and its own tests come over unchanged.
2. **Unify the shared files.**
   - One registry table with every driver, in a deterministic order: native candidates by specificity; the Savage keeps the T23 behaviour.
   - One source list in the build and in the host suite.
   - One set of credits and notice entries, and one names-test allowlist covering every driver directory.
   - One legacy fixture.
   - **One generic DM9 native-only comparison path** in `hardware_check.c`, used by every driver without its own sweep, with each family's self-test hook preserved. Duplicated variants of that path must be merged into one, not kept side by side.
   - The family table rows in `video-driver-families.md` and the families list in `display-driver-model.md`.
3. **Part B relocation.** Every family must compile against B's layout: Savage under `video/s3savage/`, and the attempt marker in `attempt.c` with keyed markers.
4. **Intel i810/i815 (f2-119).** Apply the f2-107 lead decision 1 to its ring and GTT: kernel-owned pages in proved-free GTT entries, never stolen memory, entries restored on stop, activation refused without proof.
5. **Verification.** Run `scripts/test/host_kernel_tests.sh` and the display, driver-model, names, hardware-check, desktop accel-notice and device-manager tests, then each family's own Python test module. Report exact commands and results. A LeakSanitizer exit after all assertions in this sandbox is not an assertion failure; say so.
6. **Report** the final driver list with the PCI families each covers, and any branch content you could not integrate, with the reason.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Host tests one at a time with `nice -n 19`. Write `build/codex/integ-video-report.md`.
