# Directive f3-06: reference DOS workloads (DOS/4GW probe, DOOM, Wolf3D) and the F3 workload gate

- **Step:** F3 (closing directive). **Contracts:** `dos-dpmi-contract.md`
  (acceptance row "F3 workloads": pinned DOS/4GW probe and original DOS
  DOOM complete launch/demo/exit loops; Wolf3D exercises real-mode VGA/
  input/UMB; a background CLI-loop VM cannot starve audio/input; PCM/OPL
  counters checked separately; workload records include executable/
  extender/data hashes and redistribution status; private commercial
  payloads stay out of release images), `test-architecture.md`,
  `foundations-transition.md` (license rules; `third_party/Doom/` and
  `third_party/WOLF3D/` are local, ignored by git, never published),
  directives f3-00…f3-05.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f3-workloads`, from `main` after f3-00…f3-05 are
  merged. Files: `src/probes/dos/` DOS-side payloads and the DOS/4GW probe
  (`src/probes/vm/dpmi_*` rebuilt with the existing OpenWatcom recipe),
  `scripts/build_dos_image.py` (payload inclusion with hashes and
  redistribution status; local commercial payloads only when present on
  the host and never into the system image: they are placed on a separate
  fixture disk by the runner), `scripts/test/run.py` and new
  `scripts/test/dos_fixtures.py` (fixture generator building a FAT16 disk
  image from a declared local directory, with hashes recorded and
  `not_run reason=fixtures_absent` when the directory is missing),
  `src/kernel/probes/f3_probes_workloads.c`, `tests/suites/f3-workloads.json`,
  host tests, `docs/design/f3-acceptance.md` (`## f3-06`),
  `docs/build-and-run.md` (how the owner declares local game directories).

## Scope

1. **DOS/4GW probe.** The pinned probe (hashes of the executable, the
   extender and its data) boots in a VM from the private disk, selects
   Ciuki VMM as DPMI host, runs its interrupt, port, lifetime and fault
   tests and exits with a status recorded through the record port; 20
   consecutive launch/exit loops with page/handle/device baselines
   restored.
2. **Original DOOM (local payload).** When `third_party/Doom/` is present
   on the host, the runner builds a fixture disk with `DOOM.EXE`, its
   DOS/4GW and `DOOM.WAD`/`DOOM1.WAD`; the VM runs `DOOM -nosound
   -timedemo demo1` (or the shortest built-in demo) to completion and
   exits; the probe records launch time, frames/ticks reported through the
   screen (text-cell buffer of f3-00 or the VGA digest of f3-02), the
   demo exit status and the counters of f3-05 (PCM blocks/OPL events
   produced, zero audible output in F3); 5 launch/demo/exit loops. Then
   with sound enabled (`-nosound` removed) the same loop checks the SB16/
   DMA/IRQ counters of f3-05 (digital evidence only). Absent payload →
   `not_run reason=fixtures_absent`.
3. **Wolf3D (local payload).** When `third_party/WOLF3D/` is present:
   boot to the title screen, start a game through scripted keyboard
   input, verify the Mode X VGA digest and the UMB/XMS usage records,
   exit; 5 loops.
4. **Starvation gate.** A second VM runs a CLI loop (`cli; jmp $`) while
   the first runs the DOS/4GW probe: the first VM's timer and keyboard
   events and the native heartbeat keep their rates (recorded), host
   preemption unaffected.
5. **Evidence rules.** Every workload record includes the image/build
   identity, VM admission arithmetic, executable/extender/data SHA-256
   and `redistribution=local-only|free`; the runner refuses to copy a
   local commercial payload into `build/f0/ciukios.img` or any release.

## Acceptance by the lead

Host tests; kernel and image build; QEMU `f3-workloads` PASS with the
owner's local payloads present on this workstation and `not_run` on a
clean checkout; `f2-all` and `f3-vm` unaffected; an owner-visible DOOM
window on the desktop (`/bin/dosbox`) as the F3 closing evidence.
