# Windows portable release

`bash scripts/build_full.sh` refreshes
`build/releases/CiukiOS-0.8.3-Windows-portable.zip` after building the
canonical FAT16 image. The ZIP contains `Start-CiukiOS.cmd`, `CiukiOS.img`,
64-bit Windows QEMU, firmware, DLLs, and license notices. It can also be
regenerated without rebuilding the image:

```bash
python3 scripts/package_windows_portable.py
```

The packager pins Stefan Weil's QEMU 11.1.0 Windows installer from
<https://qemu.weilnetz.de/w64/2026/> and verifies its published SHA-512
before extraction. QEMU's Windows files stay under `qemu/` inside the ZIP.
The manifest records hashes of the image and executable. The image is copied
before local Doom, Wolfenstein 3D and DOS Navigator payloads and matching
desktop shortcuts are removed. Freed FAT16 clusters are zeroed so deleted
game data is absent from the ZIP; the development image is left untouched.
It also records the source commit and whether the build tree was clean.

The launcher presents a 1280×800 VGA window, SB16/AdLib sound and an NE2000
adapter on user NAT. It runs QEMU **on Windows**; DOS programs still run in
CiukiOS. The current guest does not get GPU acceleration or Win32 support from
this wrapper.

The first package build downloads the pinned installer (about 198 MB); later
builds reuse it. Set `CIUKIOS_WINDOWS_PORTABLE=0` for an isolated development
build that should skip ZIP refresh.

On the supported Linux workstation, the pre-push build runs in a bounded
systemd user scope (`MemoryMax=3G`, `MemorySwapMax=1G`, `CPUQuota=100%`). The
Linux QEMU runner uses a separate scope (`MemoryMax=768M`, no swap,
`CPUQuota=200%`, `TasksMax=128`). Full and full-CD builds/runs stay sequential.

The ZIP is checked for integrity on Linux. Run-time testing is limited to the
main Linux full-image profile, as requested; no Windows launch result is
claimed. The ZIP is a generated file under `build/releases/`, so it is not
committed to Git.

## Publish on push without GitHub Actions

In this clone, the local Git `pre-push` hook handles pushes to `origin/main`.
It builds the full image and ZIP, runs one 128 MiB Linux QEMU boot smoke,
then lets Git push the commit. A local background publisher waits until
`origin/main` has that commit, then creates a GitHub prerelease and uploads
the ZIP. It records the result in `build/releases/pending/<tag>/publish.log`
and `status.json`; Git itself has no client-side post-push hook. If an upload
failed after a successful push, `python3 scripts/push_release.py --resume`
retries from the prepared ZIP without rebuilding. A dirty worktree is refused
before each new push so the ZIP matches the commit.

For a fresh clone, install the tracked hook with:

```bash
bash scripts/install_release_push_hook.sh
```

GitHub CLI (`gh`) must be installed and authenticated with repository write
access. The release tag includes `0.8.3`, the commit time in UTC, the Git
commit count as build number, and the short commit ID. For example:
`v0.8.3-20261004T103012Z-b829-g934830dd`. The ZIP asset carries the same
version, timestamp and build number. The release notes state that Windows
runtime execution was not tested.

Run `python3 scripts/push_release.py --plan` to preview the next tag without
building or publishing. Pushes to other branches do not create releases.

See the [current project status](project-status-2026-10-04.md) for the 0.8.3
feature and validation boundary. This bundle is checked on Linux for archive
integrity; no Windows runtime launch is claimed.
