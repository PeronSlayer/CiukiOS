# F1-120 host evidence

Recorded 2026-10-23 on `wt/f1-120`, base
`5e430f810872782db6134e70fca331b97ed8ef79` plus this uncommitted change.
Source pins and implementation decisions are in
[the design record](../design/f1-120-audio-engines.md).
No QEMU, systemd scope, image build, hardware trial or image hash is claimed.

`nice -n 19 stdbuf -oL -eL bash tests/host/audio/run.sh` compiles with the
repository's ASan/UBSan flags and reaches **11,419,300 functional checks PASS**.
The process then exits 1 because LeakSanitizer cannot perform its inspection
in this sandbox. Its message identifies the ptrace/ptrace_scope restriction.
Leak detection was not disabled or suppressed. This is functional evidence,
not a green sanitizer suite. The earlier fixture defects found while writing
the tests were corrected before this final run.

The new models cover DX, NX and M5451 revisions 1/2, voice packing and
stereo frames, bounded posted codec IO, revision-2 read alias/write bit,
one aggregate 100-ms transaction budget across pre-command and completion waits,
silent progress before readiness, PCM wrap, shared IRQ source ownership,
virtual SB/OPL mixer publication and recovery, copied full-duplex capture,
generation rejection, allocation/claim refusal, stale cursor/overrun,
record mixer restoration, 30-/31-bit directional pins, failed stop/unpin
retention, permanent quarantine and refusal to restart or change the mixer.
ALi M1533/M1563 route tests exhaust the 16 link and 16 nibble values, plus
zero/out-of-range refusal, without route writes.

The original audio script stops after its first failing process. Its remaining
sections were therefore executed sequentially with their original compile
flags, using the session helper under `build/codex/`:

| Check | Result |
| --- | --- |
| Production VM recovery (32 fresh 64-unit blocks), late-event and arithmetic sections | Functional assertions PASS; each sanitized process exits 1 at the same LeakSanitizer sandbox restriction |
| Original optimized audio cost and VM cost sections | Exit 0 |
| Original i686 VM/native/OPL and SB16 code-generation section | Exit 0, no wide helpers in guarded paths, 4-KiB frame bound |
| Five changed/new production C units with `scripts/build_kernel.py:CFLAGS` and its opcode audit | Exit 0; no FPU/SIMD violations |
| `nice -n 19 env PYTHONPATH=tests/host python3 -m unittest test_audio` | 10 tests PASS, exit 0 |
| `nice -n 19 python3 -m unittest discover -s tests/host -p test_credits.py` | 8 tests PASS, exit 0 |
| `nice -n 19 env PYTHONPATH=tests/host python3 -m unittest test_devices.DeviceTests.test_registry_query` | PASS, exit 0 |
| Broader Device Manager suite | 3 tests pass; 2 cannot run because the pinned FreeType archive is absent |
| Broad recursive `test_audio.py` discovery | 16 tests pass; unrelated Win32 PE fixture test lacks its pinned NSIS source archive |
| `scripts/build_image.py` Python compilation, donor Git blob checks and whitespace check | PASS |

The exact commands, exit results, source links and outstanding work are in
`build/codex/f1-120-report.md`; local logs and reproducible continuation
commands remain alongside it. No missing dependency was downloaded as a
substitute for the repository's pinned archive.

The lead still needs a sanitizer-capable host rerun, its canonical integration
build and actual DX/NX/M5451 playback/capture, routing, stop, analog and latency
qualification. Firmware-ready AC-link and disabled/unclaimed legacy functions
are required; unsupported firmware state is explicitly refused. A general
bridge/PM reset takeover or public recording ABI needs a further directive.
