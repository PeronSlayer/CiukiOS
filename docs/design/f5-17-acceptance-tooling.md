# F5-17A host acceptance tooling — 2026-10-14

The research and scope decisions are appended to the Win32 personality
contract's review area (§7). These tools implement a host model only. Existing
`run.py`, loader selectors, suite declarations and kernel probes are unchanged.
Integration is F5-17B after the relevant loader/installer/media/input APIs.

The workload manifest contains the ten approved titles and four reserves.
`source.size`/`sha1` are the contract's archived metadata observations, not new
measurements. Nulls explicitly mean unknown; `unknowns` explains pending import
work. `licence.class` labels a demo/shareware/local corpus and
`distribution_verified=false` means redistribution has not been established.
The source URL is not a licence grant. Owner-local rows have only a local path
pattern and cannot be downloaded. GTA2 remains conservatively local pending
exact-edition/terms verification. Self-authored fixtures are a separate corpus;
no rebuilt game engine or Wine conformance binary closes an original-title gate.

Run the manifest validator without downloads:

```sh
nice -n 19 python3 -B scripts/win32/workloads.py
```

After licence/edition discovery, a lead-requested `--fetch <id>` requires size
and SHA-256 pins. `--import-unpinned --fetch <id>` instead verifies the
contract's size/SHA-1 and writes a `candidate-unreviewed` SHA-256 receipt for
lead review. It never modifies the trusted manifest. Both modes stream once
to `build/downloads/win32/<id>/<file>`; cached files are reverified, partial
failures are removed, and no payload enters a canonical image or release.
Milestone A does not download original title archives. Silent/scripted switches
remain null unless their exact installer edition's documentation is linked.
Installed-file lists become `verified` only after an original install, with
names/sizes and preferably hashes; archive extraction is discovery evidence.

Build directory fixtures with the Python standard library:

```sh
nice -n 19 python3 -B scripts/test/iso_fixtures.py --source <directory> --out build/codex/media --joliet
nice -n 19 python3 -B scripts/test/iso_fixtures.py --source <directory> --out build/codex/audio-media --audio-seconds 4
```

The API is `iso_fixtures.generate_directory(source, output, joliet=True)` and
`generate_mixed_mode(source, output_directory, audio_seconds=4)`. It returns
manifest entries with readable paths, UTF-8 path hex, types and file hashes.
Primary names are stable level-1 aliases; Joliet preserves supported UCS-2
names (64-character directory identifiers; file names at most 62 characters
plus the `;1` version suffix). Reject names with control/surrogate/non-BMP
characters, Windows case collisions, semicolons, trailing dots/spaces and
primary-alias collisions; reject symlinks/special files, directory depth >8
(counting root as level 1) and files
requiring multi-extents. Both descriptor hierarchies have ordered L/M path
tables and block-padded directories with records that never cross sectors.
Dates, order and padding are fixed; input timestamps/modes do not affect bytes.

Mixed-mode output is `disc.cue`, `data.iso` (MODE1/2048) and `audio.bin`:
track 1 is data; track 2 is authored S16LE stereo 44.1 kHz audio, with a
150-sector silent INDEX 00 pregap and INDEX 01 at 00:02:00 in its own file.
One audio sector is 588 stereo sample frames, 2352 bytes, 1/75 s. The manifest
records track-relative indices mapped to disc LBAs and the lead-out. The
logical data track is at least 300 sectors; the non-silent audio lasts 4–60 s.
No raw-sector ECC is claimed for cooked data. ISO mounting tests prove only
the data hierarchy; a lead-qualified track-aware transport, TOC/audio commands
and hardware burn/readback are still required for MCI cdaudio.

## Session format and clock domains

`win32_session.selector()` accepts only a validated runtime fw_cfg request:
`f5:win32-<workload-id> run=<8-lowercase-hex>` at the existing
`opt/it.alcybercloud.ciukios/test` item. It does not add guest support for it.
Each selected title has a separate runner case/session/overlay and a 600 s
host deadline; no rebuilding or combined ten-title session.

A QMP script is a JSON object with `schema_version=1`, `workload`, `scene`,
`clock=host-monotonic-ms`, `timeout_ms<=600000` and ordered `segments`.
Each segment has a `name` from install/start/play/focus/exit/uninstall and
`steps`. A step contains a globally unique positive `input_id`, an `at_ms`
offset from its segment ARM and a QAPI `events` batch. Events are qcode key
press/release, relative x/y motion or left/middle/right button press/release.
Times strictly increase, at most 64 events per batch and 4096 steps per
segment. Every held key/button is released within that segment. Actual qcode
availability remains a QMP/profile check; a rejected command never advances
the script.

`InputPlayer.observe(record, host_ms)` consumes already validated records.
The caller supplies `started_at_ms` from host launch, so the whole-case
deadline also applies before any guest BEGIN. `ARM segment=<name> action=script`
anchors that segment's events. `tick(host_ms, qmp)` dispatches one due batch,
records its input ID, planned/actual host time and QMP acknowledgement.
Delayed polling preserves scheduled minimum gaps between batches rather than
compressing key holds into a burst. There are no blocking sleeps. Acknowledged
input does not prove delivery, latency or visible response. Guest elapsed ms
and guest icount are separate serial fields and are never converted to host
seconds; TCG icount is not hardware speed.

