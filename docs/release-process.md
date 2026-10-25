# Stable image releases

Stable releases are separate from the suspended push prereleases. Only the lead
runs `scripts/release_stable.py`, after the owner approves a named disk SHA-256.
The pre-push hook and `main: suspended` policy remain in force.

## Research and implementation contract

The release tool reads the existing `build_image.py` and `build_cd.py` schemas:
their clean-source fields are `git_dirty: false` and `git_revision`, rather than
the portable bundle's `source_dirty` and `source_commit`. Both builds must name
one clean commit; the CD must additionally identify that disk's SHA-256 and
have `source_matches_build: true`. Packaging never rebuilds or scrubs the approved
disk: every converted disk must compare equal to its original logical sectors.

- [QEMU image utility](https://www.qemu.org/docs/master/tools/qemu-img.html)
  and [disk formats](https://www.qemu.org/docs/master/system/images.html):
  use explicit raw input, standalone qcow2, streamOptimized IDE VMDK for OVF,
  and monolithicSparse IDE VMDK for the runnable VMware VMX package. Check
  formats and compare every conversion to the approved raw disk.
- [QEMU invocation](https://www.qemu.org/docs/master/system/invocation.html)
  and [i440fx devices](https://www.qemu.org/docs/master/system/i386/pc.html):
  retain the repository's qemu-t23 machine, Pentium III, 512 MiB, one CPU,
  standard VGA, AC'97 and PS/2, with E1000 user networking and local RTC.
  Launchers prefer the host's KVM/WHPX/HVF accelerator where available, then
  TCG. Apple Silicon needs TCG for this x86 guest. Emulators are installed
  separately; their binaries and firmware are not redistributed.
- [DMTF DSP0243 1.0.0](https://www.dmtf.org/sites/default/files/standards/documents/DSP0243_1.0.0.pdf),
  sections 5, 7, 8 and 9, and the
  [OVF 1.0.0 schema](https://schemas.dmtf.org/ovf/envelope/1/dsp8023_1.0.0.xsd):
  an uncompressed USTAR OVA starts with the OVF descriptor, then its SHA-1
  manifest and referenced streamOptimized disk. Outer `SHA256SUMS` uses SHA-256.
  The descriptor declares one IDE PIIX4 disk, 512 MiB, one CPU, ES1371, E1000
  and VGA. VirtualBox's
  [OVF importer](https://raw.githubusercontent.com/mirror/vbox/master/src/VBox/Main/src-server/ApplianceImplImport.cpp)
  maps the simple OVF sound request to AC'97 and defaults IDE to PIIX4;
  these are upstream source observations, with runtime qualification pending.
  Host tests check the schema rules used by this restricted descriptor
  (namespaces, required attributes/elements, order, types and references), plus
  semantic disk/controller/network links. This is not importer certification.
- [Wine 10.0 LGPL 2.1](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/COPYING.LIB),
  sections 4 and 6, and `third_party/wine/SOURCE-OFFER.txt`: publish the exact
  corresponding sources and notices alongside every binary package, including
  Wine's pinned archive, patches, full build recipes and configuration records.
  Reuse the CD's hash-checked `SOURCES` and `LICENSES` contents. Check the CiukiOS
  source ZIP against the built Git tree, and pinned/produced archives against
  their configured hashes; a URL-only offer is insufficient.
- [GitHub CLI release create](https://cli.github.com/manual/gh_release_create):
  explicitly target the source commit and upload all artifacts with a notes file;
  stable releases do not use `--prerelease`; alpha/beta channels do. Dry runs never invoke `gh`.

The present Setup CD is El Torito hard-disk emulation, without a hybrid USB
boot layout (`build_cd.py`). Distribute it as a CD ISO and state this limit;
use the raw disk for USB writing. OVF device mappings vary by importer, so a
separate VMware VMX explicitly selects ES1371, E1000, IDE and VMware SVGA.
VirtualBox/VMware imports and Windows/macOS launchers require lead runtime
qualification; host conversions do not establish boot or driver compatibility.

## Procedure

The following procedure is completed by the lead after integration. Install
Python 3, mtools, xorriso, qemu-img, Git and authenticated GitHub CLI. Build the
canonical disk and Setup CD from the same clean source commit using the normal
capped, sequential build procedure in [Build and run](build-and-run.md).
Do not package a mounted, running or modified image.

Record the owner's explicit approval in `config/release-policy.json` under
`stable`: set `owner_approved` to `true`, `approved_image_sha256` to the full
64-digit disk digest, `source_commit` to its full Git commit, and
`approval_record` to the owner decision reference. The shipped default grants
no approval. These fields are an operator record, not a cryptographic signature.
The tool refuses missing approval, mismatched/dirty manifests, unlisted files,
unrecognised components, missing sources/notices, private or commercial payloads.
It audits FAT files (including nested guest images), ISO contents and source
archives; private bytes are also checked by content against the configured
private roots on the release workstation. Keep those roots available for audit.

Release channels. `--channel stable` (default) publishes `v<version>-stable.<n>`
titled `CiukiOS <version> — stable <n>`, not a prerelease. `--channel alpha` (or
`beta`) publishes `v<version>-alpha.<n>` titled `CiukiOS <version> alpha`
(` <n>` appended only when n > 1), with `gh release create --prerelease`; the
source's `config/version.json` stage must equal the channel. `n` counts earlier
tags of that version and channel, local and (outside dry runs) remote. All
other checks, artifacts and approvals are the same for every channel.

Public text rule: the tool's disk README, VM README and `RELEASE-NOTES.md`
describe hardware generically (late-1990s/early-2000s desktops and laptops,
component classes). Packaging refuses if they name a specific PC model, and a
host test fails if the CHANGELOG section of the version or
`docs/release-notes-1.0-alpha.md` does.

Fetch release tags before an offline preview. For example:

```bash
python3 scripts/release_stable.py --version 1.0.0 --approved-image-sha256 <sha256> --dry-run
python3 scripts/release_stable.py --version 1.0.0 --approved-image-sha256 <sha256>
# alpha pre-release (tag v1.0.0-alpha.1, GitHub prerelease):
python3 scripts/release_stable.py --version 1.0.0 --channel alpha --approved-image-sha256 <sha256> --dry-run
python3 scripts/release_stable.py --version 1.0.0 --channel alpha --approved-image-sha256 <sha256>
```

Dry run validates approval and both inputs and prints the planned artifacts,
conversions and publication command without writing release outputs or using
the network. Its next stable number uses local tags. Publication checks remote
tags too, uses `v<version>-stable.<n>` and title `CiukiOS <version> — stable <n>`,
and refuses an existing destination. Packaging stages on disk, then retains
only the new stable release directory under `build/releases/`; unrelated files
there must be relocated by the lead first. Failed preparation preserves the
previous release. A publication failure retains the complete prepared artifacts
for inspection; the tool does not overwrite an existing release or retry upload.

Each release contains disk/QEMU/VMware/VirtualBox ZIPs (VMDK+VMX; VDI+VBoxManage scripts and README), Setup ISO, OVF OVA, corresponding
sources ZIP, notices ZIP, both input manifests, checksums and release notes.
The notes quote the matching CHANGELOG section, disk digest and source commit,
with instructions and qualification limits. Verify `sha256sum -c SHA256SUMS`
before publication or writing physical media. The disk README covers Rufus,
balenaEtcher and `dd`, and the generic late-1990s/early-2000s i686/CMOV BIOS targets
from [the hardware baseline](design/hardware-baseline.md). Always select the
whole destination disk carefully: writing the raw image erases it.

Host verification (one suite at a time):

```bash
nice -n 19 python3 -m unittest discover -s tests/host -p 'test_release_stable.py' -v
```
