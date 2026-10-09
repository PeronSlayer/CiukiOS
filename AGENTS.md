## Project identity — Ciuki

CiukiOS is dedicated to **Ciuki**, the dog pictured in
`misc/CiukiOS_SplashScreen.png`. This is an explicit project requirement from
the owner, recorded on 2026-09-25; preserve it across sessions and redesigns.

The approved OS logo is the detailed Ciuki portrait in `assets/brand/ciuki-logo.png`
(the first generated proposal, explicitly selected by the owner). Derive runtime
icons directly from that exact asset. The owner rejected the hand-drawn pixel
reinterpretation and the later simplified alternative: do not redraw, regenerate,
or substitute them. Preserve the approved portrait's shape, pose and colours;
only deterministic size/format conversion for the renderer is appropriate.
Do not substitute a generic dog, an abstract letter C, or an unrelated mascot.
Keep the original Ciuki photograph in the boot splash. Desktop and Setup must
share this identity.
UI copy remains English; the existing tagline is `A modern Retro OS`.

The official system icon family uses **Tango Icon Theme 0.8.90**, whose upstream
icons are released into the Public Domain. Keep its source archive, authors and
license notice in `assets/icons/`; see `assets/icons/README.md`. Computer, About
and other CiukiOS identity icons must incorporate deterministic conversions of
the exact approved Ciuki portrait above. The portrait is a separate project
asset, not part of Tango's Public Domain release. Do not replace this family
with extracted Microsoft/Apple artwork or change the approved portrait.

## Active build profile

Since the F0 switch (2026-10-09) the canonical build on `main` is the Ciuki
VMM image: `make build-full` builds `build/f0/VMM.ELF` with
`scripts/build_kernel.py` and the 512 MiB FAT32 image `build/f0/ciukios.img`
with `scripts/build_image.py` (T1 checks, `build-manifest.json`). Tests go
through `scripts/test/run.py` (`make qemu-test-full` = `f0-smoke`); see
`docs/build-and-run.md`. The 0.8 image is built only on branch `legacy-0.8`;
on `main` its scripts remain as `make legacy-*` targets until each component
is migrated or retired (`docs/design/foundations-transition.md`). Do not
build, run or test the floppy profile; the CiukiDOS kernel source
(`src/boot/floppy_stage1.asm`) stays as the future DOS personality.

The Windows portable bundle (`scripts/package_windows_portable.py`) belongs to
the 0.8 line until the new image passes F1; it must never contain local
commercial game payloads, and the Windows launcher is not runtime-tested on
Windows. On this clone, a local `pre-push` hook runs `scripts/push_release.py`
for each `origin/main` push (publication is suspended by
`config/release-policy.json`). Do not bypass that hook. Other clones must run
`bash scripts/install_release_push_hook.sh`. No GitHub Actions are used.

## Foundations transition (decided 2026-10-09)

Claude and Codex decided, on the owner's mandate, to move CiukiOS to a new
32-bit protected-mode kernel with Windows 95/98-class structure, native
32-bit processes, FAT32 and later NTFS read-only. The binding record is
`dev_diary/2026-10-09-06-decisione-fondamenta-32bit.md` (decisions D1–D11
and the F0 acceptance criteria). In short:

- Branch `legacy-0.8` (at `868cac9`) holds the 0.8 line; prerelease b849 is
  its preserved release. Only critical fixes go there.
- `config/release-policy.json` says whether main prereleases are published.
  It is `suspended` until the new image passes the F1 gate; the hook still
  runs and still requires a clean tree. Resuming publication needs a
  re-qualified Windows bundle for the new image.
- The 0.8 build/boot path on main is replaced atomically by a runnable F0
  scaffold. Reusable models, applications, tests, assets, licenses and DOS
  build dependencies stay until each is migrated or retired explicitly.
- No F0 code before the seven design contracts listed in D11 exist in
  `docs/design/` and have been reviewed by the other agent.
- Scope (owner decision 2026-10-09, dev diary 2026-10-09-10): a modern
  retro gaming OS, not a Windows clone; F0 foundations followed by eight
  steps, F1–F8 (drivers and disk, native programs, DOS, sound and media,
  native games, network, 3D, Windows games through Wine). The F2 native API is a POSIX
  subset with a ported libc; networking is step F6 with lwIP, a NIC driver
  compatibility layer and a virtual NE2000/IPX for DOS VMs. Each step's
  contract is written and cross-reviewed before its code.

