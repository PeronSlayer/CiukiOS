# f2-123 — Complete hardware names: INF literal descriptions and the PCI/USB ID databases

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning medium, `--search`. **Worktree:** `wt/f2-123` from `main`.
- **Source:** `docs/design/video-driver-families.md`, section 2 and lead decision 4. `scripts/build_hwids.py:75–97` needs a `%description%` token on the left side of an INF model line, so models with literal descriptions are missing; 202 display model ids are absent, including every ATI Radeon in the pack. The owner wants every common device of a retro PC named in Device Manager and Control Panel.

## Required

1. **INF extraction.** `scripts/build_hwids.py` also accepts literal and quoted descriptions on model lines, with the same INF parsing rules (string sections, decorations, UTF-16). Host tests use synthetic INF fixtures: literal, quoted, token and mixed lines, and duplicates with a deterministic winner.
2. **Second name source.** Add the public PCI and USB ID databases, `pci.ids` and `usb.ids` from https://pci-ids.ucw.cz and http://www.linux-usb.org/usb.ids.
   - Pin the snapshot by date and SHA-256 under `build/downloads/` (never committed) and record it in the generator.
   - Credit them in the notices.
   - INF names win and the databases fill the gaps.
   - Limit the output to device classes a 1998–2004 PC can have: display, multimedia, network, storage, bridges, serial bus/USB, input, communication.
3. **Size.** Measure the resulting JSON. If it exceeds `MAX_JSON` (400 KiB), propose a compact encoding, such as one gzip file or a sorted binary table with a loader change in `apps/control/main.c`. Do not raise the limit without measuring the memory the Control Panel uses to load the file.
4. **Tests.** Existing hardware-ID tests stay green. New tests check that the ATI Radeon, Rage, Matrox, Intel, 3dfx, S3, SiS, Trident, Cirrus, NeoMagic and VIA display ids of `video-driver-families.md` resolve to names.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f2-123-report.md`.
