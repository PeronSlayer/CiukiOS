# Directive f4-00: draft the F4 contract (audio, media, shared volumes, optical media, NTFS read-only)

- **Step:** F4 (contract before code: decision D11 and the scope decision
  of 2026-10-09). **Inputs:** `foundations-transition.md` (F4 row),
  `dos-dpmi-contract.md` (the `[F4]` requirements: audible SB16/OPL output,
  DOS-to-VFS bridge), `device-firmware-ownership.md`,
  `vfs-storage-contract.md`, `hardware-baseline.md` (which audio devices
  the T23, the E500 and the generic desktops really have), the legacy
  notes in `legacy/CiukiOS-docs-legacy-2026-10-09.zip`
  (`docs/design/ac97-music-streaming.md`, `dos-audio-block-scheduling.md`,
  `dos-audio-codec-timing.md`, `t23-startup-audio.md`,
  `docs/validation/2026-10-04-ac97-audio-latency.md`: extract into
  `build/` to read, never restore into the tree).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`, web search on.
- **Worktree:** `wt/f4-contract`. File: `docs/design/f4-audio-media-contract.md`
  only. Status line: "Draft by Codex, awaiting the lead's review".

## Research first (primary sources, linked in the document)

Intel AC'97 component specification 2.3 and the ICH/82801 AC'97 bus-master
programming (buffer descriptor list, civ/lvi, status), the audio controller
actually present on each target (check the baseline and the registry logs:
do not assume ESS Maestro from the chipset name), Sound Blaster 16 DSP and
mixer programming and OPL3 (YMF262) register semantics (for the virtual
device only), ATAPI packet commands (SFF-8020i/MMC) for CD-ROM, ECMA-119
with Joliet, the NTFS on-disk format from a GPLv2-compatible reference, and
S3/Mach64 2D acceleration only if the sources are solid (otherwise defer
with a sentence).

## Sections required

Decision and sources; native audio API for processes (device node,
formats, period/ring model, mixing policy, latency budget, volume
persistence); kernel mixer and backends (AC'97 first with the QEMU twin,
the second device after qualification); the DOS VM audio path (SB16
DSP/mixer/DMA and OPL state from f3-05 into the mixer; virtual IRQ timing
while guest IF is clear); the DOS-to-VFS bridge carrying every `[F4]`
requirement of `dos-dpmi-contract.md` in testable form (shared-drive
routing inside CIUKIDOS, JFT/SFT, sharing and locking matrix, searches,
FCB, raw-access refusal); ATAPI with ISO 9660 read-only mounts; NTFS
read-only mount (driver choice and license); acceptance tests per
`test-architecture.md` (host, QEMU, T23/E500); open questions; interfaces
required from other contracts. Phase tags `[F4]` as in the other
contracts. Requirements are project policy informed by the sources, not
claims about the existing code.
