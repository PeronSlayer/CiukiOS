# Diablo shareware: acceptance runner dry run — 2026-10-20

**Dry run; QEMU run pending by the lead.** This is authored canned telemetry
and synthetic solid-colour frames. It proves runner/oracle/parser behavior;
it is not evidence of Diablo execution, installation, audio or compatibility.

| Identity | Canned value |
| --- | --- |
| Workload / scene | diablo-shareware / default-play |
| Canned edition / original edition | authored-canned-fixture / unknown; original qualification pending |
| Image SHA-256 | 64 lowercase `a` characters (synthetic identity) |
| Payload SHA-256 | 64 lowercase `b` characters (synthetic identity) |
| Run ID | 1234abcd |
| Install inventory | authored GAME.EXE, 12 bytes, SHA-256 of 64 `c` characters (synthetic field) |
| Reference frames | generated 320x300 RGB solid colours, 2x2 checked regions |
| Original provenance | manifest points to DIABLOSW.iso, 137576448 bytes, SHA-1 bf1a62b24ce01ce39993955bff4b3c4d4ab9d647; no archive inspected/downloaded here |
| Licence / archive SHA-256 | shareware designation; terms, distribution verification and reviewed digest pending |
| Verification entry point | tests/host/test_runner_win32.py |

| Phase | Canned observation | Model verdict |
| --- | --- | --- |
| Install tier | begin/ARM; installer process start/exit; default choices and zero system writes; decoded file inventory compared with authored reference; installer and desktop region matches; kernel start/exit | pass |
| Title start | process 7; fixture-tree launch and decoded GAME.EXE path; ordered map/imports/TLS/entry; menu region | pass |
| Play | 180000 guest ms, 5400 game frames, 10800 presents; 30000 milli-FPS average/median, 26000 p05; 38000 us p95; 5400 samples; stream 1 at 44100 Hz stereo | pass |
| Audio | 7938000 consumed PCM frames, 7000000 non-silent; zero underruns; independent host-reference boolean supplied by the canned harness | model only |
| Input / progress | measured input ID 2; 10000 us and one frame guest latency; ordered counters 1 then 2 and distinct checked-region hashes; QMP receipts | pass |
| Focus | ten round trips, corrupt=0; synthetic focus frame | pass |
| Exit | code 0; desktop frame; all six pre/post ledgers zero, post-exit 50 guest ms | pass |
| Uninstall | not included in these tier fixtures; whole-title conditional gate remains pending | not_run |

The frame/record driver also exercises timeout and wrong-scene failures for
both tiers; repeated presents with unchanged checked-region pixels fail despite
increasing guest progress counters. The installer integration test feeds serial
records and frames through the real collector, scope-verification gate and
cleanup using fake_qemu.py (Python FIFO/QMP fixture, no emulator/systemd).
It checks passing retention is exactly result.json and serial.log. A separate
preflight test checks no overlay or launch occurs for pending references/probes.

The parsed records and evidence are exported by `nice -n 19 python3 -B tests/host/test_runner_win32.py --dry-run-report build/codex/f5-17b-dry-run.json`.

The canned installer and title sessions pass their respective model predicates.
The **original-title gate is not_run**. Both live suite cases, their input
scripts and hardware operator script remain authored for lead qualification.
The live probes are unregistered; original paths, edition, file/registry/link
references, screen pins, audio capture window and guest driver depend on f5-15B
and the API owners. Separate cold-boot fixtures do not prove persistence or
Start-menu launch after the original installer. The final acceptance census is
incomplete because no original executables or runtime import traces are present.
