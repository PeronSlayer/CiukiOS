# F5 title gate evidence template

Status: **not_run** until every required observation is present. This template
is for one title/edition, one machine and one canonical image; F5-17A host
fixtures do not fill in original-title evidence. Copy it to a dated validation
record and retain private payloads and raw hardware logs only in `legacy/local/`.

| Identity | Value |
| --- | --- |
| Run ID / probe / segment IDs | pending |
| Tier / machine / CPU / RAM / audio backend | T3 or T4; pending |
| Canonical image SHA-256 / build manifest | pending |
| Workload ID / title / exact version | pending |
| Original payload size / SHA-1 / SHA-256 / provenance URL | pending |
| Corpus / licence class / bundled terms digest | pending |
| Operator or QMP script SHA-256 / reference qualification | pending |
| Gate scene / map / save / renderer / resolution / bpp | pending |
| Disc image SHA-256 / track table / music path | none or pending |
| Shims applied / peak commit bytes / memory headroom | pending |
| Host monotonic timing / guest elapsed timing / icount | separately recorded |

Record install, start, play, focus, exit and conditional uninstall separately;
all share the same image, payload and scene identity. Record each segment's
begin/end guest milliseconds and icount plus its host start/end time. A 600 s
host deadline covers the entire case. An incomplete run cannot inherit a PASS
from a different image or edition. Each title gets its own overlay and session;
use the canonical runner resource/locking/retention rules at integration.

| Segment | Required evidence | Verdict / reason |
| --- | --- | --- |
| Install | Original entry point/default choices; actual files with sizes/digests versus reference; menu entry/Uninstall key if registered; zero `C:\System` writes | not_run |
| Start | Start-menu launch, process and loader stages, qualified main-menu region within 120 s | not_run |
| Play | Exact actions and scene, >=180 s with progress, non-silent consumed PCM and reference audio, no crash/unhandled stub | not_run |
| Focus (selected three) | Ten round trips, desktop and game screenshots, mode/palette restoration | not_run or not_applicable |
| Exit | Own Quit, documented exit code, desktop mode, baseline ledgers restored in <=2 s | not_run |
| Uninstall (if registered) | Original uninstaller, menu/key/file removal evidence | not_run or not_applicable |

| Play measurement | Value / units / observation method |
| --- | --- |
| Guest play elapsed / host elapsed | pending ms / ms |
| New game frames / display presents | pending / pending; repeated presents do not count as game progress |
| Average / median / 5th percentile game fps | pending fps; average = game frames / guest duration |
| 95th percentile frame time | pending ms; sample source/count/window required |
| Consumed / non-silent PCM frames / qualified backend | pending / pending / pending |
| Audio underruns | pending count over exact play interval |
| Input IDs / host QMP acknowledgement times | pending; acknowledgement is not guest response |
| Input latency | pending ms and game frames, input-to-visible-response observation method |
| Progress checkpoints | At least two ordered scene-specific counters plus independent visual/operator evidence |
| Resource ledgers | process, handle, section, stream, channel, commit; pre-start and post-exit count/time |

Screenshots: list relative evidence path, SHA-256, segment, host time, guest
time, dimensions, region `[x,y,w,h]`, pinned reference-region SHA-256, exact
or tolerance mode, channel delta, maximum bad pixels and actual bad pixels.
Use region checks to avoid cursor/clock noise; golden regions are qualified
from original reference runs, not derived from the run being judged. Retention
exceptions for screenshots needed by the gate require lead integration with
the runner's passing-run cleanup policy.

Verdict: **pass / fail / not_run** with failed and missing checks listed.
Timeout, explicit fault, unhandled stub or failed screen/operator check is
`fail`; missing identity, audio qualification, metrics or reference checks is
`not_run`. A guest END PASS alone never closes a title gate. E500 unqualified
audio remains `not_run`. Hardware playable thresholds require an explicitly
recorded lead disposition: Q7 specifies average >=25 fps, zero underruns,
input response within one game frame and ten minutes; the whole-case ceiling
is also ten minutes. Do not silently shorten play or extend a run.

The per-title JSON format is `tests/win32/operators/<workload-id>.json`:
`schema_version`, `kind=hardware-operator`, workload/scene, nullable digest
bindings, serial settings, case ceiling, reference prerequisites, nullable
threshold policy and ordered segment steps. A step carries an action,
required evidence and optional minimum duration, deadline, round trips or
condition. These proposed scripts must acquire exact edition/menu/map/input
and screenshot references before hardware execution can yield a PASS.