## Research before implementation

Before changing CiukiOS code or architecture, research the relevant behavior
on the internet. Prefer original specifications, vendor manuals, upstream
source and official documentation. Check the actual implementation in this
repository against those sources, record the links and the resulting decision
in the relevant design or validation document, then implement and test it.
For DOS compatibility, distinguish Windows 95/98 V86 DOS sessions from the
unrelated Windows NT NTVDM architecture. Do not infer that the presence of
VMs is itself a compatibility defect. Measure available and reserved memory
on the supported hardware profile before changing memory limits.

## Build memory safety on this workstation

Run full image builds and other heavy jobs in a separate systemd user scope
with memory and swap caps, rather than as unrestricted children of VS Code.
On this 14 GiB Linux host, use:

```bash
systemd-run --user --scope -p MemoryMax=3G -p MemorySwapMax=1G -- make build-full
```

Check available host memory first. Run QEMU and full/CD builds sequentially.
If a capped build exceeds its limit, investigate its memory use before
changing the cap.

## Working method

Owner directive, 2026-10-09: Claude Code and OpenAI Codex work together on
this repository. Both read this file.

**Roles (owner directive, 2026-10-10).** Claude directs, Codex writes the
code. Claude owns the plan, the directives (specification, acceptance
criteria, interfaces, constraints), reviews, integration, QEMU and hardware
validation, commits and pushes, and reports disagreements between the two
agents to the owner instead of silently picking one. Codex implements every
coding task from a written directive, in its own worktree, with host tests;
it chooses nothing about scope. Claude writes code only for trivial glue or
an urgent fix it can verify at once, and records it. Codex cannot run QEMU or
systemd scopes from its sandbox: directives ask it for host-testable code and
Claude runs the QEMU evidence. Avoid extra agents when a task is too small or
cannot be split usefully.

**Codex models (checked 2026-10-09, `~/.codex/models_cache.json`).** Pick the
cheapest model and effort that fits:

| Model | Use | Effort |
| --- | --- | --- |
| `gpt-6-luna` | research, file searches, summaries, simple checks, small mechanical code changes | `low`–`medium` |
| `gpt-6.1-sol` | reviews of plans and diffs, ordinary implementation (drivers, tools, tests, runner) | `high`–`xhigh` |
| `gpt-6-astra` | hardest problems: architecture critique, deep debugging, kernel core, V86/DPMI, filesystem write paths | `xhigh`–`max` |

Claude picks the model and effort per task by its complexity and risk, and
states both in the directive.

`ultra` only when the owner asks for it. Re-check the cache when models change.

**How Claude calls Codex.** Prefer the CLI, which honours model and effort:

```bash
codex exec -m gpt-6.1-sol -c model_reasoning_effort=high \
  --sandbox read-only --ephemeral -o <scratch>/answer.md "<prompt>"
```

The `codex` MCP bridge (claude-codex-bridge 0.3.1) is fine for quick questions,
but it cannot set effort (it uses `~/.codex/config.toml`), its model list is
stale (omit `model`), it times out after 10 minutes, and it resumes one shared
Codex thread across calls without re-applying the sandbox.

**Writes.** Only one agent writes to the main working tree at a time. A
teammate implementation runs in its own git worktree
(`git worktree add ../CiukiOS-wt-<task> -b wt/<task>`), and the lead reviews
and merges it; remove the worktree and branch afterwards. The teammate never
commits to `main` or pushes.

**Cross-review.** Before an architecture decision or a substantial change, the
lead asks the teammate for a plan review; before committing a substantial diff,
for a code review. Resource rules still apply to both agents: one heavy build
or QEMU at a time, capped as described above.

## Development diary

Owner directive, recorded on 2026-10-09: every essential change, decision,
release or hardware analysis gets its own new file in `dev_diary/`, named
`YYYY-MM-DD-NN-short-title.md`, following the template and rules in
`dev_diary/README.md`. Add the entry to that README's index. Never rewrite
an earlier entry to change a decision; add a new entry that supersedes it.
Diary entries are written in Italian. `CHANGELOG.md` remains the release
record.
Entries record facts (what was done, verified, measured) and decisions with
their technical rationale, nothing else: no internal considerations,
conversation narration, personal logistics (purchases, cable availability,
plans for the day), personal configuration or details of private material.
Operating procedures belong in `docs/`. Removing such details from an
existing entry is allowed; changing its decisions is not.