`screen_check((width,height,rgb_bytes), check, reference)` accepts a rectangle
`[x,y,w,h]`, a pinned region SHA-256 and `mode=sha256` or `mode=tolerance`.
Tolerance additionally requires a reference frame whose region matches the
pin, `channel_delta` (0–255) and `max_bad_pixels` strictly less than the
region's pixel count. Compare channels per pixel, not a whole-screen average.
The caller can use existing `run.read_ppm()` for QMP P6 captures. Choose
stable menu/scene cells outside dynamic cursor/clock areas. Original reference
runs must qualify all coordinates and pins. Screen similarity alone is not
proof of gameplay or focus restoration.

## Proposed serial wire schema

`win32_evidence.Parser` retains the existing bounded `CIUKI_TEST` style:
ASCII key=value tokens, at most 240 bytes excluding CR/LF, no duplicate keys,
version 1, strictly increasing six-digit sequence, exact run/probe, one BEGIN,
no records after a terminal event, at most 16384 records. Non-record serial
banners are ignored; malformed marker prefixes fail. It is intentionally
separate from existing suite parsers; the draft is not a frozen guest ABI.

Envelope: `v=1 run=<8-hex> seq=<6-digits> probe=win32-<id> event=<event>`.
BEGIN contains `image_sha256`; a following DATA identity contains
`payload_sha256` and `scene`. Split these hashes across records to fit UART
line bounds even for long workload IDs. Identity must precede segments.

| DATA case | Required fields after `case` |
| --- | --- |
| identity | payload_sha256, scene |
| segment | segment, op=begin/end, guest_ms, icount |
| process | segment, pid, op=start/exit, guest_ms; exit also has signed code |
| loader | segment, pid, stage=map/imports/tls/entry, status=PASS/FAIL |
| mode | segment, pid, width, height, bpp=8/16/24/32 |
| frames | segment, pid, duration_ms, game_frames, presents |
| fps | segment, pid, avg_milli, median_milli, p05_milli, p95_us, samples |
| audio | segment, pid, qualified=0/1, underruns, consumed_frames, non_silent_frames |
| input | segment, pid, input_id, latency_us, latency_frames |
| progress | segment, pid, checkpoint, counter |
| focus | segment, pid, round_trips, corrupt=0/1 |
| ledger | segment, when=pre/post, kind, count, guest_ms |
| install | segment, default_choices, system_writes, menu_entry, uninstall_registered |
| uninstall | segment, key_removed, menu_removed |
| commit | segment, pid, peak_bytes |
| shim | segment, pid, name |
| stub | segment, pid, name, handled=0/1 |
| crash | segment, pid, code |

Boolean values are integral 0/1. Other numerical measurements are unsigned
64-bit integers; fps is thousandths, latency/frame time is microseconds,
commit is bytes. Guest ms/icount at segment boundaries must not decrease.
Each process starts before its data and exits once; map/imports/tls/entry
loader records are ordered. Play telemetry belongs to the same process as the
start loader. Emit one aggregate frames/fps/audio/mode/commit summary for the
play interval, plus distinct input and at least two progress observations.
Record percentile sample count and independently describe the sample source
in evidence: percentiles refer to new game frames, not presenter refreshes.
Median and 5th percentile fps use instantaneous 1/frame-time samples; p95_us
uses the same frame-time samples. Average is game_frames/duration, checked
against counters; frame-time sampling itself needs guest/reference review.
Ledger kinds are process/handle/section/stream/channel/commit (commit count in
bytes); record one baseline before process start and one after exit for each.

ARM carries segment/action. END carries status=PASS/FAIL/NOT_RUN (NOT_RUN
requires reason). ERROR/PANIC carry reason; NOT_RUN before BEGIN carries
reason and closes an absent fixture/prerequisite run. A PASS with an open
segment is malformed. Fault/stub/loader failure cannot be hidden by END PASS.

`Parser.verdict(host_checks, focus=..., uninstall=..., timeout=...,
hardware=..., policy=...)` requires all relevant segments and guest summaries
plus independent installed-files, menu/play/desktop-screen, progress, reference
sound and acknowledged-input checks; focus/uninstall require their additional
checks. `host_checks.input_ids` lists the unique latency-measured IDs from the
reference script, confirmed in QMP receipts/operator observation, and must
match the guest latency records. It is not a list of every release/motion batch.
Missing evidence is `not_run`; an explicit failed check, timeout, crash or
unhandled stub is `fail`. Duplicate summaries are failures. The guest END PASS
alone is insufficient. Verdicts always remain bound to parser image/payload/
scene; retain the operator/script and reference hashes in the evidence record.

Start has a 120 s deadline, play a 180 s minimum, selected focus ten round
trips, exit code must be documented and baseline ledgers restored within 2 s.
Unknown/unqualified audio never passes. Hardware additionally needs an
explicit lead-approved policy object: `play_ms`, `min_fps_milli`,
`max_underruns`, `max_latency_us`, `max_latency_frames`. This is not approval
to lower Q7's thresholds; it makes any lead disposition visible in evidence.
Ten-minute hardware play versus the whole-case ceiling remains unresolved.

The validation template is `docs/validation/f5-title-gate-template.md`; proposed
per-title hardware operator scripts are under `tests/win32/operators/`.
Their actions are scene proposals, not tested menu coordinates or silent
installer sequences. The lead qualifies these on original reference editions,
resolves timing/2D-fps policy and approves Diablo/StarCraft/RCT focus coverage
before F5-17B can execute or close any title gate.