## Legacy archive

On 2026-10-09 the owner archived the old documentation and scripts:

- `legacy/CiukiOS-docs-legacy-2026-10-09.zip`: the former `docs/` tree
  (design notes, validation records, history, old logbook).
- `legacy/CiukiOS-scripts-legacy-2026-10-09.zip`: about 370 old test, probe,
  diagnosis, floppy and macOS scripts plus the old Makefile.
- `legacy/local/` (untracked, never publish): physical-PC logs, old handoffs,
  private media and compressed backups of original disk prefixes.

Extract a single file from these archives when an old decision, marker or
QMP sequence is needed. Do not restore archived files into the tree; rewrite
what is still useful for the current structure. New design and validation
documents go in `docs/design/` and `docs/validation/`.

## Repository hygiene and resources

The owner requires a clean tree and economical use of this workstation.

- `scripts/` holds only what the build, run, release and physical-log
  collection use. One-off diagnosis scripts are not committed; keep them in
  the session scratchpad and delete them when the investigation ends.
- `build/` keeps only `external/`, `downloads/`, `tools/`, `releases/`
  (current release only) and the current `build/full` output. Never leave
  dated variant directories, copied images, RAM dumps, `.ppm` captures or
  hardware dumps there. Delete investigation artifacts when the
  investigation closes; save the evidence that matters as a short text
  record in `docs/validation/` or in a `dev_diary/` entry.
- Never put large files in `/tmp`: it is a 7.4 GiB tmpfs (RAM).
- Raw dumps of physical disks are not kept, except an explicitly requested
  backup of original data, stored compressed in `legacy/local/`.
- Do not add files to the repository root.

## Testing

Follow `docs/design/test-architecture.md`:

- One canonical build per source state (`make build-full`, plus
  `make build-full-cd` when the CD matters). Tests never rebuild, never use
  build-time variant flags and never copy whole images. A build variant
  needs the owner's approval and a `dev_diary/` entry.
- Test behaviour is selected at run time (QEMU fw_cfg item
  `opt/it.alcybercloud.ciukios/test`), and every run uses a qcow2
  copy-on-write overlay under `build/test-runs/`, deleted when the run passes.
- One QEMU at a time, each under
  `systemd-run --user --scope -p MemoryMax=1500M -p MemorySwapMax=0`; check
  free memory first, no RAM dumps unless a diagnosis needs them, and no
  `qemu-system` process left running. Never rebuild `build/f0/ciukios.img`
  while a runner suite is using it (overlays back onto it and its hash is
  part of the evidence), and do not run the host runner tests
  (`tests/host`) while a suite runs: they share the common lock.
- QEMU TCG is not a Pentium III: known deviations are recorded in
  `docs/validation/2026-10-09-f0/README.md` (for example FLD m64 is rounded
  to the x87 precision-control setting under TCG, not on real hardware).
- Cheapest tier first: host unit tests, then static image checks, then the
  `make qemu-test-full` boot smoke, then focused QEMU suites, then hardware.
- A result is evidence only for the image SHA-256 it ran on.

## Code Search Policy

Use Semble before reading large files.

For code discovery, prefer:

```bash
semble search "describe what you need" .
semble search "symbol_or_label_name" .
semble find-related path/to/file line .

If semble is not available on PATH, use:

uvx --from "semble[mcp]" semble search "describe what you need" .
uvx --from "semble[mcp]" semble find-related path/to/file line .

Rules:

Use semble search before opening large files such as src/boot/floppy_stage1.asm.
Open only the returned file ranges or nearby small ranges.
Use rg only for exact literal confirmation.
Do not read entire large files unless strictly required.
Do not run broad repository scans.
For CiukiOS, always search first for labels/functions before opening floppy_stage1.asm.

Recommended searches:

semble search "int21 rmdir parent sector read write ES corruption" .
semble search "int21 mkdir directory cluster zero fill" .
semble search "SHELL.COM command parser builtins" .
semble search "read_sector_lba32 write_sector_lba32 FAT16" .
semble search "qemu shell com validation markers" .
semble search "external command execution INT21 AH 4B" .

## 4. Rendi disponibile il comando `semble`

Per i subagent è meglio avere anche il comando shell disponibile.

Opzione A, con `uv tool`:

```bash
uv tool install "semble[mcp]"

```
